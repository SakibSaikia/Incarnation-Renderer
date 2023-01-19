#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
#define THREAD_GROUP_SIZE_Y 1
#endif

#include "common/math.hlsli"
#include "preetham.hlsli"

#define rootsig \
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=27)"

SamplerState g_anisoSampler : register(s0);

cbuffer cb : register(b0)
{
	FPerezDistribution g_perezConstants;
	float g_turbidity;
	float3 g_sunDir;
	uint2 g_texSize;
	uint g_uavIndex;
}

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x < g_texSize.x &&
		dispatchThreadId.y < g_texSize.y)
	{
		// Normalized coordinates
		float2 ndc;
		ndc.x = 2.f * dispatchThreadId.x / (float)g_texSize.x - 1.f;
		ndc.y = -2.f * dispatchThreadId.y / (float)g_texSize.y + 1.f;

		// Convert to polar angles
		float theta = ndc.y * k_Pi * 0.5f;
		float phi = ndc.x * k_Pi;

		// Get direction from polar angles
		float3 dir;
		dir.x = sin(theta) * cos(phi);
		dir.z = sin(theta) * sin(phi);
		dir.y = cos(theta);

		RWTexture2D<float4> dest = ResourceDescriptorHeap[g_uavIndex];
		dest[dispatchThreadId.xy].rgb = 1000 * CalculateSkyRadianceRGB(g_sunDir, normalize(dir), g_turbidity, g_perezConstants);
	}
}