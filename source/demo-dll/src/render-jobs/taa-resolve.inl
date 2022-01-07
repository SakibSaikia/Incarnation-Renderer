namespace RenderJob
{
	struct TAAResolveDesc
	{
		FRenderTexture* source;
		FBindlessUav* target;
		uint32_t resX;
		uint32_t resY;
		uint32_t historyIndex;
	};

	concurrency::task<void> TAAResolve(RenderJob::Sync& jobSync, const TAAResolveDesc& passDesc)
	{
		size_t renderToken = jobSync.GetToken();
		size_t colorSourceTransitionToken = passDesc.source->m_resource->GetTransitionToken();
		size_t uavTransitionToken = passDesc.target->m_resource->GetTransitionToken();

		return concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_taa_resolve", PIX_COLOR_DEFAULT);

			FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			cmdList->SetName(L"taa_resolve_job");

			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

			SCOPED_COMMAND_LIST_EVENT(cmdList, "taa_resolve", 0);

			passDesc.source->m_resource->Transition(cmdList, colorSourceTransitionToken, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			passDesc.target->m_resource->Transition(cmdList, uavTransitionToken, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({
				L"postprocess/taa-resolve.hlsl",
				L"rootsig",
				L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig.get());

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({
				L"postprocess/taa-resolve.hlsl",
				L"cs_main",
				L"THREAD_GROUP_SIZE_X=16 THREAD_GROUP_SIZE_Y=16",
				L"cs_6_6" });

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = rootsig.get();
			psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
			psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			D3DPipelineState_t* pso = RenderBackend12::FetchComputePipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			struct
			{
				uint32_t hdrSceneColorTextureIndex;
				uint32_t taaAccumulationUavIndex;
				uint32_t resX;
				uint32_t resY;
				uint32_t historyIndex;
			} rootConstants = { passDesc.source->m_srvIndex, passDesc.target->m_uavIndices[0], passDesc.resX, passDesc.resY, passDesc.historyIndex };

			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);

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
}