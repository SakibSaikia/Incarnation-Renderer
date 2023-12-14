namespace RenderJob::BatchCullingPass
{
	struct Desc
	{
		FShaderBuffer* batchArgsBuffer_Default;
		FShaderBuffer* batchArgsBuffer_DoubleSided;
		FShaderBuffer* batchCountsBuffer;
		FSystemBuffer* sceneConstantBuffer;
		FSystemBuffer* viewConstantBuffer;
		size_t drawCount;
		FConfig renderConfig;
	};

	Result Execute(Sync* jobSync, const Desc& passDesc)
	{
		size_t renderToken = jobSync->GetToken();
		size_t defaultBatchArgsBufferTransitionToken = passDesc.batchArgsBuffer_Default->m_resource->GetTransitionToken();
		size_t doubleSidedBatchArgsBufferTransitionToken = passDesc.batchArgsBuffer_DoubleSided->m_resource->GetTransitionToken();
		size_t batchCountsBufferTransitionToken = passDesc.batchCountsBuffer->m_resource->GetTransitionToken();
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"batch_culling", D3D12_COMMAND_LIST_TYPE_DIRECT);

		Result passResult;
		passResult.m_syncObj = cmdList->GetSync();
		passResult.m_task = concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("batch_culling", PIX_COLOR_DEFAULT);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "batch_culling", 0);

			// Transitions
			passDesc.batchArgsBuffer_Default->m_resource->Transition(cmdList, defaultBatchArgsBufferTransitionToken, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			passDesc.batchArgsBuffer_DoubleSided->m_resource->Transition(cmdList, doubleSidedBatchArgsBufferTransitionToken, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			passDesc.batchCountsBuffer->m_resource->Transition(cmdList, batchCountsBufferTransitionToken, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"batch_cull_rootsig",
				cmdList,
				FRootSignature::Desc{ L"culling/batch-culling.hlsl", L"rootsig", L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

			std::wstring shaderMacros = PrintString(
				L"THREAD_GROUP_SIZE_X=128 FRUSTUM_CULLING=%d",
				passDesc.renderConfig.FrustumCulling ? 1 : 0);

			std::wstring shaderEntryPoint = passDesc.renderConfig.UseMeshlets ? L"cs_meshlet_cull_main" : L"cs_primitive_cull_main";

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({
				L"culling/batch-culling.hlsl",
				shaderEntryPoint,
				shaderMacros,
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
				uint32_t m_defaultArgsBufferIndex;
				uint32_t m_doubleSidedArgsBufferIndex;
				uint32_t m_countsBufferIndex;
			};

			FPassConstants cb = {};
			cb.m_defaultArgsBufferIndex = passDesc.batchArgsBuffer_Default->m_descriptorIndices.UAV;
			cb.m_doubleSidedArgsBufferIndex = passDesc.batchArgsBuffer_DoubleSided->m_descriptorIndices.UAV;
			cb.m_countsBufferIndex = passDesc.batchCountsBuffer->m_descriptorIndices.UAV;

			d3dCmdList->SetComputeRoot32BitConstants(0, std::max<uint32_t>(1, sizeof(FPassConstants) / 4), &cb, 0);
			d3dCmdList->SetComputeRootConstantBufferView(1, passDesc.viewConstantBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress());
			d3dCmdList->SetComputeRootConstantBufferView(2, passDesc.sceneConstantBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress());

			// Initialize counts buffer to 0
			const uint32_t clearValue[] = { 0, 0, 0, 0 };
			d3dCmdList->ClearUnorderedAccessViewUint(
				RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passDesc.batchCountsBuffer->m_descriptorIndices.UAV),
				RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passDesc.batchCountsBuffer->m_descriptorIndices.NonShaderVisibleUAV, false),
				passDesc.batchCountsBuffer->m_resource->m_d3dResource,
				clearValue, 0, nullptr);

			// Dispatch
			size_t threadGroupCountX = GetDispatchSize(passDesc.drawCount, 128);
			d3dCmdList->Dispatch(threadGroupCountX, 1, 1);

			return cmdList;

		}).then([=](FCommandList* recordedCl) mutable
		{
			jobSync->Execute(renderToken, recordedCl);
		});

		return passResult;
	}
}