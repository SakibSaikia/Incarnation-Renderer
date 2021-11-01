namespace RenderJob
{
	struct PathTracingDesc
	{
		FBindlessUav* target;
		uint32_t resX;
		uint32_t resY;
		const FScene* scene;
		const FView* view;
	};

	concurrency::task<void> PathTrace(RenderJob::Sync& jobSync, const PathTracingDesc& passDesc)
	{
		size_t renderToken = jobSync.GetToken();
		size_t uavTransitionToken = passDesc.target->m_resource->GetTransitionToken();

		return concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_path_tracing", PIX_COLOR_DEFAULT);

			FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			cmdList->SetName(L"path_tracing_job");
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "path_tracing", PIX_COLOR_DEFAULT);

			// Compile the lib
			IDxcBlob* rtLib = RenderBackend12::CacheShader({ L"raytracing/pathtracing.hlsl", L"", L"" , L"lib_6_6"});

			// Define lib exports
			D3D12_EXPORT_DESC exports[] = {
				{L"rgsMain", nullptr, D3D12_EXPORT_FLAG_NONE },
				{L"chsMain",nullptr, D3D12_EXPORT_FLAG_NONE },
				{L"msMain", nullptr, D3D12_EXPORT_FLAG_NONE },
				{L"k_globalRootsig", nullptr, D3D12_EXPORT_FLAG_NONE},
				{L"k_hitGroupLocalRootsig", nullptr, D3D12_EXPORT_FLAG_NONE},
				{L"k_hitGroup", nullptr, D3D12_EXPORT_FLAG_NONE},
				{L"k_hitGroupLocalRootsigAssociation", nullptr, D3D12_EXPORT_FLAG_NONE},
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
			std::unique_ptr<FTransientBuffer> raygenShaderTable = RenderBackend12::CreateTransientBuffer(
				L"raygen_sbt",
				1 * raygenShaderRecordSize,
				cmdList,
				[shaderId = psoInfo->GetShaderIdentifier(L"rgsMain")](uint8_t* pDest)
				{
					memcpy(pDest, shaderId, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
				});

			// Miss shader table
			const size_t missShaderRecordSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
			std::unique_ptr<FTransientBuffer> missShaderTable = RenderBackend12::CreateTransientBuffer(
				L"miss_sbt",
				1 * missShaderRecordSize,
				cmdList,
				[shaderId = psoInfo->GetShaderIdentifier(L"msMain")](uint8_t* pDest)
				{
					memcpy(pDest, shaderId, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
				});

			// Hit group shader table
			struct HitGroupRootConstants
			{
				int indexAccessor;
				int positionAccessor;
				int uvAccessor;
				int normalAccessor;
				int tangentAccessor;
				int materialIndex;
			};

			const size_t hitGroupShaderRecordSize = GetAlignedSize(D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + sizeof(HitGroupRootConstants));
			const size_t numHitGroups = passDesc.scene->m_entities.GetScenePrimitiveCount();
			std::unique_ptr<FTransientBuffer> hitGroupShaderTable = RenderBackend12::CreateTransientBuffer(
				L"hit_sbt",
				numHitGroups * hitGroupShaderRecordSize,
				cmdList,
				[&, shaderId = psoInfo->GetShaderIdentifier(L"k_hitGroup")](uint8_t* pDest)
				{	
					for (int meshIndex = 0; meshIndex < passDesc.scene->m_entities.m_meshList.size(); ++meshIndex)
					{
						const FMesh& mesh = passDesc.scene->m_entities.m_meshList[meshIndex];
						for (const FMeshPrimitive& primitive : mesh.m_primitives)
						{
							HitGroupRootConstants args = {};
							args.indexAccessor = primitive.m_indexAccessor;
							args.positionAccessor = primitive.m_positionAccessor;
							args.uvAccessor = primitive.m_uvAccessor;
							args.normalAccessor = primitive.m_normalAccessor;
							args.tangentAccessor = primitive.m_tangentAccessor;
							args.materialIndex = primitive.m_materialIndex;

							memcpy(pDest, shaderId, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
							memcpy(pDest + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, &args, sizeof(args));
							pDest += hitGroupShaderRecordSize;
						}
					}
				});

			// Descriptor heaps
			D3DDescriptorHeap_t* descriptorHeaps[] =
			{
				RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
				RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
			};
			d3dCmdList->SetDescriptorHeaps(2, descriptorHeaps);

			// Global Root Signature
			winrt::com_ptr<D3DRootSignature_t> globalRootsig = RenderBackend12::FetchRootSignature(rtLib);
			d3dCmdList->SetComputeRootSignature(globalRootsig.get());

			// Root signature arguments
			struct GlobalCbLayout
			{
				int sceneMeshAccessorsIndex;
				int sceneMeshBufferViewsIndex;
				int sceneMaterialBufferIndex;
				int sceneBvhIndex;
				Vector3 cameraPosition;
				int destUavIndex;
				Matrix projectionToWorld;
				int envmapTextureIndex;
			};

			std::unique_ptr<FTransientBuffer> globalCb = RenderBackend12::CreateTransientBuffer(
				L"global_cb",
				sizeof(GlobalCbLayout),
				cmdList,
				[passDesc](uint8_t* pDest)
				{
					auto cbDest = reinterpret_cast<GlobalCbLayout*>(pDest);
					cbDest->destUavIndex = passDesc.target->m_uavIndices[0];
					cbDest->sceneMeshAccessorsIndex = passDesc.scene->m_packedMeshAccessors->m_srvIndex;
					cbDest->sceneMeshBufferViewsIndex = passDesc.scene->m_packedMeshBufferViews->m_srvIndex;
					cbDest->sceneMaterialBufferIndex = passDesc.scene->m_packedMaterials->m_srvIndex;
					cbDest->sceneBvhIndex = passDesc.scene->m_tlas->m_srvIndex;
					cbDest->cameraPosition = passDesc.view->m_position;
					cbDest->projectionToWorld = (passDesc.view->m_viewTransform * passDesc.view->m_projectionTransform).Invert();
					cbDest->envmapTextureIndex = passDesc.scene->m_globalLightProbe.m_envmapTextureIndex;
				});

			d3dCmdList->SetComputeRootConstantBufferView(0, globalCb->m_resource->m_d3dResource->GetGPUVirtualAddress());
			d3dCmdList->SetComputeRootShaderResourceView(1, passDesc.scene->m_tlas->m_resource->m_d3dResource->GetGPUVirtualAddress());

			// Transitions
			passDesc.target->m_resource->Transition(cmdList, uavTransitionToken, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// Dispatch rays
			D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
			dispatchDesc.HitGroupTable.StartAddress = hitGroupShaderTable->m_resource->m_d3dResource->GetGPUVirtualAddress();
			dispatchDesc.HitGroupTable.SizeInBytes = numHitGroups * hitGroupShaderRecordSize;
			dispatchDesc.HitGroupTable.StrideInBytes = hitGroupShaderRecordSize;
			dispatchDesc.MissShaderTable.StartAddress = missShaderTable->m_resource->m_d3dResource->GetGPUVirtualAddress();
			dispatchDesc.MissShaderTable.SizeInBytes = 1 * missShaderRecordSize;
			dispatchDesc.MissShaderTable.StrideInBytes = missShaderRecordSize;
			dispatchDesc.RayGenerationShaderRecord.StartAddress = raygenShaderTable->m_resource->m_d3dResource->GetGPUVirtualAddress();
			dispatchDesc.RayGenerationShaderRecord.SizeInBytes = 1 * raygenShaderRecordSize;
			dispatchDesc.Width = passDesc.resX;
			dispatchDesc.Height = passDesc.resY;
			dispatchDesc.Depth = 1;
			d3dCmdList->DispatchRays(&dispatchDesc);

			return cmdList;

		}).then([&, renderToken](FCommandList* recordedCl) mutable
		{
			jobSync.Execute(renderToken, recordedCl);
		});
	}
}