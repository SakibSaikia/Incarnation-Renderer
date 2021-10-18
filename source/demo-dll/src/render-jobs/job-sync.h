#pragma once

namespace RenderJob
{
	struct Sync
	{
		winrt::com_ptr<D3DFence_t> m_fence;
		size_t m_fenceValue;
		std::mutex m_mutex;

		Sync() : m_fenceValue{ 0 }
		{
			AssertIfFailed(RenderBackend12::GetDevice()->CreateFence(
				0,
				D3D12_FENCE_FLAG_NONE,
				IID_PPV_ARGS(m_fence.put())));
		}

		// Each render job gets a token for execution which determines the order in which 
		// it will be submitted for rendering to the API
		size_t GetToken()
		{
			return ++m_fenceValue;
		}

		// Executes commandlist with token-based ordering
		void Execute(const size_t token, FCommandList* cmdList)
		{
			const size_t completedFenceValue = m_fence->GetCompletedValue();
			const size_t wait = token > 0 ? token - 1 : 0;
			if (completedFenceValue < wait)
			{
				SCOPED_CPU_EVENT("wait_turn", PIX_COLOR_DEFAULT);
				HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
				if (event)
				{
					m_fence->SetEventOnCompletion(wait, event);
					WaitForSingleObject(event, INFINITE);
				}
			}

			// Aquire mutex before modifying shared state
			std::lock_guard<std::mutex> scopeLock{ m_mutex };

			RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
			m_fence->Signal(token);
		}
	};
}