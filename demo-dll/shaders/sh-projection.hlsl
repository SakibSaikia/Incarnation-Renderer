#include "spherical-harmonics.hlsli"

#define rootsig \
    "RootConstants(b0, num32BitConstants=5, visibility = SHADER_VISIBILITY_ALL)," \
    "DescriptorTable(SRV(t0, space = 0, numDescriptors = 1000, flags = DESCRIPTORS_VOLATILE), visibility = SHADER_VISIBILITY_ALL), " \
    "DescriptorTable(UAV(u0, space = 0, numDescriptors = 1000, flags = DESCRIPTORS_VOLATILE), visibility = SHADER_VISIBILITY_ALL), "

static const float PI = 3.14159265f;

struct CbLayout
{
    uint inputHdriIndex;
    uint outputUavIndex;
    uint hdriWidth;
    uint hdriHeight;
    uint hdriMip;
};

ConstantBuffer<CbLayout> g_constants : register(b0);
Texture2D g_srvBindless2DTextures[] : register(t0);
RWTexture2DArray<float4> g_uavBindless2DTextureArrays[] : register(u0);


[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID)
{
    // theta = elevation angle
    // phi = azimuth angle
    float theta = PI * dispatchThreadId.y / (float)g_constants.hdriHeight;
    float phi = 2.f * PI * dispatchThreadId.x / (float)g_constants.hdriWidth;

    SH9 sh = ShEvaluate(theta, phi);

    // Sample radiance from the HDRI
    const float lightIntensity = 25000.f;
    float4 radiance = lightIntensity * g_srvBindless2DTextures[g_constants.inputHdriIndex].Load(int3(dispatchThreadId.x, dispatchThreadId.y, g_constants.hdriMip));

    // Project radiance to SH basis
    RWTexture2DArray<float4> dest = g_uavBindless2DTextureArrays[g_constants.outputUavIndex];

    [UNROLL]
    for (int i = 0; i < SH_COEFFICIENTS; ++i)
    {
        dest[uint3(dispatchThreadId.x, dispatchThreadId.y, i)] = radiance * sh.c[i];
    }
}