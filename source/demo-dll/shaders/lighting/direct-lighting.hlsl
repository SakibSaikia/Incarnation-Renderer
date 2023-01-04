#include "lighting/common.hlsli"
#include "geo-raster/encoding.hlsli"

#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
#define THREAD_GROUP_SIZE_Y 1
#endif

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=5)," \
    "CBV(b1)," \
    "CBV(b2)"

struct FPassConstants
{
    uint m_colorTargetUavIndex;
    uint m_depthTargetSrvIndex;
    uint m_gbufferBaseColorSrvIndex;
    uint m_gbufferNormalsSrvIndex;
    uint m_gbufferMetallicRoughnessAoSrvIndex;
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

        const float3 basecolor = gbuffer[0][dispatchThreadId.xy].rgb;
        const float3 normal = OctDecode(gbuffer[1][dispatchThreadId.xy].rg);
        const float4 misc = gbuffer[2][dispatchThreadId.xy].rgba;

        #define metallic  (misc.r)
        #define roughness (misc.g)
        #define ao        (misc.b)
        #define aoBlend   (misc.a)


        // Calculate pixel world position
        float2 screenPos = dispatchThreadId.xy / float2(g_viewCb.m_resX, g_viewCb.m_resY);
        screenPos = 2.f * screenPos - 1.f;

        Texture2D<float> depthTex = ResourceDescriptorHeap[g_passCb.m_depthTargetSrvIndex];
        float depth = depthTex[dispatchThreadId.xy];

        float4 pixelWorldPos = mul(float4(screenPos.x, -screenPos.y, depth, 1.f), g_viewCb.m_invViewProjTransform);
        pixelWorldPos /= pixelWorldPos.w;


        // Calculate view vector
        float3 V = normalize(g_viewCb.m_eyePos - pixelWorldPos.xyz);


        // Calculate radiance
        ByteAddressBuffer lightPropertiesBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedGlobalLightPropertiesBufferIndex];
        ByteAddressBuffer lightTransformsBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedLightTransformsBufferIndex];
        RaytracingAccelerationStructure sceneBvh = ResourceDescriptorHeap[g_sceneCb.m_sceneBvhIndex];

        FLight light = lightPropertiesBuffer.Load<FLight>(g_sceneCb.m_sunIndex * sizeof(FLight));
        float4x4 lightTransform = lightTransformsBuffer.Load<float4x4>(g_sceneCb.m_sunIndex * sizeof(float4x4));
        float3 radiance = GetDirectRadiance(light, lightTransform, pixelWorldPos.xyz, basecolor, metallic, roughness, normal, V, sceneBvh);


        // Output radiance
        RWTexture2D<float3> colorTarget = ResourceDescriptorHeap[g_passCb.m_colorTargetUavIndex];
        colorTarget[dispatchThreadId.xy] += radiance;
    }
}