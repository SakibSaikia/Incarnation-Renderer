#pragma once

#include <backend-d3d12.h>
#include <Tracy.hpp>

#define DO_TOKEN_PASTE(a, b) a ## b
#define TOKEN_PASTE(a, b)  DO_TOKEN_PASTE(a,b)

#define SCOPED_CPU_EVENT(name, color) Profiling::ScopedCpuEvent TOKEN_PASTE(event_, __LINE__)("CPU", name, color); ZoneScopedN(name);
#define SCOPED_COMMAND_LIST_EVENT(cmdList, name, color) Profiling::ScopedCommandListEvent TOKEN_PASTE(event_, __LINE__)(cmdList, name, color)
#define SCOPED_COMMAND_QUEUE_EVENT(cmdQueueType, name, color) Profiling::ScopedCommandQueueEvent TOKEN_PASTE(event_, __LINE__)(cmdQueueType, name, color)

namespace Profiling
{
	struct ScopedCpuEvent
	{
		ScopedCpuEvent(const char* groupName, const char* eventName, uint64_t color);
		ScopedCpuEvent(const char* groupName, const wchar_t* eventName, uint64_t color);
		~ScopedCpuEvent();
	};

	struct ScopedCommandListEvent
	{
		FCommandList* m_cmdList;

		ScopedCommandListEvent(FCommandList* cmdList, const char* eventName, uint64_t color);
		ScopedCommandListEvent(FCommandList* cmdList, const wchar_t* eventName, uint64_t color);
		~ScopedCommandListEvent();
	};

	struct ScopedCommandQueueEvent
	{
		D3DCommandQueue_t* m_cmdQueue;

		ScopedCommandQueueEvent(D3D12_COMMAND_LIST_TYPE queueType, const char* eventName, uint64_t color);
		ScopedCommandQueueEvent(D3D12_COMMAND_LIST_TYPE queueType, const wchar_t* eventName, uint64_t color);
		~ScopedCommandQueueEvent();
	};
}