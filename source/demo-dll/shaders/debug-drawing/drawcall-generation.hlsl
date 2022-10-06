#include "gpu-shared-types.h"

#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)"

cbuffer cb : register(b0)
{
    uint g_argsBufferIndex;
    uint g_countsBufferIndex;
    uint g_queuedCommandsBufferIndex;
    uint g_drawCount;
};

// Helper struct that *must* alias FIndirectDrawWithRootConstants
struct FIndirectDebugDrawCmd
{
	float4 m_color;
	float4x4 m_transform;
	uint m_shapeType;
	uint3 __pad0;
	uint4 __pad1;
	uint4 __pad2;

	FDrawInstanced m_drawArguments;
};

[numthreads(THREAD_GROUP_SIZE_X, 1, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint drawId = dispatchThreadId.x;

    if (drawId < g_drawCount)
    {
        ByteAddressBuffer queuedCommandsBuffer = ResourceDescriptorHeap[g_queuedCommandsBufferIndex];
        const FDebugDrawCmd srcCmd = queuedCommandsBuffer.Load<FDebugDrawCmd>(drawId * sizeof(FDebugDrawCmd));

		FIndirectDebugDrawCmd cmd = (FIndirectDebugDrawCmd)0;

		cmd.m_color = srcCmd.m_color;
		cmd.m_transform = srcCmd.m_transform;
		cmd.m_shapeType = srcCmd.m_shapeType;
		
		cmd.m_drawArguments.m_vertexCount = srcCmd.m_indexCount;
		cmd.m_drawArguments.m_instanceCount = 1;
		cmd.m_drawArguments.m_startVertexLocation = 0;
		cmd.m_drawArguments.m_startInstanceLocation = 0;

		RWByteAddressBuffer countsBuffer = ResourceDescriptorHeap[g_countsBufferIndex];
		uint currentIndex;
		countsBuffer.InterlockedAdd(0, 1, currentIndex);

		RWByteAddressBuffer argsBuffer = ResourceDescriptorHeap[g_argsBufferIndex];
		uint destAddress = currentIndex * sizeof(FIndirectDebugDrawCmd);
		argsBuffer.Store(destAddress, cmd);
	}
}