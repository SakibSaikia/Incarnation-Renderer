#include "common/math.hlsli"
#include "image-based-lighting/spherical-harmonics/common.hlsli"

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=6, visibility = SHADER_VISIBILITY_ALL),"

struct CbLayout
{
    uint inputHdriIndex;
    uint outputUavIndex;
    uint hdriWidth;
    uint hdriHeight;
    uint hdriMip;
    float radianceScale;
};

ConstantBuffer<CbLayout> g_constants : register(b0);

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID)
{
    // Polar angles
    float elevation = k_Pi * dispatchThreadId.y / (float)g_constants.hdriHeight;
    float azimuth = 2.f * k_Pi * dispatchThreadId.x / (float)g_constants.hdriWidth;

    SH9 sh = ShEvaluate(elevation, azimuth);

    // Sample radiance from the HDRI
    Texture2D inputHdriTex = ResourceDescriptorHeap[g_constants.inputHdriIndex];
    float4 radiance = inputHdriTex.Load(int3(dispatchThreadId.x, dispatchThreadId.y, g_constants.hdriMip));

    // Project the incoming radiance (from the cubemap) to SH basis
    RWTexture2DArray<float4> dest = ResourceDescriptorHeap[g_constants.outputUavIndex];

    [unroll]
    for (int i = 0; i < SH_COEFFICIENTS; ++i)
    {
        dest[uint3(dispatchThreadId.x, dispatchThreadId.y, i)] = g_constants.radianceScale * radiance * sh.c[i];
    }
}