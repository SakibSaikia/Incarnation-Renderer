#include "gpu-shared-types.h"
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
    uint __padding0;
    uint3 g_clusterGridSize;
    float g_cameraNearPlane;
    float4x4 g_ProjTransform;
    float4x4 g_invViewProjTransform;
};

struct FFrustum
{
    // .xyz = Plane Normal, .w = d
    float4 m_nearPlane;
    float4 m_farPlane;
    float4 m_leftPlane;
    float4 m_rightPlane;
    float4 m_bottomPlane;
    float4 m_topPlane;
};

// See light types in gpu-shared-types.h
bool IsPunctualLight(int lightType)
{
    return lightType != (int)Light::Directional;
}

// Flattened cluster ID corresponding to location of the cluster in the volume
uint GetClusterId(float3 clusterIndex)
{
    return (g_clusterGridSize.x * g_clusterGridSize.y) * clusterIndex.z + g_clusterGridSize.x * clusterIndex.y + clusterIndex.x;
}

// Get the planes that describe the cluster frustum in world space
FFrustum GetClusterFrustum(uint3 clusterIndex)
{
    // Cluster slices are evenly assigned in NDC space for X & Y directions
    float2 NDCIndex = float2(clusterIndex.x - g_clusterGridSize.x / 2.f, clusterIndex.y - g_clusterGridSize.y / 2.f);
    float2 Stride = 2.f / g_clusterGridSize.xy;

    // Cluster slices should not be evenly assigned for depth in NDC space as it causes skewed placement of clusters due to z-nonlinearity.
    // Instead, use an exponential scheme for slices in view space to counter this effect as per [Tiago Sousa SIGGRAPH 2016]
    const float zFar = 20.f;
    const float zNear = g_cameraNearPlane;
    float ViewSpaceClusterDepthExtents[] = {
        zNear * pow(zFar / zNear, clusterIndex.z / (float)g_clusterGridSize.z),
        zNear * pow(zFar / zNear, (clusterIndex.z + 1.f) / (float)g_clusterGridSize.z)
    };

    // Project the view space depth extents to NDC space
    float4 NDCNearPoint = mul(float4(0.f, 0.f, ViewSpaceClusterDepthExtents[0], 1.f), g_ProjTransform);
    float4 NDCFarPoint = mul(float4(0.f, 0.f, ViewSpaceClusterDepthExtents[1], 1.f), g_ProjTransform);

    float4 ProjectedClusterPoints[] = {
        // Near plane points
        float4(NDCIndex.x * Stride.x, NDCIndex.y * Stride.y, NDCNearPoint.z / NDCNearPoint.w, 1.f),
        float4(NDCIndex.x * Stride.x + Stride.x, NDCIndex.y * Stride.y, NDCNearPoint.z / NDCNearPoint.w, 1.f),
        float4(NDCIndex.x * Stride.x + Stride.x, NDCIndex.y * Stride.y + Stride.y, NDCNearPoint.z / NDCNearPoint.w, 1.f),
        float4(NDCIndex.x * Stride.x, NDCIndex.y * Stride.y + Stride.y, NDCNearPoint.z / NDCNearPoint.w, 1.f),
        // Far plane points
        float4(NDCIndex.x * Stride.x, NDCIndex.y * Stride.y, NDCFarPoint.z / NDCFarPoint.w, 1.f),
        float4(NDCIndex.x * Stride.x + Stride.x, NDCIndex.y * Stride.y, NDCFarPoint.z / NDCFarPoint.w, 1.f),
        float4(NDCIndex.x * Stride.x + Stride.x, NDCIndex.y * Stride.y + Stride.y, NDCFarPoint.z / NDCFarPoint.w, 1.f),
        float4(NDCIndex.x * Stride.x, NDCIndex.y * Stride.y + Stride.y, NDCFarPoint.z / NDCFarPoint.w, 1.f),
    };

    // Unproject the NDC points to get frustum in World Space
    float3 P[8];
    for (int i = 0; i < 8; ++i)
    {
        float4 worldPos = mul(ProjectedClusterPoints[i], g_invViewProjTransform);
        P[i] = worldPos.xyz / worldPos.w;
    }

    DrawDebugFrustum(float4(0.f, 0.f, 1.f, 1.f), P[0], P[1], P[2], P[3], P[4], P[5], P[6], P[7]);

    // Extract frustum planes from the world space frustum points
    // Equation of plane passing through 3 points A, B and C is given by
    // n = (B - A) X (C - A) and d = -n.A
    FFrustum clusterFrustum;
    clusterFrustum.m_nearPlane.xyz = cross(P[1] - P[0], P[3] - P[0]);
    clusterFrustum.m_farPlane.xyz = cross(P[7] - P[4], P[5] - P[4]);
    clusterFrustum.m_leftPlane.xyz = cross(P[3] - P[0], P[4] - P[0]);
    clusterFrustum.m_rightPlane.xyz = cross(P[5] - P[1], P[2] - P[1]);
    clusterFrustum.m_topPlane.xyz = cross(P[6] - P[2], P[3] - P[2]);
    clusterFrustum.m_bottomPlane.xyz = cross(P[4] - P[0], P[1] - P[0]);

    clusterFrustum.m_nearPlane.w = -dot(clusterFrustum.m_nearPlane.xyz, P[0].xyz);
    clusterFrustum.m_farPlane.w = -dot(clusterFrustum.m_farPlane.xyz, P[4].xyz);
    clusterFrustum.m_leftPlane.w = -dot(clusterFrustum.m_leftPlane.xyz, P[0].xyz);
    clusterFrustum.m_rightPlane.w = -dot(clusterFrustum.m_rightPlane.xyz, P[1].xyz);
    clusterFrustum.m_topPlane.w = -dot(clusterFrustum.m_topPlane.xyz, P[2].xyz);
    clusterFrustum.m_bottomPlane.w = -dot(clusterFrustum.m_bottomPlane.xyz, P[0].xyz);

    return clusterFrustum;
}

// Return true if the bounds are contained within the frustum, else return false.
bool FrustumCull(FFrustum frustum, float4 bounds)
{
    // Light bounds
    const float4 boundsCenter = float4(bounds.xyz, 1.f);
    const float boundsRadius = bounds.w;

    // Frustum test - scale the radius by the plane normal instead of normalizing the plane
    return (dot(boundsCenter, frustum.m_nearPlane) + boundsRadius * length(frustum.m_nearPlane.xyz) >= 0)
        && (dot(boundsCenter, frustum.m_farPlane) + boundsRadius * length(frustum.m_farPlane.xyz) >= 0)
        && (dot(boundsCenter, frustum.m_leftPlane) + boundsRadius * length(frustum.m_leftPlane.xyz) >= 0)
        && (dot(boundsCenter, frustum.m_rightPlane) + boundsRadius * length(frustum.m_rightPlane.xyz) >= 0)
        && (dot(boundsCenter, frustum.m_bottomPlane) + boundsRadius * length(frustum.m_bottomPlane.xyz) >= 0)
        && (dot(boundsCenter, frustum.m_topPlane) + boundsRadius * length(frustum.m_topPlane.xyz) >= 0);
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
        const uint clusterId = GetClusterId(clusterIndex);

        // Per cluster - visible light count and light indices list
        uint visibleLightCount = 0;
        uint visibleLightIndices[MAX_LIGHTS_PER_CLUSTER];

        const FFrustum clusterFrustum = GetClusterFrustum(clusterIndex);

        for (uint i = 0; i < g_lightCount && visibleLightCount < MAX_LIGHTS_PER_CLUSTER; ++i)
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

            if (FrustumCull(clusterFrustum, float4(lightPos.xyz, lightRange)))
            {
                visibleLightIndices[visibleLightCount++] = globalLightIndex;

                float4x4 scaledLightTransform =
                {
                    lightRange, 0.f, 0.f, 0.f,
                    0.f, lightRange, 0.f, 0.f,
                    0.f, 0.f, lightRange, 0.f,
                    lightPos.x, lightPos.y, lightPos.z, 1.f
                };
                DrawDebugPrimitive((uint)DebugShape::Sphere, float4(0, 1.f, 0.f, 1.f), scaledLightTransform);
            }
            else
            {
                int previousValue;
                renderStatsBuffer.InterlockedAdd(sizeof(int), 1, previousValue);

                float4x4 scaledLightTransform =
                {
                    lightRange, 0.f, 0.f, 0.f,
                    0.f, lightRange, 0.f, 0.f,
                    0.f, 0.f, lightRange, 0.f,
                    lightTransform._41, lightTransform._42, lightTransform._43, lightTransform._44
                };
                DrawDebugPrimitive((uint)DebugShape::Sphere, float4(1, 0.f, 0.f, 1.f), scaledLightTransform);
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