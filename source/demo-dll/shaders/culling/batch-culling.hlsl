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
    uint g_primitivesBufferIndex;
    uint g_primitiveCount;
};

[numthreads(THREAD_GROUP_SIZE_X, 1, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint primId = dispatchThreadId.x;
    if (primId < g_primitiveCount)
    {
        ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[g_primitivesBufferIndex];
        const FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(primId * sizeof(FGpuPrimitive));

        FDrawWithRootConstants cmd = (FDrawWithRootConstants)0;
        cmd.rootConstants[0] = primId;
        cmd.drawArguments.VertexCount = primitive.m_indexCount;
        cmd.drawArguments.InstanceCount = 1;
        cmd.drawArguments.StartVertexLocation = 0;
        cmd.drawArguments.StartInstanceLocation = 0;

        uint currentIndex;
        RWByteAddressBuffer countsBuffer = ResourceDescriptorHeap[g_countsBufferIndex];
        countsBuffer.InterlockedAdd(0, 1, currentIndex);

        RWByteAddressBuffer argsBuffer = ResourceDescriptorHeap[g_argsBufferIndex];
        uint destAddress = currentIndex * sizeof(FDrawWithRootConstants);
        argsBuffer.Store(destAddress, cmd);
    }
}