namespace RenderJob
{
	struct PresentDesc
	{
		FShaderSurface* backBuffer;
	};

	concurrency::task<void> Present(RenderJob::Sync& jobSync, const PresentDesc& passDesc)
	{
		size_t renderToken = jobSync.GetToken();
		size_t transitionToken = passDesc.backBuffer->m_resource->GetTransitionToken();

		return concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record present", PIX_COLOR_DEFAULT);
			FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"present_job", D3D12_COMMAND_LIST_TYPE_DIRECT);
			passDesc.backBuffer->m_resource->Transition(cmdList, transitionToken, 0, D3D12_RESOURCE_STATE_PRESENT);
			return cmdList;

		}).then([&, renderToken](FCommandList* recordedCl) mutable
		{
			jobSync.Execute(renderToken, recordedCl);
		});
	}
}