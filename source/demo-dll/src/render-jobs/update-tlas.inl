namespace RenderJob
{
	concurrency::task<void> UpdateTLAS(RenderJob::Sync& jobSync, const FScene* scene)
	{
		size_t renderToken = jobSync.GetToken();

		return concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_tlas_update", PIX_COLOR_DEFAULT);

			FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			cmdList->SetName(L"tlas_update_job");
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "update_tlas", PIX_COLOR_DEFAULT);

			std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
			instanceDescs.reserve(scene->m_entities.m_meshList.size());

			for (int meshIndex = 0; meshIndex < scene->m_entities.m_meshList.size(); ++meshIndex)
			{
				const FMesh& mesh = scene->m_entities.m_meshList[meshIndex];
				for (int primitiveIndex = 0; primitiveIndex < mesh.m_primitives.size(); ++primitiveIndex)
				{
					const FMeshPrimitive& primitive = mesh.m_primitives[primitiveIndex];
					const auto& search = scene->m_blasList.find(primitive);
					DebugAssert(search != scene->m_blasList.end());

					D3D12_RAYTRACING_INSTANCE_DESC instance = {};
					instance.InstanceID = 0;
					instance.InstanceContributionToHitGroupIndex = 0;
					instance.InstanceMask = 1;
					instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
					instance.AccelerationStructure = search->second->m_resource->m_d3dResource->GetGPUVirtualAddress();

					// Transpose and convert to 3x4 matrix
					const Matrix& localToWorld = scene->m_entities.m_transformList[meshIndex] * scene->m_rootTransform;
					decltype(instance.Transform)& dest = instance.Transform;
					dest[0][0] = localToWorld._11;	dest[1][0] = localToWorld._12;	dest[2][0] = localToWorld._13;
					dest[0][1] = localToWorld._21;	dest[1][1] = localToWorld._22;	dest[2][1] = localToWorld._23;
					dest[0][2] = localToWorld._31;	dest[1][2] = localToWorld._32;	dest[2][2] = localToWorld._33;
					dest[0][3] = localToWorld._41;	dest[1][3] = localToWorld._42;	dest[2][3] = localToWorld._43;

					instanceDescs.push_back(instance);
				}
			}

			const size_t instanceDescBufferSize = instanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
			auto instanceDescBuffer = RenderBackend12::CreateTransientBuffer(
				L"instance_descs_buffer",
				instanceDescBufferSize,
				cmdList,
				[pData = instanceDescs.data(), instanceDescBufferSize](uint8_t* pDest)
				{
					memcpy(pDest, pData, instanceDescBufferSize);
				});

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputsDesc = {};
			tlasInputsDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
			tlasInputsDesc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			tlasInputsDesc.InstanceDescs = instanceDescBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress();
			tlasInputsDesc.NumDescs = instanceDescs.size();
			tlasInputsDesc.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPreBuildInfo = {};
			RenderBackend12::GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputsDesc, &tlasPreBuildInfo);

			// TLAS scratch buffer
			auto tlasScratch = RenderBackend12::CreateBindlessUavBuffer(
				L"tlas_scratch",
				GetAlignedSize(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, tlasPreBuildInfo.ScratchDataSizeInBytes),
				false);

			// Build TLAS
			scene->m_tlas->m_resource->UavBarrier(cmdList);
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
			buildDesc.Inputs = tlasInputsDesc;
			buildDesc.ScratchAccelerationStructureData = tlasScratch->m_resource->m_d3dResource->GetGPUVirtualAddress();
			buildDesc.DestAccelerationStructureData = scene->m_tlas->m_resource->m_d3dResource->GetGPUVirtualAddress();
			cmdList->m_d3dCmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
			scene->m_tlas->m_resource->UavBarrier(cmdList);

			return cmdList;

		}).then([&, renderToken](FCommandList* recordedCl) mutable
		{
			jobSync.Execute(renderToken, recordedCl);
		});
	}
}