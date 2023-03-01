#include "lighting/common.hlsli"
#include "common/cluster-culling.hlsli"
#include "geo-raster/encoding.hlsli"

#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
#define THREAD_GROUP_SIZE_Y 1
#endif

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=12)," \
    "CBV(b1)," \
    "CBV(b2)"

struct FPassConstants
{
    uint3 m_clusterGridSize;
    uint m_lightListsBufferSrvIndex;
    uint m_lightGridBufferSrvIndex;
    uint m_colorTargetUavIndex;
    uint m_depthTargetSrvIndex;
    uint m_gbufferBaseColorSrvIndex;
    uint m_gbufferNormalsSrvIndex;
    uint m_gbufferMetallicRoughnessAoSrvIndex;
    float2 m_clusterSliceScaleAndBias;
};

ConstantBuffer<FPassConstants> g_passCb : register(b0);
ConstantBuffer<FViewConstants> g_viewCb : register(b1);
ConstantBuffer<FSceneConstants> g_sceneCb : register(b2);


[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x < g_viewCb.m_resX && dispatchThreadId.y < g_viewCb.m_resY)
    {
        // Read in G-Buffer 
        Texture2D gbuffer[3] = {
            ResourceDescriptorHeap[g_passCb.m_gbufferBaseColorSrvIndex],
            ResourceDescriptorHeap[g_passCb.m_gbufferNormalsSrvIndex],
            ResourceDescriptorHeap[g_passCb.m_gbufferMetallicRoughnessAoSrvIndex]
        };

        float3 basecolor = gbuffer[0][dispatchThreadId.xy].rgb;
        const float3 normal = OctDecode(gbuffer[1][dispatchThreadId.xy].rg);
        const float4 misc = gbuffer[2][dispatchThreadId.xy].rgba;

    #if LIGHTING_ONLY
        basecolor = 0.5.xxx;
    #endif

        #define metallic  (misc.r)
        #define roughness (misc.g)
        #define ao        (misc.b)
        #define aoBlend   (misc.a)


        // Calculate pixel world & view position
        float2 screenPos = dispatchThreadId.xy / float2(g_viewCb.m_resX, g_viewCb.m_resY);
        screenPos = 2.f * screenPos - 1.f;
        screenPos.y = -screenPos.y;

        Texture2D<float> depthTex = ResourceDescriptorHeap[g_passCb.m_depthTargetSrvIndex];
        float depth = depthTex[dispatchThreadId.xy];

        float4 pixelWorldPos = mul(float4(screenPos.xy, depth, 1.f), g_viewCb.m_invViewProjTransform);
        pixelWorldPos /= pixelWorldPos.w;

        float4 pixelViewPos = mul(float4(screenPos.xy, depth, 1.f), g_viewCb.m_invProjTransform);
        pixelViewPos /= pixelViewPos.w;

        // Retrieve the active cluster for this pixel
        float2 clusterGridRes;
        clusterGridRes.x = g_viewCb.m_resX / (float)g_passCb.m_clusterGridSize.x;
        clusterGridRes.y = g_viewCb.m_resY / (float)g_passCb.m_clusterGridSize.y;
        uint3 pixelCluster = GetPixelCluster(dispatchThreadId.xy, pixelViewPos.z, clusterGridRes, g_passCb.m_clusterSliceScaleAndBias);
        uint clusterId = GetClusterId(pixelCluster, (float3)g_passCb.m_clusterGridSize);


        // Calculate view vector
        float3 V = normalize(g_viewCb.m_eyePos - pixelWorldPos.xyz);


        // Calculate radiance
        ByteAddressBuffer lightListsBuffer = ResourceDescriptorHeap[g_passCb.m_lightListsBufferSrvIndex];
        ByteAddressBuffer lightGridBuffer = ResourceDescriptorHeap[g_passCb.m_lightGridBufferSrvIndex];
        ByteAddressBuffer lightPropertiesBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedGlobalLightPropertiesBufferIndex];
        ByteAddressBuffer lightTransformsBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedLightTransformsBufferIndex];
        RaytracingAccelerationStructure sceneBvh = ResourceDescriptorHeap[g_sceneCb.m_sceneBvhIndex];

        float3 radiance = 0.f;
        FLightGridData clusterInfo = lightGridBuffer.Load<FLightGridData>(clusterId * sizeof(FLightGridData));

        for (int i = 0; i < clusterInfo.m_count; ++i)
        {
            uint lightIndex = lightListsBuffer.Load<uint>((clusterInfo.m_offset + i) * sizeof(uint));
            FLight light = lightPropertiesBuffer.Load<FLight>(lightIndex * sizeof(FLight));

            // Don't allow infinite range
            if (light.m_range == 0)
            {
                light.m_range = MAX_LIGHT_RANGE;
            }

            float4x4 lightTransform = lightTransformsBuffer.Load<float4x4>(lightIndex * sizeof(float4x4));
            radiance += GetDirectRadiance(light, lightTransform, pixelWorldPos.xyz, basecolor, metallic, roughness, normal, V, sceneBvh);
        }


        // Output radiance
        RWTexture2D<float3> colorTarget = ResourceDescriptorHeap[g_passCb.m_colorTargetUavIndex];
        colorTarget[dispatchThreadId.xy] += radiance;
    }
}