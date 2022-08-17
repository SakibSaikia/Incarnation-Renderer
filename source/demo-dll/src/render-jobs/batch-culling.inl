namespace RenderJob
{
	struct BatchCullingDesc
	{
		FShaderBuffer* batchArgsBuffer;
		FShaderBuffer* batchCountsBuffer;
		FShaderBuffer* debugStatsBuffer;
		const FScene* scene;
		const FView* view;
		size_t primitiveCount;
		Vector2 jitter;
	};

	concurrency::task<void> BatchCulling(RenderJob::Sync& jobSync, const BatchCullingDesc& passDesc)
	{
		size_t renderToken = jobSync.GetToken();
		size_t batchArgsBufferTransitionToken = passDesc.batchArgsBuffer->m_resource->GetTransitionToken();
		size_t batchCountsBufferTransitionToken = passDesc.batchCountsBuffer->m_resource->GetTransitionToken();

		return concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("batch_culling", PIX_COLOR_DEFAULT);
			FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"batch_culling", D3D12_COMMAND_LIST_TYPE_DIRECT);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "batch_culling", 0);

			// Transitions
			passDesc.batchArgsBuffer->m_resource->Transition(cmdList, batchArgsBufferTransitionToken, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			passDesc.batchCountsBuffer->m_resource->Transition(cmdList, batchCountsBufferTransitionToken, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"batch_cull_rootsig",
				cmdList,
				FRootsigDesc{ L"culling/batch-culling.hlsl", L"rootsig", L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({
				L"culling/batch-culling.hlsl",
				L"cs_main",
				L"THREAD_GROUP_SIZE_X=128",
				L"cs_6_6" });

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = rootsig->m_rootsig;
			psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
			psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			D3DPipelineState_t* pso = RenderBackend12::FetchComputePipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			// Root Constants
			struct Constants
			{
				uint32_t batchArgsBufferUavIndex;
				uint32_t batchCountsBufferUavIndex;
				uint32_t debugStatsBufferUavIndex;
				uint32_t scenePrimitivesIndex;
				uint32_t primitiveCount;
				Matrix viewProjTransform;
			};

			std::unique_ptr<FUploadBuffer> cbuf = RenderBackend12::CreateUploadBuffer(
				L"batch_cull_cb",
				sizeof(Constants),
				cmdList,
				[passDesc](uint8_t* pDest)
				{
					auto cb = reinterpret_cast<Constants*>(pDest);
					cb->batchArgsBufferUavIndex = passDesc.batchArgsBuffer->m_uavIndex;
					cb->batchCountsBufferUavIndex = passDesc.batchCountsBuffer->m_uavIndex;
					cb->debugStatsBufferUavIndex = passDesc.debugStatsBuffer->m_uavIndex;
					cb->scenePrimitivesIndex = passDesc.scene->m_packedPrimitives->m_srvIndex;
					cb->primitiveCount = (uint32_t)passDesc.primitiveCount;
					cb->viewProjTransform = passDesc.view->m_viewTransform * passDesc.view->m_projectionTransform * Matrix::CreateTranslation(passDesc.jitter.x, passDesc.jitter.y, 0.f);
				});

			d3dCmdList->SetComputeRootConstantBufferView(0, cbuf->m_resource->m_d3dResource->GetGPUVirtualAddress());

			// Initialize counts buffer to 0
			const uint32_t clearValue[] = { 0, 0, 0, 0 };
			d3dCmdList->ClearUnorderedAccessViewUint(
				RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passDesc.batchCountsBuffer->m_uavIndex),
				RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passDesc.batchCountsBuffer->m_nonShaderVisibleUavIndex, false),
				passDesc.batchCountsBuffer->m_resource->m_d3dResource,
				clearValue, 0, nullptr);

			// Dispatch
			size_t threadGroupCountX = std::max<size_t>(std::ceil(passDesc.primitiveCount / 128), 1);
			d3dCmdList->Dispatch(threadGroupCountX, 1, 1);

			return cmdList;

		}).then([&, renderToken](FCommandList* recordedCl) mutable
		{
			jobSync.Execute(renderToken, recordedCl);
		});
	}
}