namespace RenderJob::HBAO
{
	struct Desc
	{
		FShaderSurface* aoTarget;
		FShaderSurface* depthStencil;
		FShaderSurface* gbufferNormals;
		FSystemBuffer* sceneConstantBuffer;
		FSystemBuffer* viewConstantBuffer;
		FConfig renderConfig;
		uint32_t resX;
		uint32_t resY;
	};

	Result Execute(Sync* jobSync, const Desc& passDesc)
	{
		size_t renderToken = jobSync->GetToken();
		size_t aoTargetTransitionToken = passDesc.aoTarget->m_resource->GetTransitionToken();
		size_t depthStencilTransitionToken = passDesc.depthStencil->m_resource->GetTransitionToken();
		size_t gbufferNormalsTransitionToken = passDesc.gbufferNormals->m_resource->GetTransitionToken();
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"hbao", D3D12_COMMAND_LIST_TYPE_DIRECT);

		Result passResult;
		passResult.m_syncObj = cmdList->GetSync();
		passResult.m_task = concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("hbao", PIX_COLOR_DEFAULT);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "hbao", 0);

			// Transitions
			passDesc.aoTarget->m_resource->Transition(cmdList, aoTargetTransitionToken, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			passDesc.depthStencil->m_resource->Transition(cmdList, depthStencilTransitionToken, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			passDesc.gbufferNormals->m_resource->Transition(cmdList, gbufferNormalsTransitionToken, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"hbao_rootsig",
				cmdList,
				FRootSignature::Desc{ L"postprocess/hbao.hlsl", L"rootsig", L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

			IDxcBlob* csBlob = RenderBackend12::CacheShader({
				L"postprocess/hbao.hlsl",
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

			struct FPassConstants
			{
				uint32_t m_aoTargetUavIndex;
				uint32_t m_depthTargetSrvIndex;
				uint32_t m_gbufferNormalsSrvIndex;
			};

			FPassConstants cb = {};
			cb.m_aoTargetUavIndex = passDesc.aoTarget->m_descriptorIndices.UAVs[0];
			cb.m_depthTargetSrvIndex = passDesc.depthStencil->m_descriptorIndices.SRV;
			cb.m_gbufferNormalsSrvIndex = passDesc.gbufferNormals->m_descriptorIndices.SRV;

			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(FPassConstants) / 4, &cb, 0);
			d3dCmdList->SetComputeRootConstantBufferView(1, passDesc.viewConstantBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress());
			d3dCmdList->SetComputeRootConstantBufferView(2, passDesc.sceneConstantBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress());

			// Dispatch
			const size_t threadGroupCountX = GetDispatchSize(passDesc.resX, 16);
			const size_t threadGroupCountY = GetDispatchSize(passDesc.resY, 16);
			d3dCmdList->Dispatch(threadGroupCountX, threadGroupCountY, 1);

			return cmdList;

		}).then([=](FCommandList* recordedCl) mutable
		{
			jobSync->Execute(renderToken, recordedCl);
		});

		return passResult;
	}
}