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
			FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			cmdList->SetName(L"present_job");

			passDesc.backBuffer->m_resource->Transition(cmdList, transitionToken, 0, D3D12_RESOURCE_STATE_PRESENT);

			return cmdList;

		}).then([&, renderToken](FCommandList* recordedCl) mutable
		{
			jobSync.Execute(renderToken, recordedCl);
		});
	}
}