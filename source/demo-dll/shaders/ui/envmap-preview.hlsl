#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
#define THREAD_GROUP_SIZE_Y 1
#endif

#include "common/math.hlsli"
#include "lighting/pbr.hlsli"

#define rootsig \
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=4)," \
	"StaticSampler(s0, space = 1, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, addressW = TEXTURE_ADDRESS_WRAP)"

SamplerState g_trilinearSampler : register(s0, space1);

cbuffer cb : register(b0)
{
	uint2 g_texSize;
	uint g_envmapTextureIndex;
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
		float3 dir = Polar2Rect(theta, phi, true);

		// HDR color
		TextureCube envMapTexture = ResourceDescriptorHeap[g_envmapTextureIndex];
		float3 hdrColor = envMapTexture.SampleLevel(g_trilinearSampler, dir, 0).rgb;

		// Exposure correction and tonemapping
		float e = Exposure(13.f);
		hdrColor *= e;
		float3 ldrColor = ACESFilm(hdrColor);

		RWTexture2D<float4> dest = ResourceDescriptorHeap[g_uavIndex];
		dest[dispatchThreadId.xy] = float4(ldrColor, 1.f);
	}
}