#include "lighting/pbr.hlsli"
#include "sampling.hlsli"

#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
#define THREAD_GROUP_SIZE_Y 1
#endif

#define rootsig \
    "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_ALL, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP), " \
    "RootConstants(b0, num32BitConstants=7, visibility = SHADER_VISIBILITY_ALL),"

struct CbLayout
{
    uint mipSize;
    uint cubemapSize;
    uint faceIndex;
    uint envmapIndex;
    uint outputUavIndex;
    uint sampleCount;
    float roughness;
};

ConstantBuffer<CbLayout> g_computeConstants : register(b0);
SamplerState g_trilinearSampler : register(s0);

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
        const float Resolution = float(g_computeConstants.cubemapSize);
        TextureCube EnvMap = ResourceDescriptorHeap[g_computeConstants.envmapIndex];

        for (uint i = 0; i < NumSamples; i++)
        {
            float2 Xi = Hammersley(i, NumSamples);
            float3 H = ImportanceSampleGGX(Xi, Roughness, N);
            float3 L = normalize(2 * dot(V, H) * H - V);
            float NoL = saturate(dot(N, L));

            if (NoL > 0.f)
            {
                // Use PDF to select mip level. Samples with low PDF choose a smaller mip to get averaged results. This helps avoid fireflies
                // See : https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-20-gpu-based-importance-sampling and https://learnopengl.com/PBR/IBL/Specular-IBL
                float NoH = max(dot(N, H), 0.0);
                float VoH = max(dot(V, H), 0.0);
                float D = D_GGX(NoH, Roughness);
                float pdf = (D * NoH / (4.0 * VoH)) + 0.0001;
                float saTexel = 4.f * PI / (6.0 * Resolution * Resolution);
                float saSample = 1.f / (float(NumSamples) * pdf + 0.0001);
                float mipLevel = (Roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel));
                
                PrefilteredColor += EnvMap.SampleLevel(g_trilinearSampler, L, mipLevel).rgb * NoL;
                TotalWeight += NoL;
            }
        }

        RWTexture2DArray<float4> dest = ResourceDescriptorHeap[g_computeConstants.outputUavIndex];
        dest[uint3(dispatchThreadId.x, dispatchThreadId.y, face)] = float4(PrefilteredColor / TotalWeight, 1.f);
    }
}