#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=8, visibility = SHADER_VISIBILITY_ALL),"

static const float PI = 3.14159265f;

cbuffer cb : register(b0)
{
    uint g_srcUavIndex;
    uint g_destUavIndex;
    uint g_srcMipIndex;
    uint g_destMipIndex;
    float2 g_uvScale;
};

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, THREAD_GROUP_SIZE_Z)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWTexture2DArray<float4> srcMip = ResourceDescriptorHeap[g_srcUavIndex];
    RWTexture2DArray<float4> destMip = ResourceDescriptorHeap[g_destUavIndex];

    uint2 destIndex = dispatchThreadId.xy;
    uint arrayIndex = dispatchThreadId.z;

    uint2 srcIndex = floor(destIndex * g_uvScale);

    float4 sum = 0;
    if (all(g_uvScale > 1.f))
    {
        destMip[uint3(destIndex.x, destIndex.y, arrayIndex)] =
            srcMip[uint3(srcIndex.x, srcIndex.y, arrayIndex)] +
            srcMip[uint3(srcIndex.x + 1, srcIndex.y, arrayIndex)] +
            srcMip[uint3(srcIndex.x, srcIndex.y + 1, arrayIndex)] +
            srcMip[uint3(srcIndex.x + 1, srcIndex.y + 1, arrayIndex)];
    }
    else if (g_uvScale.x > 1.f)
    {
        destMip[uint3(destIndex.x, destIndex.y, arrayIndex)] =
            srcMip[uint3(srcIndex.x, srcIndex.y, arrayIndex)] +
            srcMip[uint3(srcIndex.x + 1, srcIndex.y, arrayIndex)];
    }
    else if (g_uvScale.y > 1.f)
    {
        destMip[uint3(destIndex.x, destIndex.y, arrayIndex)] =
            srcMip[uint3(srcIndex.x, srcIndex.y, arrayIndex)] +
            srcMip[uint3(srcIndex.x, srcIndex.y + 1, arrayIndex)];
    }
}