#include <profiling.h>
#include <backend-d3d12.h>
#include <common.h>
#include <pix3.h>

Profiling::ScopedCpuEvent::ScopedCpuEvent(const char* groupName, const char* eventName, uint64_t color)
{
	PIXBeginEvent(color, eventName);
}

Profiling::ScopedCpuEvent::~ScopedCpuEvent()
{
	PIXEndEvent();
}

Profiling::ScopedCommandListEvent::ScopedCommandListEvent(FCommandList* cmdList, const char* eventName, uint64_t color) :
	m_cmdList{ cmdList }
{
	PIXBeginEvent(cmdList->m_d3dCmdList.get(), color, eventName);
}

Profiling::ScopedCommandListEvent::~ScopedCommandListEvent()
{
	PIXEndEvent(m_cmdList->m_d3dCmdList.get());
}

Profiling::ScopedCommandQueueEvent::ScopedCommandQueueEvent(D3D12_COMMAND_LIST_TYPE queueType, const char* eventName, uint64_t color)
{
	m_cmdQueue = RenderBackend12::GetCommandQueue(queueType);
	PIXBeginEvent(m_cmdQueue, color, eventName);
}

Profiling::ScopedCommandQueueEvent::~ScopedCommandQueueEvent()
{
	PIXEndEvent(m_cmdQueue);
}