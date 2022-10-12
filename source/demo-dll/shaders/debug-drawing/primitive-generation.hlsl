#include "gpu-shared-types.h"
#include "debug-drawing/common.hlsli"

#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)"

cbuffer cb : register(b0)
{
    uint g_queuedCommandsBufferIndex;
    uint g_drawCount;
};

[numthreads(THREAD_GROUP_SIZE_X, 1, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint drawId = dispatchThreadId.x;

    if (drawId < g_drawCount)
    {
        ByteAddressBuffer queuedCommandsBuffer = ResourceDescriptorHeap[g_queuedCommandsBufferIndex];
        const FDebugDrawCmd srcCmd = queuedCommandsBuffer.Load<FDebugDrawCmd>(drawId * sizeof(FDebugDrawCmd));
		DrawDebugPrimitive(srcCmd.m_shapeType, srcCmd.m_color, srcCmd.m_transform);
	}
}