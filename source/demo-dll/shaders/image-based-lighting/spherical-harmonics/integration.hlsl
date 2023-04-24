#include "image-based-lighting/spherical-harmonics/common.hlsli"

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=2, visibility = SHADER_VISIBILITY_ALL),"

static const float PI = 3.14159265f;

struct CbLayout
{
    uint srcUavIndex;
    uint destUavIndex;
};

ConstantBuffer<CbLayout> g_constants : register(b0);

#define NUM_SLICES THREAD_GROUP_SIZE_Z
groupshared SH9ColorCoefficient g_sum[NUM_SLICES];

// For parallel reduction, see https://gpuopen.com/wp-content/uploads/2017/07/GDC2017-Wave-Programming-D3D12-Vulkan.pdf
[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, THREAD_GROUP_SIZE_Z)]
void cs_main(
    uint3 dispatchThreadId : SV_DispatchThreadID, 
    uint3 groupThreadId : SV_GroupThreadID, 
    uint3 groupId : SV_GroupID)
{
    RWTexture2DArray<float4> src = ResourceDescriptorHeap[g_constants.srcUavIndex];
    RWTexture2DArray<float4> dest = ResourceDescriptorHeap[g_constants.destUavIndex];

    // Sum SH coefficients for all threads in the wave
    SH9ColorCoefficient sum;
    int i;
    [unroll]
    for (i = 0; i < SH_NUM_COEFFICIENTS; ++i)
    {
        float3 coeff = src[uint3(dispatchThreadId.x * dispatchThreadId.z, dispatchThreadId.y, i)].rgb;
        sum.c[i] = WaveActiveSum(coeff);
    }

    // Use 0th lane in the wave to write out integration results to LDS so that we can calculate for entire thread group
    if (WaveGetLaneIndex() == 0)
    {
        [unroll]
        for (int i = 0; i < SH_NUM_COEFFICIENTS; ++i)
        {
            g_sum[groupThreadId.z].c[i] = sum.c[i];
        }
    }

    // Sync
    GroupMemoryBarrierWithGroupSync();

    // Compute for all waves in the thread group
    SH9ColorCoefficient result = { {0.f.xxx, 0.f.xxx, 0.f.xxx, 0.f.xxx, 0.f.xxx, 0.f.xxx, 0.f.xxx, 0.f.xxx, 0.f.xxx} };
    [unroll]
    for (int k = 0; k < SH_NUM_COEFFICIENTS; ++k)
    {
        [unroll]
        for (int i = 0; i < NUM_SLICES; ++i)
        {
            result.c[k] += g_sum[i].c[k];
        }
    }

    // Write results to the UAV
    [unroll]
    for (i = 0; i < SH_NUM_COEFFICIENTS; ++i)
    {
        dest[uint3(groupId.x, groupId.y, i)] = float4(result.c[i], 1.f);
    }
}