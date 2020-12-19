#pragma once

#include <microprofile.h>
#include <backend-d3d12.h>

#define SCOPED_CPU_EVENT(name, color)			Profiling::ScopedCpuEvent MICROPROFILE_TOKEN_PASTE(event_, __LINE__)(L"CPU", name, color)
#define SCOPED_GPU_EVENT(cmdList, name, color)	Profiling::ScopedGpuEvent MICROPROFILE_TOKEN_PASTE(event_, __LINE__)(cmdList, name, color)

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

	struct ScopedGpuEvent
	{
		FCommandList* m_cmdList;
		MicroProfileThreadLogGpu* m_uprofLog;
		MicroProfileToken m_uprofToken;
		uint64_t m_uprofTick;

		ScopedGpuEvent(FCommandList* cmdList, const wchar_t* eventName, uint64_t color);
		~ScopedGpuEvent();
	};

	void Initialize();
	void Teardown();
	void Flip();
}