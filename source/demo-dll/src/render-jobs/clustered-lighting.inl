namespace RenderJob
{
	struct ClusteredLightingDesc
	{
		FShaderBuffer* lightListsBuffer;
		FShaderBuffer* lightGridBuffer;
		FShaderSurface* colorTarget;
		FShaderSurface* depthStencilTex;
		FShaderSurface* gbufferBaseColorTex;
		FShaderSurface* gbufferNormalsTex;
		FShaderSurface* gbufferMetallicRoughnessAoTex;
		FConfig renderConfig;
		const FScene* scene;
		const FView* view;
		Vector2 jitter;
		uint32_t resX;
		uint32_t resY;
	};

	concurrency::task<void> ClusteredLighting(RenderJob::Sync& jobSync, const ClusteredLightingDesc& passDesc, const bool bRequiresClear)
	{
		size_t renderToken = jobSync.GetToken();
		size_t lightListsBufferTransitionToken = passDesc.lightListsBuffer->m_resource->GetTransitionToken();
		size_t lightGridBufferTransitionToken = passDesc.lightGridBuffer->m_resource->GetTransitionToken();
		size_t colorTargetTransitionToken = passDesc.colorTarget->m_resource->GetTransitionToken();
		size_t depthTexTransitionToken = passDesc.depthStencilTex->m_resource->GetTransitionToken();
		size_t gbufferBaseColorTransitionToken = passDesc.gbufferBaseColorTex->m_resource->GetTransitionToken();
		size_t gbufferNormalsTransitionToken = passDesc.gbufferNormalsTex->m_resource->GetTransitionToken();
		size_t gbufferMetallicRoughnessAoTransitionToken = passDesc.gbufferMetallicRoughnessAoTex->m_resource->GetTransitionToken();

		return concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("clustered_lighting", PIX_COLOR_DEFAULT);
			FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"clustered_lighting", D3D12_COMMAND_LIST_TYPE_DIRECT);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "clustered_lighting", 0);

			// Transitions
			passDesc.lightListsBuffer->m_resource->Transition(cmdList, lightListsBufferTransitionToken, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			passDesc.lightGridBuffer->m_resource->Transition(cmdList, lightGridBufferTransitionToken, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			passDesc.colorTarget->m_resource->Transition(cmdList, colorTargetTransitionToken, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			passDesc.depthStencilTex->m_resource->Transition(cmdList, depthTexTransitionToken, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			passDesc.gbufferBaseColorTex->m_resource->Transition(cmdList, gbufferBaseColorTransitionToken, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			passDesc.gbufferNormalsTex->m_resource->Transition(cmdList, gbufferNormalsTransitionToken, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			passDesc.gbufferMetallicRoughnessAoTex->m_resource->Transition(cmdList, gbufferMetallicRoughnessAoTransitionToken, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"clustered_lighting_rootsig",
				cmdList,
				FRootsigDesc{ L"lighting/clustered-lighting.hlsl", L"rootsig", L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

			IDxcBlob* csBlob = RenderBackend12::CacheShader({
			L"lighting/clustered-lighting.hlsl",
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

			// Root Constants
			struct Constants
			{
				uint32_t lightListsBufferSrvIndex;
				uint32_t lightGridBufferSrvIndex;
				uint32_t colorTargetUavIndex;
				uint32_t depthTargetSrvIndex;
				uint32_t gbufferBaseColorSrvIndex;
				uint32_t gbufferNormalsSrvIndex;
				uint32_t gbufferMetallicRoughnessAoSrvIndex;
				uint32_t packedLightTransformsBufferIndex;
				uint32_t packedGlobalLightPropertiesBufferIndex;
				uint32_t resX;
				uint32_t resY;
				uint32_t sceneBvhIndex;
				Vector3 eyePos;
				uint32_t __pad0;
				Matrix invViewProjTransform;
			};

			std::unique_ptr<FUploadBuffer> cbuf = RenderBackend12::CreateUploadBuffer(
				L"clustered_lighting_cb",
				sizeof(Constants),
				cmdList,
				[passDesc](uint8_t* pDest)
				{
					auto cb = reinterpret_cast<Constants*>(pDest);
					cb->lightListsBufferSrvIndex = passDesc.lightListsBuffer->m_srvIndex;
					cb->lightGridBufferSrvIndex = passDesc.lightGridBuffer->m_srvIndex;
					cb->colorTargetUavIndex = passDesc.colorTarget->m_uavIndices[0];
					cb->depthTargetSrvIndex = passDesc.depthStencilTex->m_srvIndex;
					cb->gbufferBaseColorSrvIndex = passDesc.gbufferBaseColorTex->m_srvIndex;
					cb->gbufferNormalsSrvIndex = passDesc.gbufferNormalsTex->m_srvIndex;
					cb->gbufferMetallicRoughnessAoSrvIndex = passDesc.gbufferMetallicRoughnessAoTex->m_srvIndex;
					cb->packedLightTransformsBufferIndex = passDesc.scene->m_packedLightTransforms->m_srvIndex;
					cb->packedGlobalLightPropertiesBufferIndex = passDesc.scene->m_packedGlobalLightProperties->m_srvIndex;
					cb->resX = passDesc.resX;
					cb->resY = passDesc.resY;
					cb->sceneBvhIndex = passDesc.scene->m_tlas->m_srvIndex;
					cb->eyePos = passDesc.view->m_position;
					cb->invViewProjTransform = (passDesc.view->m_viewTransform * passDesc.view->m_projectionTransform * Matrix::CreateTranslation(passDesc.jitter.x, passDesc.jitter.y, 0.f)).Invert();
				});

			d3dCmdList->SetComputeRootConstantBufferView(0, cbuf->m_resource->m_d3dResource->GetGPUVirtualAddress());

			if (bRequiresClear)
			{
				// Clear the color target
				const uint32_t clearValue[] = { 0, 0, 0, 0 };
				d3dCmdList->ClearUnorderedAccessViewUint(
					RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passDesc.colorTarget->m_uavIndices[0]),
					RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passDesc.colorTarget->m_nonShaderVisibleUavIndices[0], false),
					passDesc.colorTarget->m_resource->m_d3dResource,
					clearValue, 0, nullptr);
			}

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