#include "lighting/pbr.hlsli"

#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
#define THREAD_GROUP_SIZE_Y 1
#endif

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=6, visibility = SHADER_VISIBILITY_ALL)"

struct CbLayout
{
    uint hdrSceneColorTextureIndex;
    uint taaAccumulationUavIndex;
    uint resX;
    uint resY;
    uint historyIndex;
    float exposure;
};

ConstantBuffer<CbLayout> g_constants : register(b0);

float3 Tonemap(float3 hdrColor)
{
    float e = Exposure(g_constants.exposure);
    return Reinhard(e * hdrColor);
}

float3 InverseTonemap(float3 ldrColor)
{
    float3 hdrColor = ldrColor / (1.f - ldrColor);
    float e = Exposure(g_constants.exposure);
    return hdrColor / e;
}


[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    Texture2D hdrSceneColor = ResourceDescriptorHeap[g_constants.hdrSceneColorTextureIndex];
    RWTexture2D<float3> taaAccumulationBuffer = ResourceDescriptorHeap[g_constants.taaAccumulationUavIndex];

    if (dispatchThreadId.x < g_constants.resX && dispatchThreadId.y < g_constants.resY)
    {
        float3 currentColor = hdrSceneColor.Load(uint3(dispatchThreadId.x, dispatchThreadId.y, 0)).rgb;

        if (g_constants.historyIndex == 0)
        {
            taaAccumulationBuffer[dispatchThreadId.xy] = currentColor;
        }
        else
        {
            float3 previousColor = taaAccumulationBuffer[dispatchThreadId.xy];
            float3 output = Tonemap(currentColor) * 0.1f + Tonemap(previousColor) * 0.9f;
            taaAccumulationBuffer[dispatchThreadId.xy] = InverseTonemap(output);
        }
    }
}