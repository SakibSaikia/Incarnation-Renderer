#include "image-based-lighting/spherical-harmonics/common.hlsli"

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=3, visibility = SHADER_VISIBILITY_ALL)"

struct CbLayout
{
    uint srcUavIndex;
    uint destUavIndex;
};

ConstantBuffer<CbLayout> g_constants : register(b0);


[numthreads(1, 1, 1)]
void cs_main(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint globalIndex : SV_GroupIndex)
{
    RWTexture2DArray<float4> src = ResourceDescriptorHeap[g_constants.srcUavIndex];
    RWTexture2D<float4> dest = ResourceDescriptorHeap[g_constants.destUavIndex];

    [unroll]
    for (int i = 0; i < SH_NUM_COEFFICIENTS; ++i)
    {
        // Assuming a latlong texture of aspect ratio 2.0, the integration of the SH coefficients
        // results in a 2x1 mip. Both texels are summed here before exporting to the destination.
        dest[uint2(i, 0)] = src[uint3(0, 0, i)] + src[uint3(1, 0, i)];
    }
}