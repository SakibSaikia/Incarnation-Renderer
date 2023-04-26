#include "image-based-lighting/spherical-harmonics/common.hlsli"

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=3, visibility = SHADER_VISIBILITY_ALL)"

struct CbLayout
{
    uint srcUavIndex;
    uint destUavIndex;
    float normalizationFactor;
};

ConstantBuffer<CbLayout> g_constants : register(b0);


[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint globalIndex : SV_GroupIndex)
{
    RWTexture2DArray<float4> src = ResourceDescriptorHeap[g_constants.srcUavIndex];
    RWTexture2D<float4> dest = ResourceDescriptorHeap[g_constants.destUavIndex];

    SH9ColorCoefficient sum;
    int i;
    [unroll]
    for (i = 0; i < SH_NUM_COEFFICIENTS; ++i)
    {
        float3 coeff = src[uint3(dispatchThreadId.x, dispatchThreadId.y, i)].rgb;
        sum.c[i] = WaveActiveSum(coeff);
    }

    GroupMemoryBarrierWithGroupSync();

    if (globalIndex == 0)
    {
        [unroll]
        for (i = 0; i < SH_NUM_COEFFICIENTS; ++i)
        {
            dest[uint2(i, 0)] = float4(sum.c[i], 1.f);
        }
    }
}