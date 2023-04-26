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
    "RootConstants(b0, num32BitConstants=6)," \
	"StaticSampler(s0, space = 1, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, addressW = TEXTURE_ADDRESS_WRAP)"

SamplerState g_trilinearSampler : register(s0, space1);

cbuffer cb : register(b0)
{
	uint2 g_texSize;
	uint g_envmapTextureIndex;
	uint g_uavIndex;
	float g_skyBrightness;
	float g_exposure;
}

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x < g_texSize.x &&
		dispatchThreadId.y < g_texSize.y)
	{
		float2 uv = float2(
			(dispatchThreadId.x + 0.5f) / (float)g_texSize.x, 
			(dispatchThreadId.y  + 0.5f) / (float)g_texSize.y);

		// Convert from UV to polar angle
		float2 polarAngles = LatlongUV2Polar(uv);

		// Get direction from polar angles
		float3 dir = Polar2Cartesian(polarAngles.x, polarAngles.y, CoordinateSpace::World);

		// HDR color
		TextureCube envMapTexture = ResourceDescriptorHeap[g_envmapTextureIndex];
		float3 hdrColor = g_skyBrightness * envMapTexture.SampleLevel(g_trilinearSampler, dir, 0).rgb;

		// Exposure correction and tonemapping
		float e = Exposure(g_exposure);
		hdrColor *= e;
		float3 ldrColor = ACESFilm(hdrColor);

		RWTexture2D<float4> dest = ResourceDescriptorHeap[g_uavIndex];
		dest[dispatchThreadId.xy] = float4(ldrColor, 1.f);
	}
}