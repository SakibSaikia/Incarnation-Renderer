#include "gpu-shared-types.h"

#ifndef THREAD_GROUP_SIZE_X
    #define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
    #define THREAD_GROUP_SIZE_Y 1
#endif

#ifndef THREAD_GROUP_SIZE_Z
    #define THREAD_GROUP_SIZE_Z 1
#endif

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)"

static const uint k_maxLightsPerCluster = 128;

cbuffer cb : register(b0)
{
    uint g_culledLightCountBufferUavIndex;
    uint g_culledLightListsBufferUavIndex;
    uint g_lightGridBufferUavIndex;
    uint g_packedLightIndicesBufferIndex;
    uint g_packedLightPropertiesBufferIndex;
    uint g_packedLightTransformsBufferIndex;
    uint g_lightCount;
    uint __padding0;
    uint3 g_clusterGridSize;
    uint __padding1;
    float4x4 g_viewProjTransform;
};

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, THREAD_GROUP_SIZE_Z)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x < g_clusterGridSize.x &&
        dispatchThreadId.y < g_clusterGridSize.y &&
        dispatchThreadId.z < g_clusterGridSize.z)
    {   
        // Unrolled unique index for the cluster
        const uint clusterId = (g_clusterGridSize.x * g_clusterGridSize.y) * dispatchThreadId.z +  g_clusterGridSize.x * dispatchThreadId.y + dispatchThreadId.x;

        // Per cluster - visible light count and light indices list
        uint visibleLightCount = 0;
        uint visibleLightIndices[k_maxLightsPerCluster];

        ByteAddressBuffer lightIndicesBuffer = ResourceDescriptorHeap[g_packedLightIndicesBufferIndex];
        for (uint i = 0; i < g_lightCount && visibleLightCount < k_maxLightsPerCluster; ++i)
        {
            const uint lightIndex = lightIndicesBuffer.Load<uint>(i * sizeof(uint));
            if (true)
            {
                visibleLightIndices[visibleLightCount++] = lightIndex;
            }
        }

        // Wait for all clusters to finish culling
        GroupMemoryBarrierWithGroupSync();

        // Update the light indices list for the cluster
        RWByteAddressBuffer culledLightCountBuffer = ResourceDescriptorHeap[g_culledLightCountBufferUavIndex];
        RWByteAddressBuffer culledLightListsBuffer = ResourceDescriptorHeap[g_culledLightListsBufferUavIndex];

        uint previousCount;
        culledLightCountBuffer.InterlockedAdd(0, visibleLightCount, previousCount);

        const uint offset = previousCount;
        for (uint j = 0; j < visibleLightCount; ++j)
        {
            culledLightListsBuffer.Store<uint>((offset + j) * sizeof(uint), visibleLightIndices[j]);
        }

        // Update the light grid for the cluster
        RWByteAddressBuffer lightGridBuffer = ResourceDescriptorHeap[g_lightGridBufferUavIndex];
        lightGridBuffer.Store<uint>(clusterId * sizeof(uint2), offset);
        lightGridBuffer.Store<uint>(clusterId * sizeof(uint2) + sizeof(uint), visibleLightCount);
    }
}