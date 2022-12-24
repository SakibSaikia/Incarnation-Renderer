#include "preetham.hlsli"

#define rootsig \
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)"

SamplerState g_anisoSampler : register(s0);

cbuffer cb : register(b0)
{
	float4x4 g_invParallaxViewProjMatrix;
	FPerezDistribution g_perezConstants;
	float g_turbidity;
	float3 g_sunDir;
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
	float4 dir = mul(input.pixelPos, g_invParallaxViewProjMatrix);
	dir /= dir.w;

	float3 radiance = 1000 * CalculateSkyRadianceRGB(g_sunDir, normalize(dir.xyz), g_turbidity, g_perezConstants);

	return float4(radiance, 0.f);
}