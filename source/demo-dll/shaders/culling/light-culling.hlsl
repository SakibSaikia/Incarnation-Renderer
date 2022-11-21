#include "gpu-shared-types.h"
#include "common/cluster-culling.hlsli"
#include "debug-drawing/common.hlsli"

#ifndef THREAD_GROUP_SIZE_X
    #define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
    #define THREAD_GROUP_SIZE_Y 1
#endif

#ifndef THREAD_GROUP_SIZE_Z
    #define THREAD_GROUP_SIZE_Z 1
#endif

#ifndef MAX_LIGHTS_PER_CLUSTER
    #define MAX_LIGHTS_PER_CLUSTER 128
#endif

#define MAX_RANGE (2.f)

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)"


cbuffer cb : register(b0)
{
    uint g_culledLightCountBufferUavIndex;
    uint g_culledLightListsBufferUavIndex;
    uint g_lightGridBufferUavIndex;
    uint g_packedLightIndicesBufferIndex;
    uint g_packedLightTransformsBufferIndex;
    uint g_packedGlobalLightPropertiesBufferIndex;
    uint g_lightCount;
    float g_clusterDepthExtent;
    uint3 g_clusterGridSize;
    float g_cameraNearPlane;
    float4x4 g_projTransform;
    float4x4 g_invViewProjTransform;
};


// See light types in gpu-shared-types.h
bool IsPunctualLight(int lightType)
{
    return lightType != (int)Light::Directional;
}

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, THREAD_GROUP_SIZE_Z)]
void cs_main(uint3 clusterIndex : SV_DispatchThreadID)
{
    RWByteAddressBuffer renderStatsBuffer = ResourceDescriptorHeap[SpecialDescriptors::RenderStatsBufferUavIndex];
    if ((clusterIndex.x | clusterIndex.y | clusterIndex.z) == 0)
    {
        renderStatsBuffer.Store(sizeof(int), 0);
    }

    GroupMemoryBarrierWithGroupSync();

    if (clusterIndex.x < g_clusterGridSize.x &&
        clusterIndex.y < g_clusterGridSize.y &&
        clusterIndex.z < g_clusterGridSize.z)
    {   
        ByteAddressBuffer sceneLightIndicesBuffer = ResourceDescriptorHeap[g_packedLightIndicesBufferIndex];
        ByteAddressBuffer sceneLightTransformsBuffer = ResourceDescriptorHeap[g_packedLightTransformsBufferIndex];
        ByteAddressBuffer globalLightPropertiesBuffer = ResourceDescriptorHeap[g_packedGlobalLightPropertiesBufferIndex];

        // Unrolled unique index for the cluster
        const uint clusterId = GetClusterId(clusterIndex, g_clusterGridSize);

        // Per cluster - visible light count and light indices list
        FLightGridData clusterInfo = (FLightGridData)0;
        uint visibleLightIndices[MAX_LIGHTS_PER_CLUSTER];

        const float2 viewSpaceDepthExtent = float2(g_cameraNearPlane, g_clusterDepthExtent);
        const FFrustum clusterFrustum = GetClusterFrustum(clusterIndex, g_clusterGridSize, viewSpaceDepthExtent, g_projTransform, g_invViewProjTransform);

        for (uint i = 0; i < g_lightCount && clusterInfo.m_count < MAX_LIGHTS_PER_CLUSTER; ++i)
        {
            const uint globalLightIndex = sceneLightIndicesBuffer.Load<uint>(i * sizeof(uint));
            const FLight light = globalLightPropertiesBuffer.Load<FLight>(globalLightIndex * sizeof(FLight));

            // Directional lights are handled in a separate pass outside of light clustering
            if (!IsPunctualLight(light.m_type))
                continue;

            // Don't allow infinite range
            const float lightRange = light.m_range == 0 ? MAX_RANGE : light.m_range;

            // For point and spot lights, the light position is contained in the transform
            const float4x4 lightTransform = sceneLightTransformsBuffer.Load<float4x4>(i * sizeof(float4x4));
            const float3 lightPos = float3(lightTransform._41, lightTransform._42, lightTransform._43);

#if SHOW_LIGHT_BOUNDS
            if ((clusterIndex.x | clusterIndex.y | clusterIndex.z) == 0)
            {
                float4x4 scaledLightTransform =
                {
                    lightRange, 0.f, 0.f, 0.f,
                    0.f, lightRange, 0.f, 0.f,
                    0.f, 0.f, lightRange, 0.f,
                    lightPos.x, lightPos.y, lightPos.z, 1.f
                };
                DrawDebugPrimitive((uint)DebugShape::Sphere, float4(0, 1.f, 0.f, 1.f), scaledLightTransform);
            }
#endif

            if (FrustumCull(clusterFrustum, float4(lightPos.xyz, lightRange)))
            {
                visibleLightIndices[clusterInfo.m_count++] = globalLightIndex;           
            }
            else
            {
                int previousValue;
                renderStatsBuffer.InterlockedAdd(sizeof(int), 1, previousValue);
            }
        }

        // Wait for all clusters to finish culling
        GroupMemoryBarrierWithGroupSync();

        // Update the light indices list for the cluster
        RWByteAddressBuffer culledLightCountBuffer = ResourceDescriptorHeap[g_culledLightCountBufferUavIndex];
        RWByteAddressBuffer culledLightListsBuffer = ResourceDescriptorHeap[g_culledLightListsBufferUavIndex];

        if (clusterInfo.m_count > 0)
        {
            culledLightCountBuffer.InterlockedAdd(0, clusterInfo.m_count, clusterInfo.m_offset);

            for (uint j = 0; j < clusterInfo.m_count; ++j)
            {
                culledLightListsBuffer.Store<uint>((clusterInfo.m_offset + j) * sizeof(uint), visibleLightIndices[j]);
            }
        }

        // Update the light grid for the cluster
        RWByteAddressBuffer lightGridBuffer = ResourceDescriptorHeap[g_lightGridBufferUavIndex];
        lightGridBuffer.Store<FLightGridData>(clusterId * sizeof(FLightGridData), clusterInfo);
    }
}