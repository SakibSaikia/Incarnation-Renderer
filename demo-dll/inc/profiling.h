#pragma once

#include <microprofile.h>
#include <backend-d3d12.h>

#define SCOPED_CPU_EVENT(name, color) Profiling::ScopedCpuEvent MICROPROFILE_TOKEN_PASTE(event_, __LINE__)(L"CPU", name, color)
#define SCOPED_COMMAND_LIST_EVENT(cmdList, name, color) Profiling::ScopedCommandListEvent MICROPROFILE_TOKEN_PASTE(event_, __LINE__)(cmdList, name, color)
#define SCOPED_COMMAND_QUEUE_EVENT(cmdQueueType, name, color) Profiling::ScopedCommandQueueEvent MICROPROFILE_TOKEN_PASTE(event_, __LINE__)(cmdQueueType, name, color)

namespace Profiling
{
	struct ScopedCpuEvent
	{
		MicroProfileThreadLogGpu* m_uprofLog;
		MicroProfileToken m_uprofToken;
		uint64_t m_uprofTick;

		ScopedCpuEvent(const wchar_t* groupName, const wchar_t* eventName, uint64_t color);
		~ScopedCpuEvent();
	};

	struct ScopedCommandListEvent
	{
		FCommandList* m_cmdList;
		MicroProfileThreadLogGpu* m_uprofLog;
		MicroProfileToken m_uprofToken;
		uint64_t m_uprofTick;

		ScopedCommandListEvent(FCommandList* cmdList, const wchar_t* eventName, uint64_t color);
		~ScopedCommandListEvent();
	};

	struct ScopedCommandQueueEvent
	{
		D3DCommandQueue_t* m_cmdQueue;

		ScopedCommandQueueEvent(D3D12_COMMAND_LIST_TYPE queueType, const wchar_t* eventName, uint64_t color);
		~ScopedCommandQueueEvent();
	};

	void Initialize();
	void Teardown();
	void Flip();
}