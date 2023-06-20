namespace RenderJob::DebugVizPass
{
	struct Desc
	{
		FShaderSurface* visBuffer;
		FShaderSurface* gbuffers[3];
		FShaderSurface* target;
		FShaderSurface* depthBuffer;
		FShaderSurface* aoBuffer;
		FShaderBuffer* indirectArgsBuffer;
		Vector2 jitter;
		FConfig renderConfig;
		uint32_t resX, resY;
		uint32_t mouseX, mouseY;
		const FScene* scene;
		const FView* view;
	};

	// Copy Data from input UAV to output RT while applying tonemapping
	Result Execute(Sync* jobSync, const Desc& passDesc)
	{
		size_t renderToken = jobSync->GetToken();
		size_t visBufferTransitionToken = passDesc.visBuffer->m_resource->GetTransitionToken();
		size_t targetTransitionToken = passDesc.target->m_resource->GetTransitionToken();
		size_t gbuffer0TransitionToken = passDesc.gbuffers[0]->m_resource->GetTransitionToken();
		size_t gbuffer1TransitionToken = passDesc.gbuffers[1]->m_resource->GetTransitionToken();
		size_t gbuffer2TransitionToken = passDesc.gbuffers[2]->m_resource->GetTransitionToken();
		size_t depthTransitionToken = passDesc.depthBuffer->m_resource->GetTransitionToken();
		size_t aoTransitionToken = passDesc.aoBuffer->m_resource->GetTransitionToken();
		size_t indirectArgsTransitionToken = passDesc.indirectArgsBuffer->m_resource->GetTransitionToken();
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"debugviz_job", D3D12_COMMAND_LIST_TYPE_DIRECT);

		Result passResult;
		passResult.m_syncObj = cmdList->GetSync();
		passResult.m_task = concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_debugviz_pass", PIX_COLOR_DEFAULT);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "debugviz", 0);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"debugviz_rootsig",
				cmdList,
				FRootSignature::Desc{ L"postprocess/debug-visualization.hlsl", L"rootsig", L"rootsig_1_1" });
			d3dCmdList->SetGraphicsRootSignature(rootsig->m_rootsig);

			// PSO
			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.NodeMask = 1;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.pRootSignature = rootsig->m_rootsig;
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = passDesc.target->m_resource->m_d3dResource->GetDesc().Format;
			psoDesc.SampleDesc.Count = 1;
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			// PSO - Shaders
			{
				D3D12_SHADER_BYTECODE& vs = psoDesc.VS;
				D3D12_SHADER_BYTECODE& ps = psoDesc.PS;

				IDxcBlob* vsBlob = RenderBackend12::CacheShader({ L"postprocess/debug-visualization.hlsl", L"vs_main", L"" , L"vs_6_6" });
				IDxcBlob* psBlob = RenderBackend12::CacheShader({ L"postprocess/debug-visualization.hlsl", L"ps_main", PrintString(L"VIEWMODE=%d", passDesc.renderConfig.Viewmode), L"ps_6_6"});

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

			D3D12_RESOURCE_DESC colorTargetDesc = passDesc.target->m_resource->m_d3dResource->GetDesc();
			D3D12_VIEWPORT viewport{ 0.f, 0.f, (float)colorTargetDesc.Width, (float)colorTargetDesc.Height, 0.f, 1.f };
			D3D12_RECT screenRect{ 0, 0, (LONG)colorTargetDesc.Width, (LONG)colorTargetDesc.Height };
			d3dCmdList->RSSetViewports(1, &viewport);
			d3dCmdList->RSSetScissorRects(1, &screenRect);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passDesc.target->m_descriptorIndices.RTVorDSVs[0]) };
			d3dCmdList->OMSetRenderTargets(1, rtvs, FALSE, nullptr);

			d3dCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			// Root Constants
			struct
			{
				int visBufferTextureIndex;
				int gbuffer0TextureIndex;
				int gbuffer1TextureIndex;
				int gbuffer2TextureIndex;
				int depthBufferTextureIndex;
				int aoTextureIndex;
				int indirectArgsBufferIndex;
				int sceneMeshAccessorsIndex;
				int sceneMeshBufferViewsIndex;
				int scenePrimitivesIndex;
				int viewmode;
				uint32_t resX;
				uint32_t resY;
				uint32_t mouseX;
				uint32_t mouseY;
				uint32_t lightClusterSlices;
				Matrix invProjectionTransform;
			} rootConstants = {
					(int)passDesc.visBuffer->m_descriptorIndices.SRV,
					(int)passDesc.gbuffers[0]->m_descriptorIndices.SRV,
					(int)passDesc.gbuffers[1]->m_descriptorIndices.SRV,
					(int)passDesc.gbuffers[2]->m_descriptorIndices.SRV,
					(int)passDesc.depthBuffer->m_descriptorIndices.SRV,
					(int)passDesc.aoBuffer->m_descriptorIndices.SRV,
					(int)passDesc.indirectArgsBuffer->m_descriptorIndices.UAV,
					(int)passDesc.scene->m_packedMeshAccessors->m_descriptorIndices.SRV,
					(int)passDesc.scene->m_packedMeshBufferViews->m_descriptorIndices.SRV,
					(int)passDesc.scene->m_packedPrimitives->m_descriptorIndices.SRV,
					passDesc.renderConfig.Viewmode,
					passDesc.resX,
					passDesc.resY,
					passDesc.mouseX,
					passDesc.mouseY,
					passDesc.renderConfig.LightClusterDimZ,
					(passDesc.view->m_projectionTransform * Matrix::CreateTranslation(passDesc.jitter.x, passDesc.jitter.y, 0.f)).Invert()
			};
			d3dCmdList->SetGraphicsRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);

			// Transitions
			passDesc.visBuffer->m_resource->Transition(cmdList, visBufferTransitionToken, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			passDesc.gbuffers[0]->m_resource->Transition(cmdList, gbuffer0TransitionToken, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			passDesc.gbuffers[1]->m_resource->Transition(cmdList, gbuffer1TransitionToken, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			passDesc.gbuffers[2]->m_resource->Transition(cmdList, gbuffer2TransitionToken, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			passDesc.depthBuffer->m_resource->Transition(cmdList, depthTransitionToken, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			passDesc.aoBuffer->m_resource->Transition(cmdList, aoTransitionToken, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			passDesc.target->m_resource->Transition(cmdList, targetTransitionToken, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
			passDesc.indirectArgsBuffer->m_resource->Transition(cmdList, indirectArgsTransitionToken, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			d3dCmdList->DrawInstanced(3, 1, 0, 0);

			return cmdList;

		}).then([=](FCommandList* recordedCl) mutable
		{
			jobSync->Execute(renderToken, recordedCl);
		});

		return passResult;
	}
}