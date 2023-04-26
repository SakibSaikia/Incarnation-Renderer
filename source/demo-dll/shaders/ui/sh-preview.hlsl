#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
#define THREAD_GROUP_SIZE_Y 1
#endif

#include "common/math.hlsli"
#include "lighting/pbr.hlsli"
#include "image-based-lighting/spherical-harmonics/common.hlsli"

#define rootsig \
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=6)," \
	"StaticSampler(s0, space = 1, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, addressW = TEXTURE_ADDRESS_WRAP)"

SamplerState g_trilinearSampler : register(s0, space1);

cbuffer cb : register(b0)
{
	uint2 g_texSize;
	uint g_shTextureIndex;
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
			(dispatchThreadId.y + 0.5f) / (float)g_texSize.y);

		 float2 polarAngles = LatlongUV2Polar(uv);
		 float3 dir = Polar2Cartesian(polarAngles.x, polarAngles.y, CoordinateSpace::World);

		float3 radiance = 0.f;
		SH9 basis = ShEvaluate(dir);
		Texture2D shTex = ResourceDescriptorHeap[g_shTextureIndex];

		[UNROLL]
		for (int i = 0; i < SH_NUM_COEFFICIENTS; ++i)
		{
			float3 coeff = shTex.Load(int3(i, 0, 0)).rgb;
			radiance += coeff * basis.value[i];
		}

		float3 hdrColor = g_skyBrightness * radiance;

		// Exposure correction and tonemapping
		float e = Exposure(g_exposure);
		hdrColor *= e;
		float3 ldrColor = ACESFilm(hdrColor);

		RWTexture2D<float4> dest = ResourceDescriptorHeap[g_uavIndex];
		dest[dispatchThreadId.xy] = float4(ldrColor, 1.f);
	}
}