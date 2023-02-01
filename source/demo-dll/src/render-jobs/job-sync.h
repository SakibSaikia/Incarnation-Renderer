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

		// GPU fence signalled right before execution on the command queue
		winrt::com_ptr<D3DFence_t> m_gpuBeginFence;
		size_t m_gpuBeginFenceValue;

		// GPU fence signalled right after execution on the command queue
		winrt::com_ptr<D3DFence_t> m_gpuEndFence;
		size_t m_gpuEndFenceValue;

		// Cached values of the fence values for passes specified in SyncRenderPass
		size_t m_beginRenderPassGpuSync[Renderer::SyncRenderPassCount];
		size_t m_endRenderPassGpuSync[Renderer::SyncRenderPassCount];

		std::mutex m_mutex;

		Sync() : m_cpuFenceValue{ 0 }, m_gpuBeginFenceValue{ 0 }, m_gpuEndFenceValue{ 0 }
		{
			AssertIfFailed(RenderBackend12::GetDevice()->CreateFence(
				0,
				D3D12_FENCE_FLAG_NONE,
				IID_PPV_ARGS(m_cpuFence.put())));

			AssertIfFailed(RenderBackend12::GetDevice()->CreateFence(
				0,
				D3D12_FENCE_FLAG_NONE,
				IID_PPV_ARGS(m_gpuBeginFence.put())));

			AssertIfFailed(RenderBackend12::GetDevice()->CreateFence(
				0,
				D3D12_FENCE_FLAG_NONE,
				IID_PPV_ARGS(m_gpuEndFence.put())));
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

		// Halt execution on the command queue until the specified pass begins execution
		void InsertGpuWaitForBeginPass(D3D12_COMMAND_LIST_TYPE queueType, Renderer::SyncRenderPass pass)
		{
			SCOPED_COMMAND_QUEUE_EVENT(queueType, "cross_queue_sync", 0);
			RenderBackend12::GetCommandQueue(queueType)->Wait(m_gpuBeginFence.get(), m_beginRenderPassGpuSync[pass]);
		}

		// Halt execution on the command queue until the specified pass has finished execution
		void InsertGpuWaitForEndPass(D3D12_COMMAND_LIST_TYPE queueType, Renderer::SyncRenderPass pass)
		{
			SCOPED_COMMAND_QUEUE_EVENT(queueType, "cross_queue_sync", 0);
			RenderBackend12::GetCommandQueue(queueType)->Wait(m_gpuEndFence.get(), m_endRenderPassGpuSync[pass]);
		}

		// Executes commandlist with token-based ordering
		void Execute(const size_t token, FCommandList* cmdList, Renderer::SyncRenderPass pass = Renderer::SyncRenderPassCount)
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

			// Signal GPU fence when the command list is about to be processed on the GPU
			if (pass != Renderer::SyncRenderPassCount)
			{
				m_beginRenderPassGpuSync[pass] = ++m_gpuBeginFenceValue;
				RenderBackend12::GetCommandQueue(cmdList->m_type)->Signal(m_gpuBeginFence.get(), m_beginRenderPassGpuSync[pass]);
			}

			// Submit the CL
			RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

			// Signal GPU fence when the command list is processed on the GPU
			if (pass != Renderer::SyncRenderPassCount)
			{
				m_endRenderPassGpuSync[pass] = ++m_gpuEndFenceValue;
				RenderBackend12::GetCommandQueue(cmdList->m_type)->Signal(m_gpuEndFence.get(), m_endRenderPassGpuSync[pass]);
			}

			// Signal CPU fence immediately after submitting the CL
			m_cpuFence->Signal(token);
		}
	};
}