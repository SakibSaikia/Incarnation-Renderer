namespace RenderJob
{
	struct VisibilityPassDesc
	{
		FShaderSurface* visBufferTarget;
		FShaderSurface* depthStencilTarget;
		DXGI_FORMAT visBufferFormat;
		uint32_t resX;
		uint32_t resY;
		const FScene* scene;
		const FView* view;
		Vector2 jitter;
		FConfig renderConfig;
	};

	concurrency::task<void> VisibilityPass(RenderJob::Sync& jobSync, const VisibilityPassDesc& passDesc)
	{
		size_t renderToken = jobSync.GetToken();
		size_t visBufferTransitionToken = passDesc.visBufferTarget->m_resource->GetTransitionToken();
		size_t depthStencilTransitionToken = passDesc.depthStencilTarget->m_resource->GetTransitionToken();

		return concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_visibility_pass", PIX_COLOR_DEFAULT);

			FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			cmdList->SetName(L"visibility_pass_job");

			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

			SCOPED_COMMAND_LIST_EVENT(cmdList, "visibility_pass", 0);

			passDesc.visBufferTarget->m_resource->Transition(cmdList, visBufferTransitionToken, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
			passDesc.depthStencilTarget->m_resource->Transition(cmdList, depthStencilTransitionToken, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE);

			// Descriptor heaps need to be set before setting the root signature when using HLSL Dynamic Resources
			// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html
			D3DDescriptorHeap_t* descriptorHeaps[] =
			{
				RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
				RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
			};
			d3dCmdList->SetDescriptorHeaps(2, descriptorHeaps);

			// Root Signature
			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({ L"geo-raster/visibility-pass.hlsl", L"rootsig", L"rootsig_1_1" });
			d3dCmdList->SetGraphicsRootSignature(rootsig.get());

			// Frame constant buffer
			struct FrameCbLayout
			{
				Matrix sceneRotation;
				int sceneMeshAccessorsIndex;
				int sceneMeshBufferViewsIndex;
				int scenePrimitivesIndex;
			};

			std::unique_ptr<FTransientBuffer> frameCb = RenderBackend12::CreateTransientBuffer(
				L"frame_cb",
				sizeof(FrameCbLayout),
				cmdList,
				[passDesc](uint8_t* pDest)
				{
					auto cbDest = reinterpret_cast<FrameCbLayout*>(pDest);
					cbDest->sceneRotation = passDesc.scene->m_rootTransform;
					cbDest->sceneMeshAccessorsIndex = passDesc.scene->m_packedMeshAccessors->m_srvIndex;
					cbDest->sceneMeshBufferViewsIndex = passDesc.scene->m_packedMeshBufferViews->m_srvIndex;
					cbDest->scenePrimitivesIndex = passDesc.scene->m_packedPrimitives->m_srvIndex;
				});

			d3dCmdList->SetGraphicsRootConstantBufferView(2, frameCb->m_resource->m_d3dResource->GetGPUVirtualAddress());

			// View constant buffer
			struct ViewCbLayout
			{
				Matrix viewTransform;
				Matrix projectionTransform;
				Vector3 eyePos;
			};

			std::unique_ptr<FTransientBuffer> viewCb = RenderBackend12::CreateTransientBuffer(
				L"view_cb",
				sizeof(ViewCbLayout),
				cmdList,
				[passDesc](uint8_t* pDest)
				{
					auto cbDest = reinterpret_cast<ViewCbLayout*>(pDest);
					cbDest->viewTransform = passDesc.view->m_viewTransform;
					cbDest->projectionTransform = passDesc.view->m_projectionTransform * Matrix::CreateTranslation(passDesc.jitter.x, passDesc.jitter.y, 0.f);
				});

			d3dCmdList->SetGraphicsRootConstantBufferView(1, viewCb->m_resource->m_d3dResource->GetGPUVirtualAddress());

			D3D12_VIEWPORT viewport{ 0.f, 0.f, (float)passDesc.resX, (float)passDesc.resY, 0.f, 1.f };
			D3D12_RECT screenRect{ 0, 0, (LONG)passDesc.resX, (LONG)passDesc.resY };
			d3dCmdList->RSSetViewports(1, &viewport);
			d3dCmdList->RSSetScissorRects(1, &screenRect);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passDesc.visBufferTarget->m_renderTextureIndices[0]) };
			D3D12_CPU_DESCRIPTOR_HANDLE dsv = RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, passDesc.depthStencilTarget->m_renderTextureIndices[0]);
			d3dCmdList->OMSetRenderTargets(1, rtvs, FALSE, &dsv);

			// Clear to max object ID (see encoding.hlsli). G-Buffer pass skips decoding if it encouters this value.
			// Float clear values are converted to integers if RT format is int/uint. 
			// See https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#ClearView
			uint32_t clearValue = 0xFFFE0000;
			float clearColor[] = { clearValue, clearValue, clearValue, clearValue };
			d3dCmdList->ClearRenderTargetView(rtvs[0], clearColor, 0, nullptr);
			d3dCmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.f, 0, 0, nullptr);

			// Issue scene draws
			uint32_t objectId = 0;
			for (int meshIndex = 0; meshIndex < passDesc.scene->m_sceneMeshes.m_entityList.size(); ++meshIndex)
			{
				const FMesh& mesh = passDesc.scene->m_sceneMeshes.m_entityList[meshIndex];
				SCOPED_COMMAND_LIST_EVENT(cmdList, passDesc.scene->m_sceneMeshes.m_entityNames[meshIndex].c_str(), 0);

				for(int primIndex = 0; primIndex < mesh.m_primitives.size(); ++primIndex, ++objectId)
				{
					const FMeshPrimitive& primitive = mesh.m_primitives[primIndex];
					d3dCmdList->IASetPrimitiveTopology(primitive.m_topology);

					// PSO
					D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
					psoDesc.NodeMask = 1;
					psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
					psoDesc.pRootSignature = rootsig.get();
					psoDesc.SampleMask = UINT_MAX;
					psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
					psoDesc.NumRenderTargets = 1;
					psoDesc.RTVFormats[0] = passDesc.visBufferFormat;
					psoDesc.SampleDesc.Count = 1;
					psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

					// PSO - Shaders
					{
						D3D12_SHADER_BYTECODE& vs = psoDesc.VS;
						D3D12_SHADER_BYTECODE& ps = psoDesc.PS;

						IDxcBlob* vsBlob = RenderBackend12::CacheShader({ L"geo-raster/visibility-pass.hlsl", L"vs_main", L"" , L"vs_6_6" });
						IDxcBlob* psBlob = RenderBackend12::CacheShader({ L"geo-raster/visibility-pass.hlsl", L"ps_main", L"" , L"ps_6_6"});

						vs.pShaderBytecode = vsBlob->GetBufferPointer();
						vs.BytecodeLength = vsBlob->GetBufferSize();
						ps.pShaderBytecode = psBlob->GetBufferPointer();
						ps.BytecodeLength = psBlob->GetBufferSize();
					}

					// PSO - Rasterizer State
					{
						bool bDoubleSidedMaterial = passDesc.scene->m_materialList[primitive.m_materialIndex].m_doubleSided;

						D3D12_RASTERIZER_DESC& desc = psoDesc.RasterizerState;
						desc.FillMode = D3D12_FILL_MODE_SOLID;
						desc.CullMode = bDoubleSidedMaterial ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
						desc.FrontCounterClockwise = TRUE;
						desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
						desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
						desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
						desc.DepthClipEnable = TRUE;
						desc.MultisampleEnable = FALSE;
						desc.AntialiasedLineEnable = FALSE;
						desc.ForcedSampleCount = 0;
						desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
					}

					// PSO - Blend State
					{
						D3D12_BLEND_DESC& desc = psoDesc.BlendState;
						desc.AlphaToCoverageEnable = FALSE;
						desc.IndependentBlendEnable = FALSE;
						desc.RenderTarget[0].BlendEnable = FALSE;
						desc.RenderTarget[0].LogicOpEnable = FALSE;
						desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
					}

					// PSO - Depth Stencil State
					{
						D3D12_DEPTH_STENCIL_DESC& desc = psoDesc.DepthStencilState;
						desc.DepthEnable = TRUE;
						desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
						desc.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
						desc.StencilEnable = FALSE;
					}

					D3DPipelineState_t* pso = RenderBackend12::FetchGraphicsPipelineState(psoDesc);
					d3dCmdList->SetPipelineState(pso);

					// Geometry constants
					struct PrimitiveCbLayout
					{
						Matrix localToWorldTransform;
						int m_indexAccessor;
						int m_positionAccessor;
						uint32_t m_objectId;
					} primCb =
					{
						passDesc.scene->m_sceneMeshes.m_transformList[meshIndex],
						primitive.m_indexAccessor,
						primitive.m_positionAccessor,
						objectId
					};

					d3dCmdList->SetGraphicsRoot32BitConstants(0, sizeof(PrimitiveCbLayout) / 4, &primCb, 0);

					d3dCmdList->DrawInstanced(primitive.m_indexCount, 1, 0, 0);
				}
			}

			return cmdList;

		}).then([&, renderToken](FCommandList* recordedCl) mutable
		{
			jobSync.Execute(renderToken, recordedCl);
		});
	}
}