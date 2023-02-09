namespace RenderJob
{
	struct PathTracingDesc
	{
		FShaderSurface* targetBuffer;
		FShaderSurface* historyBuffer;
		FShaderBuffer* lightPropertiesBuffer;
		FShaderBuffer* lightTransformsBuffer;
		uint32_t currentSampleIndex;
		uint32_t resX;
		uint32_t resY;
		const FScene* scene;
		const FView* view;
		FConfig renderConfig;
	};

	concurrency::task<void> PathTrace(RenderJob::Sync* jobSync, const PathTracingDesc& passDesc)
	{
		size_t renderToken = jobSync->GetToken();
		size_t uavTransitionToken = passDesc.targetBuffer->m_resource->GetTransitionToken();

		return concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_path_tracing", PIX_COLOR_DEFAULT);
			FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"path_tracing_job", D3D12_COMMAND_LIST_TYPE_DIRECT);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "path_tracing", PIX_COLOR_DEFAULT);

			std::wstring shaderMacros = PrintString(
				L"PATH_TRACING=1 VIEWMODE=%d DIRECT_LIGHTING=%d ENV_SKY_MODE=%d",
				passDesc.renderConfig.Viewmode,
				passDesc.renderConfig.EnableDirectLighting ? 1 : 0,
				passDesc.renderConfig.EnvSkyMode);

			// Compile the lib
			IDxcBlob* rtLib = RenderBackend12::CacheShader({ L"raytracing/pathtracing.hlsl", L"", shaderMacros , L"lib_6_6"});

			// Define lib exports
			D3D12_EXPORT_DESC exports[] = {
				{L"rgsMain", nullptr, D3D12_EXPORT_FLAG_NONE },
				{L"ahsMain",nullptr, D3D12_EXPORT_FLAG_NONE },
				{L"chsMain",nullptr, D3D12_EXPORT_FLAG_NONE },
				{L"msEnvmap", nullptr, D3D12_EXPORT_FLAG_NONE },
				{L"msDynamicSky", nullptr, D3D12_EXPORT_FLAG_NONE },
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
			std::unique_ptr<FSystemBuffer> raygenShaderTable{ RenderBackend12::CreateNewSystemBuffer(
				L"raygen_sbt",
				ResourceAccessMode::CpuWriteOnly,
				1 * raygenShaderRecordSize,
				cmdList->GetFence(FCommandList::FenceType::GpuFinish),
				[shaderId = psoInfo->GetShaderIdentifier(L"rgsMain")](uint8_t* pDest)
				{
					memcpy(pDest, shaderId, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
				}) };

			// Miss shader table
			const size_t missShaderRecordSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
			std::unique_ptr<FSystemBuffer> missShaderTable{ RenderBackend12::CreateNewSystemBuffer(
				L"miss_sbt",
				ResourceAccessMode::CpuWriteOnly,
				3 * missShaderRecordSize,
				cmdList->GetFence(FCommandList::FenceType::GpuFinish),
				[&psoInfo](uint8_t* pDest)
				{
					memcpy(pDest, psoInfo->GetShaderIdentifier(L"msEnvmap"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
					memcpy(pDest + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, psoInfo->GetShaderIdentifier(L"msDynamicSky"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
					memcpy(pDest + 2 * D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, psoInfo->GetShaderIdentifier(L"msShadow"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
				}) };

			// Hit shader table
			const size_t hitGroupShaderRecordSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
			std::unique_ptr<FSystemBuffer> hitGroupShaderTable{ RenderBackend12::CreateNewSystemBuffer(
				L"hit_sbt",
				ResourceAccessMode::CpuWriteOnly,
				2 * hitGroupShaderRecordSize,
				cmdList->GetFence(FCommandList::FenceType::GpuFinish),
				[&psoInfo](uint8_t* pDest)
				{
					memcpy(pDest, psoInfo->GetShaderIdentifier(L"k_hitGroup"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
					memcpy(pDest + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, psoInfo->GetShaderIdentifier(L"k_shadowHitGroup"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
				}) };

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

			FPerezDistribution perezConstants;
			const float t = passDesc.renderConfig.Turbidity;
			perezConstants.A = Vector4(0.1787 * t - 1.4630, -0.0193 * t - 0.2592, -0.0167 * t - 0.2608, 0.f);
			perezConstants.B = Vector4(-0.3554 * t + 0.4275, -0.0665 * t + 0.0008, -0.0950 * t + 0.0092, 0.f);
			perezConstants.C = Vector4(-0.0227 * t + 5.3251, -0.0004 * t + 0.2125, -0.0079 * t + 0.2102, 0.f);
			perezConstants.D = Vector4(0.1206 * t - 2.5771, -0.0641 * t - 0.8989, -0.0441 * t - 1.6537, 0.f);
			perezConstants.E = Vector4(-0.0670 * t + 0.3703, -0.0033 * t + 0.0452, -0.0109 * t + 0.0529, 0.f);

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
				FPerezDistribution perez;
				float turbidity;
				Vector3 sunDir;
			};

			std::unique_ptr<FSystemBuffer> globalCb{ RenderBackend12::CreateNewSystemBuffer(
				L"global_cb",
				ResourceAccessMode::CpuWriteOnly,
				sizeof(GlobalCbLayout),
				cmdList->GetFence(FCommandList::FenceType::GpuFinish),
				[passDesc, perezConstants](uint8_t* pDest)
				{
					const int lightCount = passDesc.scene->m_sceneLights.GetCount();

					// Sun direction
					Vector4 L = Vector4(1, 0.1, 1, 0);
					int sun = passDesc.scene->GetDirectionalLight();
					if (sun != -1)
					{
						Matrix sunTransform = passDesc.scene->m_sceneLights.m_transformList[sun];
						sunTransform.Translation(Vector3::Zero);
						L = Vector4::Transform(Vector4(0, 0, -1, 0), sunTransform);
					}
					L.Normalize();

					auto cbDest = reinterpret_cast<GlobalCbLayout*>(pDest);
					cbDest->destUavIndex = passDesc.targetBuffer->m_descriptorIndices.UAVs[0];
					cbDest->sceneMeshAccessorsIndex = passDesc.scene->m_packedMeshAccessors->m_descriptorIndices.SRV;
					cbDest->sceneMeshBufferViewsIndex = passDesc.scene->m_packedMeshBufferViews->m_descriptorIndices.SRV;
					cbDest->sceneMaterialBufferIndex = passDesc.scene->m_packedMaterials->m_descriptorIndices.SRV;
					cbDest->sceneBvhIndex = passDesc.scene->m_tlas->m_descriptorIndices.SRV;
					cbDest->cameraAperture = passDesc.renderConfig.Pathtracing_CameraAperture;
					cbDest->cameraFocalLength = passDesc.renderConfig.Pathtracing_CameraFocalLength;
					cbDest->lightCount = passDesc.scene->m_globalLightList.size();
					cbDest->projectionToWorld = (passDesc.view->m_viewTransform * passDesc.view->m_projectionTransform).Invert();
					cbDest->sceneRotation = passDesc.scene->m_rootTransform;
					cbDest->cameraMatrix = passDesc.view->m_viewTransform.Invert();
					cbDest->envmapTextureIndex = passDesc.scene->m_skylight.m_envmapTextureIndex;
					cbDest->scenePrimitivesIndex = passDesc.scene->m_packedPrimitives->m_descriptorIndices.SRV;
					cbDest->scenePrimitiveCountsIndex = passDesc.scene->m_packedPrimitiveCounts->m_descriptorIndices.SRV;
					cbDest->currentSampleIndex = passDesc.currentSampleIndex;
					cbDest->sqrtSampleCount = std::sqrt(passDesc.renderConfig.MaxSampleCount);
					cbDest->globalLightPropertiesBufferIndex = lightCount > 0 ? passDesc.lightPropertiesBuffer->m_descriptorIndices.SRV : -1;
					cbDest->sceneLightIndicesBufferIndex = lightCount > 0 ? passDesc.scene->m_packedLightIndices->m_descriptorIndices.SRV : -1;
					cbDest->sceneLightsTransformsBufferIndex = lightCount > 0 ? passDesc.lightTransformsBuffer->m_descriptorIndices.SRV : -1;
					cbDest->perez = perezConstants;
					cbDest->turbidity = passDesc.renderConfig.Turbidity;
					cbDest->sunDir = Vector3(L);
				}) };

			d3dCmdList->SetComputeRootConstantBufferView(0, globalCb->m_resource->m_d3dResource->GetGPUVirtualAddress());
			d3dCmdList->SetComputeRootShaderResourceView(1, passDesc.scene->m_tlas->m_resource->m_d3dResource->GetGPUVirtualAddress());

			// Transitions
			passDesc.targetBuffer->m_resource->Transition(cmdList, uavTransitionToken, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			const float clearValue[] = { 0.f, 0.f, 0.f, 0.f };
			d3dCmdList->ClearUnorderedAccessViewFloat(
				RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passDesc.targetBuffer->m_descriptorIndices.UAVs[0]),
				RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, passDesc.targetBuffer->m_descriptorIndices.NonShaderVisibleUAVs[0], false),
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
				passDesc.targetBuffer->m_descriptorIndices.SRV,
				passDesc.historyBuffer->m_descriptorIndices.UAVs[0],
				passDesc.currentSampleIndex,
				passDesc.resX,
				passDesc.resY
			};

			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);
			size_t threadGroupCountX = std::max<size_t>(std::ceil(passDesc.resX / 16), 1);
			size_t threadGroupCountY = std::max<size_t>(std::ceil(passDesc.resY / 16), 1);
			d3dCmdList->Dispatch(threadGroupCountX, threadGroupCountY, 1);

			return cmdList;

		}).then([=](FCommandList* recordedCl) mutable
		{
			jobSync->Execute(renderToken, recordedCl);
		});
	}
}