namespace RenderJob::GBufferComputePass
{
	struct Desc
	{
		FShaderSurface* sourceVisBuffer;
		FShaderSurface* colorTarget;
		FShaderSurface* gbufferTargets[3];
		FShaderSurface* depthStencilTarget;
		FSystemBuffer* sceneConstantBuffer;
		FSystemBuffer* viewConstantBuffer;
		uint32_t resX;
		uint32_t resY;
		const FScene* scene;
		FConfig renderConfig;
	};

	Result Execute(Sync* jobSync, const Desc& passDesc)
	{
		size_t renderToken = jobSync->GetToken();
		size_t sourceTransitionToken = passDesc.sourceVisBuffer->m_resource->GetTransitionToken();
		size_t colorTargetTransitionToken = passDesc.colorTarget->m_resource->GetTransitionToken();
		size_t gbufferTransitionTokens[3] = {
			passDesc.gbufferTargets[0]->m_resource->GetTransitionToken(),
			passDesc.gbufferTargets[1]->m_resource->GetTransitionToken(),
			passDesc.gbufferTargets[2]->m_resource->GetTransitionToken()
		};

		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"gbuffer_compute", D3D12_COMMAND_LIST_TYPE_DIRECT);

		Result passResult;
		passResult.m_syncObj = cmdList->GetSync();
		passResult.m_task = concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_gbuffer_compute", PIX_COLOR_DEFAULT);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "gbuffer_compute", 0);

			passDesc.sourceVisBuffer->m_resource->Transition(cmdList, sourceTransitionToken, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			passDesc.colorTarget->m_resource->Transition(cmdList, colorTargetTransitionToken, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			passDesc.gbufferTargets[0]->m_resource->Transition(cmdList, gbufferTransitionTokens[0], 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			passDesc.gbufferTargets[1]->m_resource->Transition(cmdList, gbufferTransitionTokens[1], 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			passDesc.gbufferTargets[2]->m_resource->Transition(cmdList, gbufferTransitionTokens[2], 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"gbuffer_compute_rootsig",
				cmdList,
				FRootSignature::Desc { L"geo-raster/gbuffer-compute.hlsl", L"rootsig", L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

			std::wstring shaderMacros = PrintString(L"THREAD_GROUP_SIZE_X=16 THREAD_GROUP_SIZE_Y=16 USING_MESHLETS=%d", passDesc.renderConfig.UseMeshlets ? 1 : 0);

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({
				L"geo-raster/gbuffer-compute.hlsl",
				L"cs_main",
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
				uint32_t gbuffer0UavIndex;
				uint32_t gbuffer1UavIndex;
				uint32_t gbuffer2UavIndex;
				uint32_t visBufferSrvIndex;
				uint32_t colorTargetUavIndex;
			};

			FPassConstants cb = {};
			cb.gbuffer0UavIndex = passDesc.gbufferTargets[0]->m_descriptorIndices.UAVs[0];
			cb.gbuffer1UavIndex = passDesc.gbufferTargets[1]->m_descriptorIndices.UAVs[0];
			cb.gbuffer2UavIndex = passDesc.gbufferTargets[2]->m_descriptorIndices.UAVs[0];
			cb.visBufferSrvIndex = passDesc.sourceVisBuffer->m_descriptorIndices.SRV;
			cb.colorTargetUavIndex = passDesc.colorTarget->m_descriptorIndices.UAVs[0];

			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(FPassConstants) / 4, &cb, 0);
			d3dCmdList->SetComputeRootConstantBufferView(1, passDesc.viewConstantBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress());
			d3dCmdList->SetComputeRootConstantBufferView(2, passDesc.sceneConstantBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress());

			// Clear the color target
			const uint32_t clearValue[] = { 0, 0, 0, 0 };
			d3dCmdList->ClearUnorderedAccessViewUint(
				RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passDesc.colorTarget->m_descriptorIndices.UAVs[0]),
				RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passDesc.colorTarget->m_descriptorIndices.NonShaderVisibleUAVs[0], false),
				passDesc.colorTarget->m_resource->m_d3dResource,
				clearValue, 0, nullptr);

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