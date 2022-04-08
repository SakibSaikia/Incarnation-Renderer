namespace RenderJob
{
	struct GBufferPassDesc
	{
		FRenderTexture* sourceVisBuffer;
		FBindlessUav* gbufferTargets[3];
		uint32_t resX;
		uint32_t resY;
		const FScene* scene;
		const FView* view;
		FConfig renderConfig;
	};

	concurrency::task<void> GBufferPass(RenderJob::Sync& jobSync, const GBufferPassDesc& passDesc)
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
			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({
				L"geo-raster/gbuffer-pass.hlsl",
				L"rootsig",
				L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig.get());

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({
				L"geo-raster/gbuffer-pass.hlsl",
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

			std::unique_ptr<FTransientBuffer> cbuf = RenderBackend12::CreateTransientBuffer(
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
}