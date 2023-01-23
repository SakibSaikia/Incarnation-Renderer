#define rootsig \
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=4)"

cbuffer cb : register(b0)
{
    uint g_resX;
    uint g_resY;
	uint g_srcUavIndex;
	uint g_dstUavIndex;
}

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 index : SV_DispatchThreadID)
{
    // x & y are correspond to indices in dst UAV
    const uint x = index.x;
    const uint y = index.y; 

    if (x < g_resX && y < g_resY)
    {
        RWTexture2D<float4> src = ResourceDescriptorHeap[g_srcUavIndex];
        RWTexture2D<float4> dst = ResourceDescriptorHeap[g_dstUavIndex];

        // Indices into src UAV that are used for filtering
        uint2 tap[] =
        {
            {2 * x, 2 * y},     { 2 * x + 1, 2 * y},
            {2 * x, 2 * y + 1}, {2 * x + 1, 2 * y + 1}
        };

        dst[index.xy] = 0.25 * (src[tap[0]] + src[tap[1]] + src[tap[2]] + src[tap[3]]);
    }
}