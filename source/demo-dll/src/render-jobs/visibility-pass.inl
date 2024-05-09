namespace RenderJob::VisibilityPass
{
	struct Desc
	{
		FShaderSurface* visBufferTarget;
		FShaderSurface* depthStencilTarget;
		FShaderBuffer* indirectArgsBuffer_Default;
		FShaderBuffer* indirectArgsBuffer_DoubleSided;
		FShaderBuffer* indirectCountsBuffer;
		FSystemBuffer* sceneConstantBuffer;
		FSystemBuffer* viewConstantBuffer;
		DXGI_FORMAT visBufferFormat;
		uint32_t resX;
		uint32_t resY;
		size_t drawCount;
		FConfig renderConfig;
	};

	Result Execute(Sync* jobSync, const Desc& passDesc)
	{
		size_t renderToken = jobSync->GetToken();
		size_t visBufferTransitionToken = passDesc.visBufferTarget->m_resource->GetTransitionToken();
		size_t depthStencilTransitionToken = passDesc.depthStencilTarget->m_resource->GetTransitionToken();
		size_t defaultIndirectArgsToken = passDesc.indirectArgsBuffer_Default->m_resource->GetTransitionToken();
		size_t doubleSidedIndirectArgsToken = passDesc.indirectArgsBuffer_DoubleSided->m_resource->GetTransitionToken();
		size_t indirectCountToken = passDesc.indirectCountsBuffer->m_resource->GetTransitionToken();
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"visibility_pass_job", D3D12_COMMAND_LIST_TYPE_DIRECT);

		Result passResult;
		passResult.m_syncObj = cmdList->GetSync();
		passResult.m_task = concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_visibility_pass", PIX_COLOR_DEFAULT);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "visibility_pass", 0);

			passDesc.visBufferTarget->m_resource->Transition(cmdList, visBufferTransitionToken, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
			passDesc.depthStencilTarget->m_resource->Transition(cmdList, depthStencilTransitionToken, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			passDesc.indirectArgsBuffer_Default->m_resource->Transition(cmdList, defaultIndirectArgsToken, 0, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
			passDesc.indirectArgsBuffer_DoubleSided->m_resource->Transition(cmdList, doubleSidedIndirectArgsToken, 0, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
			passDesc.indirectCountsBuffer->m_resource->Transition(cmdList, indirectCountToken, 0, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

			// Descriptor heaps need to be set before setting the root signature when using HLSL Dynamic Resources
			// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html
			D3DDescriptorHeap_t* descriptorHeaps[] =
			{
				RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
				RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
			};
			d3dCmdList->SetDescriptorHeaps(2, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(L"visbuffer_rootsig", cmdList, FRootSignature::Desc{ L"geo-raster/visibility-pass.hlsl", L"rootsig", L"rootsig_1_1" });
			d3dCmdList->SetGraphicsRootSignature(rootsig->m_rootsig);

			d3dCmdList->SetGraphicsRootConstantBufferView(1, passDesc.viewConstantBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress());
			d3dCmdList->SetGraphicsRootConstantBufferView(2, passDesc.sceneConstantBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress());

			D3D12_VIEWPORT viewport{ 0.f, 0.f, (float)passDesc.resX, (float)passDesc.resY, 0.f, 1.f };
			D3D12_RECT screenRect{ 0, 0, (LONG)passDesc.resX, (LONG)passDesc.resY };
			d3dCmdList->RSSetViewports(1, &viewport);
			d3dCmdList->RSSetScissorRects(1, &screenRect);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passDesc.visBufferTarget->m_descriptorIndices.RTVorDSVs[0]) };
			D3D12_CPU_DESCRIPTOR_HANDLE dsv = RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, passDesc.depthStencilTarget->m_descriptorIndices.RTVorDSVs[0]);
			d3dCmdList->OMSetRenderTargets(1, rtvs, FALSE, &dsv);

			// Clear to max object ID (see encoding.hlsli). G-Buffer pass skips decoding if it encouters this value.
			// Float clear values are converted to integers if RT format is int/uint. 
			// See https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#ClearView
			uint32_t clearValue = 0xFFFE0000;
			float clearColor[] = { clearValue, clearValue, clearValue, clearValue };
			d3dCmdList->ClearRenderTargetView(rtvs[0], clearColor, 0, nullptr);
			d3dCmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.f, 0xff, 0, nullptr);

			// Issue scene draws
			d3dCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			// PSO
			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.NodeMask = 1;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.pRootSignature = rootsig->m_rootsig;
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = passDesc.visBufferFormat;
			psoDesc.SampleDesc.Count = 1;
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			// PSO - Shaders
			{
				D3D12_SHADER_BYTECODE& vs = psoDesc.VS;
				D3D12_SHADER_BYTECODE& ps = psoDesc.PS;

				const std::wstring vsEntrypoint = passDesc.renderConfig.UseMeshlets ? L"vs_meshlet_main" : L"vs_primitive_main";
				const std::wstring psEntrypoint = passDesc.renderConfig.UseMeshlets ? L"ps_meshlet_main" : L"ps_primitive_main";
				IDxcBlob* vsBlob = RenderBackend12::CacheShader({ L"geo-raster/visibility-pass.hlsl", vsEntrypoint, L"" , L"vs_6_6" });
				IDxcBlob* psBlob = RenderBackend12::CacheShader({ L"geo-raster/visibility-pass.hlsl", psEntrypoint, L"" , L"ps_6_6" });

				vs.pShaderBytecode = vsBlob->GetBufferPointer();
				vs.BytecodeLength = vsBlob->GetBufferSize();
				ps.pShaderBytecode = psBlob->GetBufferPointer();
				ps.BytecodeLength = psBlob->GetBufferSize();
			}

			// PSO - Rasterizer State
			{
				D3D12_RASTERIZER_DESC& desc = psoDesc.RasterizerState;
				desc.FillMode = D3D12_FILL_MODE_SOLID;
				desc.CullMode = D3D12_CULL_MODE_BACK;
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
				desc.StencilEnable = TRUE;
				desc.StencilReadMask = 0xff;
				desc.StencilWriteMask = 0xff;
				desc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
				desc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
				desc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
				desc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
				desc.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
				desc.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
				desc.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
				desc.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
			}

			D3DPipelineState_t* pso = RenderBackend12::FetchGraphicsPipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			d3dCmdList->OMSetStencilRef(0);

			// Command signature
			D3DCommandSignature_t* commandSignature = FIndirectDrawWithRootConstants::GetCommandSignature(rootsig->m_rootsig);

			{
				SCOPED_COMMAND_LIST_EVENT(cmdList, "default", 0);
				const size_t defaultArgsCountOffset = 0;
				d3dCmdList->ExecuteIndirect(
					commandSignature,
					passDesc.drawCount,
					passDesc.indirectArgsBuffer_Default->m_resource->m_d3dResource,
					0,
					passDesc.indirectCountsBuffer->m_resource->m_d3dResource,
					defaultArgsCountOffset);
			}

			{
				psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
				D3DPipelineState_t* pso = RenderBackend12::FetchGraphicsPipelineState(psoDesc);
				d3dCmdList->SetPipelineState(pso);

				const size_t doubleSidedArgsCountOffset = sizeof(uint32_t);

				SCOPED_COMMAND_LIST_EVENT(cmdList, "double_sided", 0);
				d3dCmdList->ExecuteIndirect(
					commandSignature,
					passDesc.drawCount,
					passDesc.indirectArgsBuffer_DoubleSided->m_resource->m_d3dResource,
					0,
					passDesc.indirectCountsBuffer->m_resource->m_d3dResource,
					doubleSidedArgsCountOffset);
			}

			return cmdList;

		}).then([=](FCommandList* recordedCl) mutable
		{
			jobSync->Execute(renderToken, recordedCl);
		});

		return passResult;
	}
}