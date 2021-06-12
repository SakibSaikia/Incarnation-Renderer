#include "pbr.hlsli"

#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
#define THREAD_GROUP_SIZE_Y 1
#endif

#define rootsig \
    "RootConstants(b0, num32BitConstants=4, visibility = SHADER_VISIBILITY_ALL)," \
    "DescriptorTable(UAV(u0, space = 0, numDescriptors = 1000, flags = DESCRIPTORS_VOLATILE), visibility = SHADER_VISIBILITY_ALL), "

struct CbLayout
{
    uint outputUavWidth;
    uint outputUavHeight;
    uint outputUavIndex;
    uint sampleCount;
};

ConstantBuffer<CbLayout> g_computeConstants : register(b0);
RWTexture2D<float2> g_uavBindless2DTextures[] : register(u0);

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x < g_computeConstants.outputUavWidth &&
        dispatchThreadId.y < g_computeConstants.outputUavHeight)
    {
        RWTexture2D<float2> dest = g_uavBindless2DTextures[g_computeConstants.outputUavIndex];
        dest[uint2(dispatchThreadId.x, dispatchThreadId.y)] = float2(0.2, 0.7);
    }
}