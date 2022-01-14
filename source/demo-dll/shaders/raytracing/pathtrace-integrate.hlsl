#define rootsig \
    "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=5, visibility = SHADER_VISIBILITY_ALL),"

struct CbLayout
{
    uint currentBufferTextureIndex;
    uint historyBufferUavIndex;
    uint historyFrameCount;
    uint resX;
    uint resY;
};

ConstantBuffer<CbLayout> g_constants : register(b0);

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x < g_constants.resX && dispatchThreadId.y < g_constants.resY)
    {
        Texture2D currentBuffer = ResourceDescriptorHeap[g_constants.currentBufferTextureIndex];
        float3 currentColor = currentBuffer.Load(uint3(dispatchThreadId.x, dispatchThreadId.y, 0)).rgb;

        RWTexture2D<float3> historyBuffer = ResourceDescriptorHeap[g_constants.historyBufferUavIndex];
        float3 historyColor = historyBuffer[dispatchThreadId.xy];

        //float3 avgColor = ((g_constants.historyFrameCount - 1.f) * historyColor + currentColor) / (float)g_constants.historyFrameCount;
        float3 avgColor = g_constants.historyFrameCount <= 1 ? currentColor : currentColor * 0.1 + historyColor * 0.9;
        historyBuffer[uint2(dispatchThreadId.x, dispatchThreadId.y)] = avgColor;
    }
}