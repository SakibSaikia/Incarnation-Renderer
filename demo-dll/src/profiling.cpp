#include <profiling.h>
#include <backend-d3d12.h>
#include <common.h>
#include <pix3.h>

Profiling::ScopedCpuEvent::ScopedCpuEvent(const wchar_t* groupName, const wchar_t* eventName, uint64_t color)
{
	char group[128];
	WideCharToMultiByte(CP_UTF8, 0, groupName, -1, group, 128, NULL, NULL);

	char name[128];
	WideCharToMultiByte(CP_UTF8, 0, eventName, -1, name, 128, NULL, NULL);

	PIXBeginEvent(color, eventName);
}

Profiling::ScopedCpuEvent::~ScopedCpuEvent()
{
	PIXEndEvent();
}

Profiling::ScopedCommandListEvent::ScopedCommandListEvent(FCommandList* cmdList, const wchar_t* eventName, uint64_t color) :
	m_cmdList{ cmdList }
{
	PIXBeginEvent(cmdList->m_d3dCmdList.get(), color, eventName);
}

Profiling::ScopedCommandListEvent::~ScopedCommandListEvent()
{
	PIXEndEvent(m_cmdList->m_d3dCmdList.get());
}

Profiling::ScopedCommandQueueEvent::ScopedCommandQueueEvent(D3D12_COMMAND_LIST_TYPE queueType, const wchar_t* eventName, uint64_t color)
{
	m_cmdQueue = RenderBackend12::GetCommandQueue(queueType);
	PIXBeginEvent(m_cmdQueue, color, eventName);
}

Profiling::ScopedCommandQueueEvent::~ScopedCommandQueueEvent()
{
	PIXEndEvent(m_cmdQueue);
}