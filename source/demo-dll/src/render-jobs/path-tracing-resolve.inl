namespace RenderJob
{
	struct PathtraceResolvePassDesc
	{
		FBindlessUav* uavSource;
		FRenderTexture* colorTarget;
	};

	// Copy Data from input UAV to output RT
	concurrency::task<void> PathtraceResolve(RenderJob::Sync& jobSync, const PathtraceResolvePassDesc& passDesc)
	{
		size_t renderToken = jobSync.GetToken();
		size_t uavSourceTransitionToken = passDesc.uavSource->m_resource->GetTransitionToken();
		size_t colorTargetTransitionToken = passDesc.colorTarget->m_resource->GetTransitionToken();

		return concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_pathtrace_resolve", PIX_COLOR_DEFAULT);

			FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			cmdList->SetName(L"path_tracing_resolve_job");

			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

			SCOPED_COMMAND_LIST_EVENT(cmdList, "path_tracing_resolve", 0);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);
			
			// Root Signature
			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({ L"utils/texture-copy-raster.hlsl", L"rootsig", L"rootsig_1_1" });
			d3dCmdList->SetGraphicsRootSignature(rootsig.get());

			// PSO
			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.NodeMask = 1;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.pRootSignature = rootsig.get();
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = Config::g_backBufferFormat;
			psoDesc.SampleDesc.Count = 1;
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			// PSO - Shaders
			{
				D3D12_SHADER_BYTECODE& vs = psoDesc.VS;
				D3D12_SHADER_BYTECODE& ps = psoDesc.PS;

				IDxcBlob* vsBlob = RenderBackend12::CacheShader({ L"utils/texture-copy-raster.hlsl", L"vs_main", L"" , L"vs_6_6" });
				IDxcBlob* psBlob = RenderBackend12::CacheShader({ L"utils/texture-copy-raster.hlsl", L"ps_main", L"" , L"ps_6_6" });

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
				desc.FrontCounterClockwise = FALSE;
				desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
				desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
				desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
				desc.DepthClipEnable = FALSE;
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
				desc.DepthEnable = FALSE;
				desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
				desc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
				desc.StencilEnable = FALSE;
			}

			D3DPipelineState_t* pso = RenderBackend12::FetchGraphicsPipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			D3D12_RESOURCE_DESC colorTargetDesc = passDesc.colorTarget->m_resource->m_d3dResource->GetDesc();
			D3D12_VIEWPORT viewport{ 0.f, 0.f, (float)colorTargetDesc.Width, (float)colorTargetDesc.Height, 0.f, 1.f };
			D3D12_RECT screenRect{ 0, 0, (LONG)colorTargetDesc.Width, (LONG)colorTargetDesc.Height };
			d3dCmdList->RSSetViewports(1, &viewport);
			d3dCmdList->RSSetScissorRects(1, &screenRect);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passDesc.colorTarget->m_renderTextureIndices[0]) };
			d3dCmdList->OMSetRenderTargets(1, rtvs, FALSE, nullptr);

			d3dCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			// Root Constants
			int textureIndex = passDesc.uavSource->m_srvIndex;
			d3dCmdList->SetGraphicsRoot32BitConstants(0, 1, &textureIndex, 0);

			// Transitions
			passDesc.uavSource->m_resource->Transition(cmdList, uavSourceTransitionToken, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			passDesc.colorTarget->m_resource->Transition(cmdList, colorTargetTransitionToken, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);

			d3dCmdList->DrawInstanced(3, 1, 0, 0);

			return cmdList;

		}).then([&, renderToken](FCommandList* recordedCl) mutable
		{
			jobSync.Execute(renderToken, recordedCl);
		});
	}
}