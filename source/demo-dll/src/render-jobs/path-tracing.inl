namespace RenderJob
{
	struct PathTracingDesc
	{
		FShaderSurface* targetBuffer;
		FShaderSurface* historyBuffer;
		uint32_t currentSampleIndex;
		uint32_t resX;
		uint32_t resY;
		const FScene* scene;
		const FView* view;
		FConfig renderConfig;
	};

	concurrency::task<void> PathTrace(RenderJob::Sync& jobSync, const PathTracingDesc& passDesc)
	{
		size_t renderToken = jobSync.GetToken();
		size_t uavTransitionToken = passDesc.targetBuffer->m_resource->GetTransitionToken();

		return concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_path_tracing", PIX_COLOR_DEFAULT);
			FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"path_tracing_job", D3D12_COMMAND_LIST_TYPE_DIRECT);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "path_tracing", PIX_COLOR_DEFAULT);

			std::wstringstream s;
			s << L"NO_UV_DERIVATIVES=1" <<
				L" VIEWMODE=" << (int)passDesc.renderConfig.Viewmode <<
				L" DIRECT_LIGHTING=" << (passDesc.renderConfig.EnableDirectLighting ? L"1" : L"0");

			// Compile the lib
			IDxcBlob* rtLib = RenderBackend12::CacheShader({ L"raytracing/pathtracing.hlsl", L"", s.str() , L"lib_6_6"});

			// Define lib exports
			D3D12_EXPORT_DESC exports[] = {
				{L"rgsMain", nullptr, D3D12_EXPORT_FLAG_NONE },
				{L"ahsMain",nullptr, D3D12_EXPORT_FLAG_NONE },
				{L"chsMain",nullptr, D3D12_EXPORT_FLAG_NONE },
				{L"msMain", nullptr, D3D12_EXPORT_FLAG_NONE },
				{L"ahsShadow",nullptr, D3D12_EXPORT_FLAG_NONE },
				{L"msShadow", nullptr, D3D12_EXPORT_FLAG_NONE },
				{L"k_globalRootsig", nullptr, D3D12_EXPORT_FLAG_NONE},
				{L"k_hitGroup", nullptr, D3D12_EXPORT_FLAG_NONE},
				{L"k_shadowHitGroup", nullptr, D3D12_EXPORT_FLAG_NONE},
				{L"k_shaderConfig", nullptr, D3D12_EXPORT_FLAG_NONE},
				{L"k_pipelineConfig", nullptr, D3D12_EXPORT_FLAG_NONE} };

			D3D12_DXIL_LIBRARY_DESC libDesc;
			libDesc.DXILLibrary.BytecodeLength = rtLib->GetBufferSize();
			libDesc.DXILLibrary.pShaderBytecode = rtLib->GetBufferPointer();
			libDesc.NumExports = std::size(exports);
			libDesc.pExports = exports;

			// Single subobject that contains all the exports
			D3D12_STATE_SUBOBJECT subObject;
			subObject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
			subObject.pDesc = &libDesc;

			// Complete pipeline state
			D3D12_STATE_OBJECT_DESC pipelineDesc = {};
			pipelineDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
			pipelineDesc.NumSubobjects = 1;
			pipelineDesc.pSubobjects = &subObject;

			// Set PSO
			D3DStateObject_t* pso = RenderBackend12::FetchRaytracePipelineState(pipelineDesc);
			d3dCmdList->SetPipelineState1(pso);

			// PSO reflection
			winrt::com_ptr<D3DStateObjectProperties_t> psoInfo;
			AssertIfFailed(pso->QueryInterface(IID_PPV_ARGS(psoInfo.put())));

			// Raygen shader table
			const size_t raygenShaderRecordSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
			std::unique_ptr<FUploadBuffer> raygenShaderTable = RenderBackend12::CreateUploadBuffer(
				L"raygen_sbt",
				1 * raygenShaderRecordSize,
				cmdList,
				[shaderId = psoInfo->GetShaderIdentifier(L"rgsMain")](uint8_t* pDest)
				{
					memcpy(pDest, shaderId, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
				});

			// Miss shader table
			const size_t missShaderRecordSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
			std::unique_ptr<FUploadBuffer> missShaderTable = RenderBackend12::CreateUploadBuffer(
				L"miss_sbt",
				2 * missShaderRecordSize,
				cmdList,
				[&psoInfo](uint8_t* pDest)
				{
					memcpy(pDest, psoInfo->GetShaderIdentifier(L"msMain"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
					memcpy(pDest + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, psoInfo->GetShaderIdentifier(L"msShadow"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
				});

			// Hit shader table
			const size_t hitGroupShaderRecordSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
			std::unique_ptr<FUploadBuffer> hitGroupShaderTable = RenderBackend12::CreateUploadBuffer(
				L"hit_sbt",
				2 * hitGroupShaderRecordSize,
				cmdList,
				[&psoInfo](uint8_t* pDest)
				{
					memcpy(pDest, psoInfo->GetShaderIdentifier(L"k_hitGroup"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
					memcpy(pDest + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, psoInfo->GetShaderIdentifier(L"k_shadowHitGroup"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
				});

			// Descriptor heaps
			D3DDescriptorHeap_t* descriptorHeaps[] =
			{
				RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
				RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
			};
			d3dCmdList->SetDescriptorHeaps(2, descriptorHeaps);

			// Global Root Signature
			std::unique_ptr<FRootSignature> globalRootsig = RenderBackend12::FetchRootSignature(L"pathtrace_rootsig", cmdList, rtLib);
			d3dCmdList->SetComputeRootSignature(globalRootsig->m_rootsig);

			// Root signature arguments
			struct GlobalCbLayout
			{
				int sceneMeshAccessorsIndex;
				int sceneMeshBufferViewsIndex;
				int sceneMaterialBufferIndex;
				int sceneBvhIndex;
				float cameraAperture;
				float cameraFocalLength;
				int lightCount;
				int destUavIndex;
				Matrix projectionToWorld;
				Matrix sceneRotation;
				Matrix cameraMatrix;
				int envmapTextureIndex;
				int scenePrimitivesIndex;
				int scenePrimitiveCountsIndex;
				uint32_t currentSampleIndex;
				uint32_t sqrtSampleCount;
				int globalLightPropertiesBufferIndex;
				int sceneLightIndicesBufferIndex;
				int sceneLightsTransformsBufferIndex;
			};

			std::unique_ptr<FUploadBuffer> globalCb = RenderBackend12::CreateUploadBuffer(
				L"global_cb",
				sizeof(GlobalCbLayout),
				cmdList,
				[passDesc](uint8_t* pDest)
				{
					const int lightCount = passDesc.scene->m_sceneLights.GetCount();

					auto cbDest = reinterpret_cast<GlobalCbLayout*>(pDest);
					cbDest->destUavIndex = passDesc.targetBuffer->m_uavIndices[0];
					cbDest->sceneMeshAccessorsIndex = passDesc.scene->m_packedMeshAccessors->m_srvIndex;
					cbDest->sceneMeshBufferViewsIndex = passDesc.scene->m_packedMeshBufferViews->m_srvIndex;
					cbDest->sceneMaterialBufferIndex = passDesc.scene->m_packedMaterials->m_srvIndex;
					cbDest->sceneBvhIndex = passDesc.scene->m_tlas->m_srvIndex;
					cbDest->cameraAperture = passDesc.renderConfig.Pathtracing_CameraAperture;
					cbDest->cameraFocalLength = passDesc.renderConfig.Pathtracing_CameraFocalLength;
					cbDest->lightCount = passDesc.scene->m_globalLightList.size();
					cbDest->projectionToWorld = (passDesc.view->m_viewTransform * passDesc.view->m_projectionTransform).Invert();
					cbDest->sceneRotation = passDesc.scene->m_rootTransform;
					cbDest->cameraMatrix = passDesc.view->m_viewTransform.Invert();
					cbDest->envmapTextureIndex = passDesc.scene->m_environmentSky.m_envmapTextureIndex;
					cbDest->scenePrimitivesIndex = passDesc.scene->m_packedPrimitives->m_srvIndex;
					cbDest->scenePrimitiveCountsIndex = passDesc.scene->m_packedPrimitiveCounts->m_srvIndex;
					cbDest->currentSampleIndex = passDesc.currentSampleIndex;
					cbDest->sqrtSampleCount = std::sqrt(passDesc.renderConfig.MaxSampleCount);
					cbDest->globalLightPropertiesBufferIndex = lightCount > 0 ? passDesc.scene->m_packedGlobalLightProperties->m_srvIndex : -1;
					cbDest->sceneLightIndicesBufferIndex = lightCount > 0 ? passDesc.scene->m_packedLightIndices->m_srvIndex : -1;
					cbDest->sceneLightsTransformsBufferIndex = lightCount > 0 ? passDesc.scene->m_packedLightTransforms->m_srvIndex : -1;
				});

			d3dCmdList->SetComputeRootConstantBufferView(0, globalCb->m_resource->m_d3dResource->GetGPUVirtualAddress());
			d3dCmdList->SetComputeRootShaderResourceView(1, passDesc.scene->m_tlas->m_resource->m_d3dResource->GetGPUVirtualAddress());

			// Transitions
			passDesc.targetBuffer->m_resource->Transition(cmdList, uavTransitionToken, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			const float clearValue[] = { 0.f, 0.f, 0.f, 0.f };
			d3dCmdList->ClearUnorderedAccessViewFloat(
				RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passDesc.targetBuffer->m_uavIndices[0]),
				RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passDesc.targetBuffer->m_nonShaderVisibleUavIndices[0], false),
				passDesc.targetBuffer->m_resource->m_d3dResource,
				clearValue, 0, nullptr);

			// Dispatch rays
			D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
			dispatchDesc.HitGroupTable.StartAddress = hitGroupShaderTable->m_resource->m_d3dResource->GetGPUVirtualAddress();
			dispatchDesc.HitGroupTable.SizeInBytes = 2 * hitGroupShaderRecordSize;
			dispatchDesc.HitGroupTable.StrideInBytes = hitGroupShaderRecordSize;
			dispatchDesc.MissShaderTable.StartAddress = missShaderTable->m_resource->m_d3dResource->GetGPUVirtualAddress();
			dispatchDesc.MissShaderTable.SizeInBytes = 2 * missShaderRecordSize;
			dispatchDesc.MissShaderTable.StrideInBytes = missShaderRecordSize;
			dispatchDesc.RayGenerationShaderRecord.StartAddress = raygenShaderTable->m_resource->m_d3dResource->GetGPUVirtualAddress();
			dispatchDesc.RayGenerationShaderRecord.SizeInBytes = 1 * raygenShaderRecordSize;
			dispatchDesc.Width = passDesc.resX;
			dispatchDesc.Height = passDesc.resY;
			dispatchDesc.Depth = 1;
			d3dCmdList->DispatchRays(&dispatchDesc);

			// Combine with history buffer to integrate results over time
			passDesc.targetBuffer->m_resource->Transition(cmdList, uavTransitionToken, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"pathtrace_integrate_rootsig",
				cmdList,
				FRootsigDesc { L"raytracing/pathtrace-integrate.hlsl", L"rootsig", L"rootsig_1_1" });
			d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

			IDxcBlob* csBlob = RenderBackend12::CacheShader({
				L"raytracing/pathtrace-integrate.hlsl",
				L"cs_main",
				L"THREAD_GROUP_SIZE_X=16 THREAD_GROUP_SIZE_Y=16",
				L"cs_6_6" });

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = rootsig->m_rootsig;
			psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
			psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			D3DPipelineState_t* newPSO = RenderBackend12::FetchComputePipelineState(psoDesc);
			d3dCmdList->SetPipelineState(newPSO);

			struct
			{
				uint32_t currentBufferTextureIndex;
				uint32_t historyBufferUavIndex;
				uint32_t historyFrameCount;
				uint32_t resX;
				uint32_t resY;
			} rootConstants = { 
				passDesc.targetBuffer->m_srvIndex,
				passDesc.historyBuffer->m_uavIndices[0],
				passDesc.currentSampleIndex,
				passDesc.resX,
				passDesc.resY
			};

			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);
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