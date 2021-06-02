#include "pbr.hlsli"

#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
#define THREAD_GROUP_SIZE_Y 1
#endif

#define rootsig \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_ALL, filter = FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP), " \
    "RootConstants(b0, num32BitConstants=6, visibility = SHADER_VISIBILITY_ALL)," \
    "DescriptorTable(SRV(t0, space = 0, numDescriptors = 1000, flags = DESCRIPTORS_VOLATILE), visibility = SHADER_VISIBILITY_ALL), " \
    "DescriptorTable(UAV(u0, space = 0, numDescriptors = 1000, flags = DESCRIPTORS_VOLATILE), visibility = SHADER_VISIBILITY_ALL), "

struct CbLayout
{
    uint mipSize;
    uint faceIndex;
    uint envmapIndex;
    uint outputUavIndex;
    uint sampleCount;
    float roughness;
};

ConstantBuffer<CbLayout> g_computeConstants : register(b0);
TextureCube g_srvBindlessCubeTextures[] : register(t0);
RWTexture2DArray<float4> g_uavBindless2DTextureArrays[] : register(u0);
SamplerState g_bilinearSampler : register(s0);

float3 GetEnvDir(uint face, float2 uv)
{   
    float2 v;
    v.x = 2.f * uv.x - 1.f;
    v.y = -2.f * uv.y + 1.f;

    switch (face)
    {
    case 0: return normalize(float3(1.f, v.y, -v.x));   // +X
    case 1: return normalize(float3(-1.f, v.y, v.x));   // -X
    case 2: return normalize(float3(v.x, 1.f, -v.y));   // +Y
    case 3: return normalize(float3(v.x, -1.f, v.y));   // -Y
    case 4: return normalize(float3(v.x, v.y, 1.f));    // +Z
    case 5: return normalize(float3(-v.x, v.y, -1.f));  // -Z
    }

    return 0.f;
}

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x < g_computeConstants.mipSize &&
        dispatchThreadId.y < g_computeConstants.mipSize)
    {
        uint face = g_computeConstants.faceIndex;
        float3 R = GetEnvDir(face, dispatchThreadId.xy / float(g_computeConstants.mipSize));

        float3 N = R;
        float3 V = R;
        float3 PrefilteredColor = 0;
        float TotalWeight = 0;
        const uint NumSamples = g_computeConstants.sampleCount;
        const float Roughness = g_computeConstants.roughness;
        TextureCube EnvMap = g_srvBindlessCubeTextures[g_computeConstants.envmapIndex];

        for (uint i = 0; i < NumSamples; i++)
        {
            float2 Xi = Hammersley(i, NumSamples);
            float3 H = ImportanceSampleGGX(Xi, Roughness, N);
            float3 L = normalize(2 * dot(V, H) * H - V);
            float NoL = saturate(dot(N, L));

            if (NoL > 0.f)
            {
                PrefilteredColor += EnvMap.SampleLevel(g_bilinearSampler, L, 0).rgb * NoL;
                TotalWeight += NoL;
            }
        }

        RWTexture2DArray<float4> dest = g_uavBindless2DTextureArrays[g_computeConstants.outputUavIndex];
        dest[uint3(dispatchThreadId.x, dispatchThreadId.y, face)] = float4(PrefilteredColor / TotalWeight, 1.f);
    }
}