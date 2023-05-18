#include "gpu-shared-types.h"

#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
#define THREAD_GROUP_SIZE_Y 1
#endif

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=3)," \
    "CBV(b1)," \
    "CBV(b2)"

struct FPassConstants
{
    uint m_aoTargetUavIndex;
    uint m_depthTargetSrvIndex;
    uint m_gbufferNormalsSrvIndex;
};

ConstantBuffer<FPassConstants> g_passCb : register(b0);
ConstantBuffer<FViewConstants> g_viewCb : register(b1);
ConstantBuffer<FSceneConstants> g_sceneCb : register(b2);


[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x < g_viewCb.m_resX && dispatchThreadId.y < g_viewCb.m_resY)
    {
        RWTexture2D<float> aoTarget = ResourceDescriptorHeap[g_passCb.m_aoTargetUavIndex];
        aoTarget[dispatchThreadId.xy] = 1.f;
    }
}