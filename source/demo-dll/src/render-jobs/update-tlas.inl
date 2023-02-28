namespace RenderJob
{
	Result UpdateTLAS(RenderJob::Sync* jobSync, const FScene* scene)
	{
		size_t renderToken = jobSync->GetToken();
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"tlas_update_job", D3D12_COMMAND_LIST_TYPE_DIRECT);

		Result passResult;
		passResult.m_syncObj = cmdList->GetSync();
		passResult.m_task = concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_tlas_update", PIX_COLOR_DEFAULT);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "update_tlas", PIX_COLOR_DEFAULT);

			std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
			instanceDescs.reserve(scene->m_sceneMeshes.GetCount());
			int instanceIndex = 0;

			for (int meshIndex = 0; meshIndex < scene->m_sceneMeshes.GetCount(); ++meshIndex)
			{
				const FMesh& mesh = scene->m_sceneMeshes.m_entityList[meshIndex];
				const std::string& meshName = scene->m_sceneMeshes.m_entityNames[meshIndex];

				const auto& search = scene->m_blasList.find(meshName);
				DebugAssert(search != scene->m_blasList.end());

				D3D12_RAYTRACING_INSTANCE_DESC instance = {};
				instance.InstanceID = 0;
				instance.InstanceContributionToHitGroupIndex = 0; // specify 0 because we will use InstanceIndex() in shader directly
				instance.InstanceMask = 1;
				instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
				instance.AccelerationStructure = search->second->m_resource->m_d3dResource->GetGPUVirtualAddress();

				// Transpose and convert to 3x4 matrix
				const Matrix& localToWorld = scene->m_sceneMeshes.m_transformList[meshIndex] * scene->m_rootTransform;
				decltype(instance.Transform)& dest = instance.Transform;
				dest[0][0] = localToWorld._11;	dest[1][0] = localToWorld._12;	dest[2][0] = localToWorld._13;
				dest[0][1] = localToWorld._21;	dest[1][1] = localToWorld._22;	dest[2][1] = localToWorld._23;
				dest[0][2] = localToWorld._31;	dest[1][2] = localToWorld._32;	dest[2][2] = localToWorld._33;
				dest[0][3] = localToWorld._41;	dest[1][3] = localToWorld._42;	dest[2][3] = localToWorld._43;

				instanceDescs.push_back(instance);
			}

			const size_t instanceDescBufferSize = instanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
			std::unique_ptr<FSystemBuffer> instanceDescBuffer{ RenderBackend12::CreateNewSystemBuffer(
				L"instance_descs_buffer",
				FResource::AccessMode::CpuWriteOnly,
				instanceDescBufferSize,
				cmdList->GetFence(FCommandList::SyncPoint::GpuFinish),
				[pData = instanceDescs.data(), instanceDescBufferSize](uint8_t* pDest)
				{
					memcpy(pDest, pData, instanceDescBufferSize);
				}) };

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputsDesc = {};
			tlasInputsDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
			tlasInputsDesc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			tlasInputsDesc.InstanceDescs = instanceDescBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress();
			tlasInputsDesc.NumDescs = instanceDescs.size();
			tlasInputsDesc.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPreBuildInfo = {};
			RenderBackend12::GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputsDesc, &tlasPreBuildInfo);

			// TLAS scratch buffer
			std::unique_ptr<FShaderBuffer> tlasScratch{ RenderBackend12::CreateNewShaderBuffer(
				L"tlas_scratch",
				FShaderBuffer::Type::AccelerationStructure,
				FResource::AccessMode::GpuWriteOnly,
				FResource::Allocation::Transient(cmdList->GetFence(FCommandList::SyncPoint::GpuFinish)),
				GetAlignedSize(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, tlasPreBuildInfo.ScratchDataSizeInBytes)) };

			// Build TLAS
			scene->m_tlas->m_resource->UavBarrier(cmdList);
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
			buildDesc.Inputs = tlasInputsDesc;
			buildDesc.ScratchAccelerationStructureData = tlasScratch->m_resource->m_d3dResource->GetGPUVirtualAddress();
			buildDesc.DestAccelerationStructureData = scene->m_tlas->m_resource->m_d3dResource->GetGPUVirtualAddress();
			cmdList->m_d3dCmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
			scene->m_tlas->m_resource->UavBarrier(cmdList);

			return cmdList;

		}).then([=](FCommandList* recordedCl) mutable
		{
			jobSync->Execute(renderToken, recordedCl);
		});

		return passResult;
	}
}
