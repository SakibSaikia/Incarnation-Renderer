namespace RenderJob
{
	struct GBufferPassDesc
	{
		FShaderSurface* sourceVisBuffer;
		FShaderSurface* gbufferTargets[3];
		FShaderSurface* depthStencilTarget;
		uint32_t resX;
		uint32_t resY;
		const FScene* scene;
		const FView* view;
		Vector2 jitter;
		FConfig renderConfig;
	};

	concurrency::task<void> GBufferComputePass(RenderJob::Sync& jobSync, const GBufferPassDesc& passDesc)
	{
		size_t renderToken = jobSync.GetToken();
		size_t sourceTransitionToken = passDesc.sourceVisBuffer->m_resource->GetTransitionToken();
		size_t gbufferTransitionTokens[3] = {
			passDesc.gbufferTargets[0]->m_resource->GetTransitionToken(),
			passDesc.gbufferTargets[1]->m_resource->GetTransitionToken(),
			passDesc.gbufferTargets[2]->m_resource->GetTransitionToken()
		};

		return concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_gbuffer_pass", PIX_COLOR_DEFAULT);

			FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			cmdList->SetName(L"gbuffer_pass");

			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

			SCOPED_COMMAND_LIST_EVENT(cmdList, "gbuffer_pass", 0);

			passDesc.sourceVisBuffer->m_resource->Transition(cmdList, sourceTransitionToken, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			passDesc.gbufferTargets[0]->m_resource->Transition(cmdList, gbufferTransitionTokens[0], 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			passDesc.gbufferTargets[1]->m_resource->Transition(cmdList, gbufferTransitionTokens[1], 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			passDesc.gbufferTargets[2]->m_resource->Transition(cmdList, gbufferTransitionTokens[2], 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"gbuffer_rootsig",
				cmdList,
				FRootsigDesc { L"geo-raster/gbuffer-pass.hlsl", L"rootsig", L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({
				L"geo-raster/gbuffer-pass.hlsl",
				L"cs_main",
				L"THREAD_GROUP_SIZE_X=16 THREAD_GROUP_SIZE_Y=16",
				L"cs_6_6" });

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = rootsig->m_rootsig;
			psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
			psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			D3DPipelineState_t* pso = RenderBackend12::FetchComputePipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			struct GBufferConstants
			{
				uint32_t gbuffer0UavIndex;
				uint32_t gbuffer1UavIndex;
				uint32_t gbuffer2UavIndex;
				uint32_t visBufferSrvIndex;
				int sceneMeshAccessorsIndex;
				int sceneMeshBufferViewsIndex;
				int sceneMaterialBufferIndex;
				int scenePrimitivesIndex;
				uint32_t resX;
				uint32_t resY;
				float __pad[2];
				Matrix viewProjTransform;
				Matrix sceneRotation;
			};

			std::unique_ptr<FUploadBuffer> cbuf = RenderBackend12::CreateUploadBuffer(
				L"gbuffer_cb",
				sizeof(GBufferConstants),
				cmdList,
				[passDesc](uint8_t* pDest)
				{
					auto cb = reinterpret_cast<GBufferConstants*>(pDest);
					cb->gbuffer0UavIndex = passDesc.gbufferTargets[0]->m_uavIndices[0];
					cb->gbuffer1UavIndex = passDesc.gbufferTargets[1]->m_uavIndices[0];
					cb->gbuffer2UavIndex = passDesc.gbufferTargets[2]->m_uavIndices[0];
					cb->visBufferSrvIndex = passDesc.sourceVisBuffer->m_srvIndex;
					cb->sceneMeshAccessorsIndex = passDesc.scene->m_packedMeshAccessors->m_srvIndex;
					cb->sceneMeshBufferViewsIndex = passDesc.scene->m_packedMeshBufferViews->m_srvIndex;
					cb->sceneMaterialBufferIndex = passDesc.scene->m_packedMaterials->m_srvIndex;
					cb->scenePrimitivesIndex = passDesc.scene->m_packedPrimitives->m_srvIndex;
					cb->resX = passDesc.resX;
					cb->resY = passDesc.resY;
					cb->viewProjTransform = passDesc.view->m_viewTransform * passDesc.view->m_projectionTransform;
					cb->sceneRotation = passDesc.scene->m_rootTransform;
				});

			d3dCmdList->SetComputeRootConstantBufferView(0, cbuf->m_resource->m_d3dResource->GetGPUVirtualAddress());

			// Dispatch
			size_t threadGroupCountX = std::max<size_t>(std::ceil(passDesc.resX / 16), 1);
			size_t threadGroupCountY = std::max<size_t>(std::ceil(passDesc.resY / 16), 1);
			d3dCmdList->Dispatch(threadGroupCountX, threadGroupCountY, 1);

			return cmdList;

		}).then([&, renderToken](FCommandList* recordedCl) mutable
		{
			jobSync.Execute(renderToken, recordedCl);
		});
	}

	concurrency::task<void> GBufferDecalPass(RenderJob::Sync& jobSync, const GBufferPassDesc& passDesc)
	{
		size_t renderToken = jobSync.GetToken();
		size_t gbufferTransitionTokens[3] = {
			passDesc.gbufferTargets[0]->m_resource->GetTransitionToken(),
			passDesc.gbufferTargets[1]->m_resource->GetTransitionToken(),
			passDesc.gbufferTargets[2]->m_resource->GetTransitionToken()
		};

		return concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_gbuffer_decals", PIX_COLOR_DEFAULT);

			FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			cmdList->SetName(L"gbuffer_decals");

			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

			SCOPED_COMMAND_LIST_EVENT(cmdList, "gbuffer_decals", 0);

			passDesc.gbufferTargets[0]->m_resource->Transition(cmdList, gbufferTransitionTokens[0], 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
			passDesc.gbufferTargets[1]->m_resource->Transition(cmdList, gbufferTransitionTokens[1], 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
			passDesc.gbufferTargets[2]->m_resource->Transition(cmdList, gbufferTransitionTokens[2], 0, D3D12_RESOURCE_STATE_RENDER_TARGET);

			// Descriptor heaps need to be set before setting the root signature when using HLSL Dynamic Resources
			// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html
			D3DDescriptorHeap_t* descriptorHeaps[] =
			{
				RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
				RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
			};
			d3dCmdList->SetDescriptorHeaps(2, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"gbuffer_geo_rootsig",
				cmdList,
				FRootsigDesc{ L"geo-raster/gbuffer-geo.hlsl", L"rootsig", L"rootsig_1_1" });
			d3dCmdList->SetGraphicsRootSignature(rootsig->m_rootsig);

			// Frame constant buffer
			struct FrameCbLayout
			{
				Matrix sceneRotation;
				int sceneMeshAccessorsIndex;
				int sceneMeshBufferViewsIndex;
				int sceneMaterialBufferIndex;
			};

			std::unique_ptr<FUploadBuffer> frameCb = RenderBackend12::CreateUploadBuffer(
				L"frame_cb",
				sizeof(FrameCbLayout),
				cmdList,
				[passDesc](uint8_t* pDest)
				{
					const int lightCount = passDesc.scene->m_lights.size();

					auto cbDest = reinterpret_cast<FrameCbLayout*>(pDest);
					cbDest->sceneRotation = passDesc.scene->m_rootTransform;
					cbDest->sceneMeshAccessorsIndex = passDesc.scene->m_packedMeshAccessors->m_srvIndex;
					cbDest->sceneMeshBufferViewsIndex = passDesc.scene->m_packedMeshBufferViews->m_srvIndex;
					cbDest->sceneMaterialBufferIndex = passDesc.scene->m_packedMaterials->m_srvIndex;
				});

			d3dCmdList->SetGraphicsRootConstantBufferView(2, frameCb->m_resource->m_d3dResource->GetGPUVirtualAddress());

			// View constant buffer
			struct ViewCbLayout
			{
				Matrix viewTransform;
				Matrix projectionTransform;
				Vector3 eyePos;
			};

			std::unique_ptr<FUploadBuffer> viewCb = RenderBackend12::CreateUploadBuffer(
				L"view_cb",
				sizeof(ViewCbLayout),
				cmdList,
				[passDesc](uint8_t* pDest)
				{
					auto cbDest = reinterpret_cast<ViewCbLayout*>(pDest);
					cbDest->viewTransform = passDesc.view->m_viewTransform;
					cbDest->projectionTransform = passDesc.view->m_projectionTransform * Matrix::CreateTranslation(passDesc.jitter.x, passDesc.jitter.y, 0.f);
					cbDest->eyePos = passDesc.view->m_position;
				});

			d3dCmdList->SetGraphicsRootConstantBufferView(1, viewCb->m_resource->m_d3dResource->GetGPUVirtualAddress());

			D3D12_VIEWPORT viewport{ 0.f, 0.f, (float)passDesc.resX, (float)passDesc.resY, 0.f, 1.f };
			D3D12_RECT screenRect{ 0, 0, (LONG)passDesc.resX, (LONG)passDesc.resY };
			d3dCmdList->RSSetViewports(1, &viewport);
			d3dCmdList->RSSetScissorRects(1, &screenRect);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { 
				RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passDesc.gbufferTargets[0]->m_renderTextureIndices[0]),
				RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passDesc.gbufferTargets[1]->m_renderTextureIndices[0]),
				RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passDesc.gbufferTargets[2]->m_renderTextureIndices[0])
			};

			D3D12_CPU_DESCRIPTOR_HANDLE dsv = RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, passDesc.depthStencilTarget->m_renderTextureIndices[0]);
			d3dCmdList->OMSetRenderTargets(3, rtvs, FALSE, &dsv);

			// Issue decal draws
			for (int meshIndex = 0; meshIndex < passDesc.scene->m_sceneMeshDecals.m_entityList.size(); ++meshIndex)
			{
				const FMesh& mesh = passDesc.scene->m_sceneMeshDecals.m_entityList[meshIndex];
				SCOPED_COMMAND_LIST_EVENT(cmdList, passDesc.scene->m_sceneMeshDecals.m_entityNames[meshIndex].c_str(), 0);

				for (const FMeshPrimitive& primitive : mesh.m_primitives)
				{
					d3dCmdList->IASetPrimitiveTopology(primitive.m_topology);

					// PSO
					D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
					psoDesc.NodeMask = 1;
					psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
					psoDesc.pRootSignature = rootsig->m_rootsig;
					psoDesc.SampleMask = UINT_MAX;
					psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
					psoDesc.NumRenderTargets = 3;
					psoDesc.RTVFormats[0] = passDesc.gbufferTargets[0]->m_resource->m_d3dResource->GetDesc().Format;
					psoDesc.RTVFormats[1] = passDesc.gbufferTargets[1]->m_resource->m_d3dResource->GetDesc().Format;
					psoDesc.RTVFormats[2] = passDesc.gbufferTargets[2]->m_resource->m_d3dResource->GetDesc().Format;
					psoDesc.SampleDesc.Count = 1;
					psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

					// PSO - Shaders
					{
						IDxcBlob* vsBlob = RenderBackend12::CacheShader({ L"geo-raster/gbuffer-geo.hlsl", L"vs_main", L"" , L"vs_6_6" });
						IDxcBlob* psBlob = RenderBackend12::CacheShader({ L"geo-raster/gbuffer-geo.hlsl", L"ps_main", L"" , L"ps_6_6"});

						psoDesc.VS.pShaderBytecode = vsBlob->GetBufferPointer();
						psoDesc.VS.BytecodeLength = vsBlob->GetBufferSize();
						psoDesc.PS.pShaderBytecode = psBlob->GetBufferPointer();
						psoDesc.PS.BytecodeLength = psBlob->GetBufferSize();
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
						desc.RenderTarget[0].BlendEnable = TRUE;
						desc.RenderTarget[0].LogicOpEnable = FALSE;
						desc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
						desc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
						desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
						desc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
						desc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
						desc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
						desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
					}

					// PSO - Depth Stencil State
					{
						D3D12_DEPTH_STENCIL_DESC& desc = psoDesc.DepthStencilState;
						desc.DepthEnable = TRUE;
						desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
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
						int m_uvAccessor;
						int m_normalAccessor;
						int m_tangentAccessor;
						int m_materialIndex;
					} primCb =
						{
							passDesc.scene->m_sceneMeshDecals.m_transformList[meshIndex],
							primitive.m_indexAccessor,
							primitive.m_positionAccessor,
							primitive.m_uvAccessor,
							primitive.m_normalAccessor,
							primitive.m_tangentAccessor,
							primitive.m_materialIndex
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