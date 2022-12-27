namespace RenderJob
{
	struct LightCullingDesc
	{
		FShaderBuffer* culledLightCountBuffer;
		FShaderBuffer* culledLightListsBuffer;
		FShaderBuffer* lightGridBuffer;
		FConfig renderConfig;
		const FScene* scene;
		const FView* view;
		Vector2 jitter;
	};

	concurrency::task<void> LightCulling(RenderJob::Sync& jobSync, const LightCullingDesc& passDesc)
	{
		size_t renderToken = jobSync.GetToken();
		size_t culledLightCountBufferTransitionToken = passDesc.culledLightCountBuffer->m_resource->GetTransitionToken();
		size_t culledLightListsBufferTransitionToken = passDesc.culledLightListsBuffer->m_resource->GetTransitionToken();
		size_t lightGridBufferTransitionToken = passDesc.lightGridBuffer->m_resource->GetTransitionToken();

		return concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("light_culling", PIX_COLOR_DEFAULT);
			FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"light_culling", D3D12_COMMAND_LIST_TYPE_DIRECT);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "light_culling", 0);

			// Transitions
			passDesc.culledLightCountBuffer->m_resource->Transition(cmdList, culledLightCountBufferTransitionToken, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			passDesc.culledLightListsBuffer->m_resource->Transition(cmdList, culledLightListsBufferTransitionToken, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			passDesc.lightGridBuffer->m_resource->Transition(cmdList, lightGridBufferTransitionToken, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"light_cull_rootsig",
				cmdList,
				FRootsigDesc{ L"culling/light-culling.hlsl", L"rootsig", L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

			const uint32_t threadGroupSize[3] = { 4, 3, 12 };
			std::wstringstream s;
			s << "THREAD_GROUP_SIZE_X=" << threadGroupSize[0] <<
				" THREAD_GROUP_SIZE_Y=" << threadGroupSize[1] <<
				" THREAD_GROUP_SIZE_Z=" << threadGroupSize[2] <<
				" MAX_LIGHTS_PER_CLUSTER=" << passDesc.renderConfig.MaxLightsPerCluster <<
				" SHOW_LIGHT_BOUNDS=" << passDesc.renderConfig.ShowLightBounds ? 1 : 0;

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({
				L"culling/light-culling.hlsl",
				L"cs_main",
				s.str(),
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
				uint32_t culledLightCountBufferUavIndex;
				uint32_t culledLightListsBufferUavIndex;
				uint32_t lightGridBufferUavIndex;
				uint32_t packedLightIndicesBufferIndex;
				uint32_t packedLightTransformsBufferIndex;
				uint32_t packedGlobalLightPropertiesBufferIndex;
				uint32_t lightCount;
				float clusterDepthExtent;
				uint32_t clusterGridSize[3];
				float cameraNearPlane;
				Matrix projTransform;
				Matrix invViewProjTransform;
			};

			std::unique_ptr<FUploadBuffer> cbuf = RenderBackend12::CreateUploadBuffer(
				L"light_cull_cb",
				sizeof(Constants),
				cmdList->GetFence(),
				[passDesc](uint8_t* pDest)
				{
					auto cb = reinterpret_cast<Constants*>(pDest);
					cb->culledLightCountBufferUavIndex = passDesc.culledLightCountBuffer->m_uavIndex;
					cb->culledLightListsBufferUavIndex = passDesc.culledLightListsBuffer->m_uavIndex;
					cb->lightGridBufferUavIndex = passDesc.lightGridBuffer->m_uavIndex;
					cb->packedLightIndicesBufferIndex = passDesc.scene->m_packedLightIndices->m_srvIndex;
					cb->packedLightTransformsBufferIndex = passDesc.scene->m_packedLightTransforms->m_srvIndex;
					cb->packedGlobalLightPropertiesBufferIndex = passDesc.scene->m_packedGlobalLightProperties->m_srvIndex;
					cb->lightCount = (uint32_t)passDesc.scene->m_sceneLights.GetCount();
					cb->clusterDepthExtent = passDesc.renderConfig.ClusterDepthExtent;
					cb->clusterGridSize[0] = (uint32_t)passDesc.renderConfig.LightClusterDimX;
					cb->clusterGridSize[1] = (uint32_t)passDesc.renderConfig.LightClusterDimY;
					cb->clusterGridSize[2] = (uint32_t)passDesc.renderConfig.LightClusterDimZ;
					cb->cameraNearPlane = passDesc.renderConfig.CameraNearPlane;
					cb->projTransform = passDesc.view->m_projectionTransform * Matrix::CreateTranslation(passDesc.jitter.x, passDesc.jitter.y, 0.f);
					cb->invViewProjTransform = (passDesc.view->m_viewTransform * passDesc.view->m_projectionTransform * Matrix::CreateTranslation(passDesc.jitter.x, passDesc.jitter.y, 0.f)).Invert();
				});

			d3dCmdList->SetComputeRootConstantBufferView(0, cbuf->m_resource->m_d3dResource->GetGPUVirtualAddress());

			// Initialize culled light count and lists buffer to 0
			const uint32_t clearValue[] = { 0, 0, 0, 0 };
			d3dCmdList->ClearUnorderedAccessViewUint(
				RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passDesc.culledLightCountBuffer->m_uavIndex),
				RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passDesc.culledLightCountBuffer->m_nonShaderVisibleUavIndex, false),
				passDesc.culledLightCountBuffer->m_resource->m_d3dResource,
				clearValue, 0, nullptr);
			
			d3dCmdList->ClearUnorderedAccessViewUint(
				RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passDesc.culledLightListsBuffer->m_uavIndex),
				RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passDesc.culledLightListsBuffer->m_nonShaderVisibleUavIndex, false),
				passDesc.culledLightListsBuffer->m_resource->m_d3dResource,
				clearValue, 0, nullptr);

			// Dispatch - one thread per cluster
			const uint32_t threadGroupCountX = std::max<size_t>(std::ceil(passDesc.renderConfig.LightClusterDimX / threadGroupSize[0]), 1);
			const uint32_t threadGroupCountY = std::max<size_t>(std::ceil(passDesc.renderConfig.LightClusterDimY / threadGroupSize[1]), 1);
			const uint32_t threadGroupCountZ = std::max<size_t>(std::ceil(passDesc.renderConfig.LightClusterDimZ / threadGroupSize[2]), 1);
			d3dCmdList->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);

			return cmdList;

		}).then([&, renderToken](FCommandList* recordedCl) mutable
		{
			jobSync.Execute(renderToken, recordedCl);
		});
	}
}