#include "image-based-lighting/spherical-harmonics/common.hlsli"

#define rootsig \
    "RootConstants(b0, num32BitConstants=3, visibility = SHADER_VISIBILITY_ALL)," \
    "DescriptorTable(UAV(u0, space = 0, numDescriptors = 1000, flags = DESCRIPTORS_VOLATILE), visibility = SHADER_VISIBILITY_ALL), " \
    "DescriptorTable(UAV(u1, space = 1, numDescriptors = 1000, flags = DESCRIPTORS_VOLATILE), visibility = SHADER_VISIBILITY_ALL), "

struct CbLayout
{
    uint srcUavIndex;
    uint destUavIndex;
    float normalizationFactor;
};

ConstantBuffer<CbLayout> g_constants : register(b0);
RWTexture2D<float4> g_uavBindless2DTextures[] : register(u0, space0);
RWTexture2DArray<float4> g_uavBindless2DTextureArrays[] : register(u1, space1);


[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint globalIndex : SV_GroupIndex)
{
    RWTexture2DArray<float4> src = g_uavBindless2DTextureArrays[g_constants.srcUavIndex];
    RWTexture2D<float4> dest = g_uavBindless2DTextures[g_constants.destUavIndex];

    SH9Color sum;
    int i;
    [unroll]
    for (i = 0; i < SH_COEFFICIENTS; ++i)
    {
        float3 coeff = src[uint3(dispatchThreadId.x, dispatchThreadId.y, i)].rgb;
        sum.c[i] = WaveActiveSum(coeff);
    }

    GroupMemoryBarrierWithGroupSync();

    if (globalIndex == 0)
    {
        [unroll]
        for (i = 0; i < SH_COEFFICIENTS; ++i)
        {
            dest[uint2(i, 0)] = float4(g_constants.normalizationFactor * sum.c[i], 1.f);
        }
    }
}