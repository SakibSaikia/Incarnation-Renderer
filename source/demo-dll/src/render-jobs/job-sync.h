#pragma once

#include <backend-d3d12.h>

namespace RenderJob
{
	// Synchronization object used to sync submission of render passes as well as 
	// execution of passes on the GPU across different command queues
	struct Sync
	{
		// Fence for CPU submission of render pass
		winrt::com_ptr<D3DFence_t> m_cpuFence;
		size_t m_cpuFenceValue;

		std::mutex m_mutex;

		Sync() : m_cpuFenceValue{ 0 } 
		{
			AssertIfFailed(RenderBackend12::GetDevice()->CreateFence(
				0,
				D3D12_FENCE_FLAG_NONE,
				IID_PPV_ARGS(m_cpuFence.put())));
		}

		// Each render job gets a token for execution which determines the order in which 
		// it will be submitted for rendering to the API
		size_t GetToken()
		{
			return ++m_cpuFenceValue;
		}

		FFenceMarker GetCpuFence()
		{
			return FFenceMarker{ m_cpuFence.get(), m_cpuFenceValue };
		}

		// Executes commandlist with token-based ordering
		void Execute(const size_t token, FCommandList* cmdList)
		{
			const size_t completedFenceValue = m_cpuFence->GetCompletedValue();
			const size_t wait = token > 0 ? token - 1 : 0;
			if (completedFenceValue < wait)
			{
				SCOPED_CPU_EVENT("wait_turn", PIX_COLOR_DEFAULT);
				HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
				if (event)
				{
					m_cpuFence->SetEventOnCompletion(wait, event);
					WaitForSingleObject(event, INFINITE);
				}
			}

			// Aquire mutex before modifying shared state
			std::lock_guard<std::mutex> scopeLock{ m_mutex };

			// Submit the CL
			RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

			// Signal CPU fence immediately after submitting the CL
			m_cpuFence->Signal(token);
		}
	};

	// Output of a render job. Contains the task the was kicked off 
	// and the sync object can be used by subsequent passes for synchronization
	struct Result
	{
		concurrency::task<void> m_task;
		FCommandList::Sync m_syncObj;
	};
}