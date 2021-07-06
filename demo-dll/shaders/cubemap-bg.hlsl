#include "pbr.hlsli"
#define rootsig \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_ANISOTROPIC, maxAnisotropy = 8, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP), " \
    "RootConstants(b0, num32BitConstants=18, visibility = SHADER_VISIBILITY_PIXEL)," \
    "DescriptorTable(SRV(t0, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_PIXEL)"

SamplerState g_anisoSampler : register(s0);
TextureCube g_bindlessCubeTextures[] : register(t0);

cbuffer cb : register(b0)
{
	float4x4 g_invParallaxViewProjMatrix;
	uint g_envmapTextureIndex;
	float g_exposure;
}

struct vs_to_ps
{
	float4 pos : SV_POSITION;
	float4 pixelPos : INTERPOLATED_POS;
	float2 uv : INTERPOLATED_UV_0;
};

vs_to_ps vs_main(uint id : SV_VertexID)
{
	vs_to_ps o;

	// generate clip space position
	o.pos.x = (float)(id / 2) * 4.f - 1.f;
	o.pos.y = (float)(id % 2) * 4.f - 1.f;
	o.pos.z = 0.0001f;
	o.pos.w = 1.f;

	// texture coordinates
	o.uv.x = (float)(id / 2) * 2.f;
	o.uv.y = 1.f - (float)(id % 2) * 2.f;

	o.pixelPos = o.pos;

	return o;
}

float4 ps_main(vs_to_ps input) : SV_Target
{
	float4 worldPos = mul(input.pixelPos, g_invParallaxViewProjMatrix);
	worldPos /= worldPos.w;

	float3 luminance = g_bindlessCubeTextures[g_envmapTextureIndex].Sample(g_anisoSampler, worldPos.xyz).rgb;

	// Exposure correction. Computes the exposure normalization from the camera's EV100
	float e = exposure(g_exposure);
	luminance *= e;

	// Tonemapping
	float3 ldrColor = Reinhard(luminance);

	return float4(ldrColor, 1.f);
}