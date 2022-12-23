#include "lighting/common.hlsli"
#include "geo-raster/encoding.hlsli"
#include "image-based-lighting/spherical-harmonics/common.hlsli"

#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
#define THREAD_GROUP_SIZE_Y 1
#endif

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)," \
    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE)"

cbuffer cb : register(b0)
{
    int g_skylightProbeIndex;
    int g_envmapIndex;
    uint g_colorTargetUavIndex;
    uint g_depthTargetSrvIndex;
    uint g_gbufferBaseColorSrvIndex;
    uint g_gbufferNormalsSrvIndex;
    uint g_gbufferMetallicRoughnessAoSrvIndex;
    uint g_resX;
    uint g_resY;
    uint3 __pad;
    float3 g_eyePos;
    int g_envBrdfTextureIndex;
    float4x4 g_invViewProjTransform;
};

SamplerState g_trilinearSampler : register(s0);


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
        const float3 N = OctDecode(gbuffer[1][dispatchThreadId.xy].rg);
        const float4 misc = gbuffer[2][dispatchThreadId.xy].rgba;

        #define metallic  (misc.r)
        #define roughness (misc.g)
        #define ao        (misc.b)
        #define aoBlend   (misc.a)


        // Calculate pixel world position
        float2 screenPos = dispatchThreadId.xy / float2(g_resX, g_resY);
        screenPos = 2.f * screenPos - 1.f;
        screenPos.y = -screenPos.y;

        Texture2D<float> depthTex = ResourceDescriptorHeap[g_depthTargetSrvIndex];
        float depth = depthTex[dispatchThreadId.xy];

        float4 pixelWorldPos = mul(float4(screenPos.xy, depth, 1.f), g_invViewProjTransform);
        pixelWorldPos /= pixelWorldPos.w;


        // Calculate view vector
        float3 V = normalize(g_eyePos - pixelWorldPos.xyz);

        float3 radiance = 0.f;

        // Diffuse IBL
#if DIFFUSE_IBL
        if (g_skylightProbeIndex != -1)
        {
            SH9Color shRadiance;
            Texture2D shTex = ResourceDescriptorHeap[g_skylightProbeIndex];

            [UNROLL]
            for (int i = 0; i < SH_COEFFICIENTS; ++i)
            {
                shRadiance.c[i] = shTex.Load(int3(i, 0, 0)).rgb;
            }

            float3 albedo = (1.f - metallic) * basecolor;
            float3 shDiffuse = /*(1.f - F) * */albedo * Fd_Lambert() * ShIrradiance(N, shRadiance);
            radiance += lerp(shDiffuse, ao * shDiffuse, aoBlend);
        }
#endif

        // Specular IBL
#if SPECULAR_IBL
        if (g_envmapIndex != -1 && g_envBrdfTextureIndex != -1)
        {
            TextureCube prefilteredEnvMap = ResourceDescriptorHeap[g_envmapIndex];
            Texture2D envBrdfTex = ResourceDescriptorHeap[g_envBrdfTextureIndex];

            float texWidth, texHeight, mipCount;
            prefilteredEnvMap.GetDimensions(0, texWidth, texHeight, mipCount);

            // FIXME - The env BRDF texture has a few lines of black near (0,0). So, apply a threshhold here for now.
            float NoV = max(dot(N, V), 0.01);
            float3 F0 = metallic * basecolor + (1.f - metallic) * 0.04;
            float3 R = normalize(reflect(-V, N));
            float3 prefilteredColor = prefilteredEnvMap.SampleLevel(g_trilinearSampler, R, roughness * (mipCount - 1)).rgb;
            float2 envBrdf = envBrdfTex.SampleLevel(g_trilinearSampler, float2(NoV, roughness), 0.f).rg;
            float3 specularIBL = prefilteredColor * (F0 * envBrdf.x + envBrdf.y);
            radiance += lerp(specularIBL, ao * specularIBL, aoBlend);
        }
#endif


        // Output radiance
        RWTexture2D<float3> colorTarget = ResourceDescriptorHeap[g_colorTargetUavIndex];
        colorTarget[dispatchThreadId.xy] += radiance;
    }
}