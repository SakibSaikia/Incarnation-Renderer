#include "common/math.hlsli"
#include "image-based-lighting/spherical-harmonics/common.hlsli"

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=5, visibility = SHADER_VISIBILITY_ALL),"

struct CbLayout
{
    uint inputHdriIndex;
    uint outputUavIndex;
    uint hdriWidth;
    uint hdriHeight;
    uint hdriMip;
};

ConstantBuffer<CbLayout> g_constants : register(b0);

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID)
{
    float2 uv = float2(
        (dispatchThreadId.x + 0.5f) / (float)g_constants.hdriWidth,
        (dispatchThreadId.y + 0.5f) / (float)g_constants.hdriHeight);

    // Convert from UV to polar angles
    float2 polarAngles = LatlongUV2Polar(uv);

    // Get direction from polar angles
    float3 dir = Polar2Cartesian(polarAngles.x, polarAngles.y, CoordinateSpace::World);

    // Evaluate SH basis at given direction
    SH9 shBasis = ShEvaluate(dir);

    // Sample radiance from the HDRI
    Texture2D inputHdriTex = ResourceDescriptorHeap[g_constants.inputHdriIndex];
    float4 radiance = inputHdriTex.Load(int3(dispatchThreadId.x, dispatchThreadId.y, g_constants.hdriMip));

    // Project the incoming radiance (from the cubemap) to SH basis
    // See "Signal Encoding" in http://www.patapom.com/blog/SHPortal/
    RWTexture2DArray<float4> dest = ResourceDescriptorHeap[g_constants.outputUavIndex];
    const float sint = sin(polarAngles.x);
    const float dTheta = k_Pi / (float)g_constants.hdriHeight;
    const float dPhi = 2.f * k_Pi / (float)g_constants.hdriWidth;

    [unroll]
    for (int i = 0; i < SH_NUM_COEFFICIENTS; ++i)
    {
        dest[uint3(dispatchThreadId.x, dispatchThreadId.y, i)] = radiance * shBasis.value[i] * sint * dTheta * dPhi;
    }
}