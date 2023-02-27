namespace RenderJob
{
	struct MSAAResolveDesc
	{
		FShaderSurface* colorSource;
		FShaderSurface* colorTarget;
		uint32_t resX;
		uint32_t resY;
		DXGI_FORMAT format;
	};

	Result MSAAResolve(RenderJob::Sync* jobSync, const MSAAResolveDesc& passDesc)
	{
		size_t renderToken = jobSync->GetToken();
		size_t colorSourceTransitionToken = passDesc.colorSource->m_resource->GetTransitionToken();
		size_t colorTargetTransitionToken = passDesc.colorTarget->m_resource->GetTransitionToken();
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"msaa_resolve_job", D3D12_COMMAND_LIST_TYPE_DIRECT);

		Result passResult;
		passResult.m_syncObj = cmdList->GetSync();
		passResult.m_task = concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_msaa_resolve", PIX_COLOR_DEFAULT);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "msaa_resolve", 0);

			// MSAA resolve
			passDesc.colorSource->m_resource->Transition(cmdList, colorSourceTransitionToken, 0, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
			passDesc.colorTarget->m_resource->Transition(cmdList, colorTargetTransitionToken, 0, D3D12_RESOURCE_STATE_RESOLVE_DEST);
			d3dCmdList->ResolveSubresource(
				passDesc.colorTarget->m_resource->m_d3dResource,
				0,
				passDesc.colorSource->m_resource->m_d3dResource,
				0,
				passDesc.format);

			return cmdList;

		}).then([=](FCommandList* recordedCl) mutable
		{
			jobSync->Execute(renderToken, recordedCl);
		});

		return passResult;
	}
}