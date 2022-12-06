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
    "CBV(b0)"

cbuffer cb : register(b0)
{
    uint g_lightListsBufferSrvIndex;
    uint g_lightGridBufferSrvIndex;
    uint g_colorTargetUavIndex;
    uint g_depthTargetSrvIndex;
    uint g_gbufferBaseColorSrvIndex;
    uint g_gbufferNormalsSrvIndex;
    uint g_gbufferMetallicRoughnessAoSrvIndex;
    uint g_packedLightTransformsBufferIndex;
    uint g_packedGlobalLightPropertiesBufferIndex;
    uint g_resX;
    uint g_resY;
    uint g_sceneBvhIndex;
    float3 g_eyePos;
    uint g_clusterGridSizeZ;
    float2 g_clusterSliceScaleAndBias;
    uint2 g_clusterGridSizeXY;
    float4x4 g_invViewProjTransform;
    float4x4 g_invProjTransform;
};


[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x < g_resX && dispatchThreadId.y < g_resY)
    {
        // Read in G-Buffer 
        Texture2D gbuffer[3] = {
            ResourceDescriptorHeap[g_gbufferBaseColorSrvIndex],
            ResourceDescriptorHeap[g_gbufferNormalsSrvIndex],
            ResourceDescriptorHeap[g_gbufferMetallicRoughnessAoSrvIndex]
        };

        const float3 basecolor = gbuffer[0][dispatchThreadId.xy].rgb;
        const float3 normal = OctDecode(gbuffer[1][dispatchThreadId.xy].rg);
        const float4 misc = gbuffer[2][dispatchThreadId.xy].rgba;

        #define metallic  (misc.r)
        #define roughness (misc.g)
        #define ao        (misc.b)
        #define aoBlend   (misc.a)


        // Calculate pixel world & view position
        float2 screenPos = dispatchThreadId.xy / float2(g_resX, g_resY);
        screenPos = 2.f * screenPos - 1.f;

        Texture2D<float> depthTex = ResourceDescriptorHeap[g_depthTargetSrvIndex];
        float depth = depthTex[dispatchThreadId.xy];

        float4 pixelWorldPos = mul(float4(screenPos.x, -screenPos.y, depth, 1.f), g_invViewProjTransform);
        pixelWorldPos /= pixelWorldPos.w;

        float4 pixelViewPos = mul(float4(screenPos.x, -screenPos.y, depth, 1.f), g_invProjTransform);
        pixelViewPos /= pixelViewPos.w;

        // Retrieve the active cluster for this pixel
        float2 clusterGridRes;
        clusterGridRes.x = g_resX / (float)g_clusterGridSizeXY.x;
        clusterGridRes.y = g_resY / (float)g_clusterGridSizeXY.y;
        uint2 pixelId = float2(dispatchThreadId.x, g_resY - dispatchThreadId.y);
        uint3 pixelCluster = GetPixelCluster(pixelId, pixelViewPos.z, clusterGridRes, g_clusterSliceScaleAndBias);
        uint clusterId = GetClusterId(pixelCluster, float3(g_clusterGridSizeXY.x, g_clusterGridSizeXY.y, g_clusterGridSizeZ));


        // Calculate view vector
        float3 V = normalize(g_eyePos - pixelWorldPos.xyz);


        // Calculate radiance
        ByteAddressBuffer lightListsBuffer = ResourceDescriptorHeap[g_lightListsBufferSrvIndex];
        ByteAddressBuffer lightGridBuffer = ResourceDescriptorHeap[g_lightGridBufferSrvIndex];
        ByteAddressBuffer lightPropertiesBuffer = ResourceDescriptorHeap[g_packedGlobalLightPropertiesBufferIndex];
        ByteAddressBuffer lightTransformsBuffer = ResourceDescriptorHeap[g_packedLightTransformsBufferIndex];
        RaytracingAccelerationStructure sceneBvh = ResourceDescriptorHeap[g_sceneBvhIndex];

        float3 radiance = 0.f;
        FLightGridData clusterInfo = lightGridBuffer.Load<FLightGridData>(clusterId * sizeof(FLightGridData));

        for (int i = 0; i < clusterInfo.m_count; ++i)
        {
            uint lightIndex = lightListsBuffer.Load<uint>((clusterInfo.m_offset + i) * sizeof(uint));
            FLight light = lightPropertiesBuffer.Load<FLight>(lightIndex * sizeof(FLight));
            float4x4 lightTransform = lightTransformsBuffer.Load<float4x4>(lightIndex * sizeof(float4x4));
            radiance += 1000.xxx;// GetDirectRadiance(light, lightTransform, pixelWorldPos.xyz, basecolor, metallic, roughness, normal, V, sceneBvh);
        }


        // Output radiance
        RWTexture2D<float3> colorTarget = ResourceDescriptorHeap[g_colorTargetUavIndex];
        colorTarget[dispatchThreadId.xy] += radiance;
    }
}