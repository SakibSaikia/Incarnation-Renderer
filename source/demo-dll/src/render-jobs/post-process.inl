namespace RenderJob
{
	struct PostprocessPassDesc
	{
		FRenderTexture* colorSource;
		FRenderTexture* colorTarget;
		uint32_t resX;
		uint32_t resY;
		const FView* view;
	};

	concurrency::task<void> Postprocess(RenderJob::Sync& jobSync, const PostprocessPassDesc& passDesc)
	{
		size_t renderToken = jobSync.GetToken();
		size_t colorSourceTransitionToken = passDesc.colorSource->m_resource->GetTransitionToken();
		size_t colorTargetTransitionToken = passDesc.colorTarget->m_resource->GetTransitionToken();

		return concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_postprocess", PIX_COLOR_DEFAULT);

			FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			cmdList->SetName(L"postprocess_job");

			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

			SCOPED_COMMAND_LIST_EVENT(cmdList, "post_process", 0);

			// MSAA resolve
			passDesc.colorSource->m_resource->Transition(cmdList, colorSourceTransitionToken, 0, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
			passDesc.colorTarget->m_resource->Transition(cmdList, colorTargetTransitionToken, 0, D3D12_RESOURCE_STATE_RESOLVE_DEST);
			d3dCmdList->ResolveSubresource(
				passDesc.colorTarget->m_resource->m_d3dResource,
				0,
				passDesc.colorSource->m_resource->m_d3dResource,
				0,
				Config::g_backBufferFormat);

			return cmdList;

		}).then([&, renderToken](FCommandList* recordedCl) mutable
		{
			jobSync.Execute(renderToken, recordedCl);
		});
	}
}