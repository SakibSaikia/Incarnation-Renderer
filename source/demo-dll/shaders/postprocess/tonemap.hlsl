#include "lighting/pbr.hlsli"

#define rootsig \
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_MIN_MAG_MIP_POINT, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP), " \
    "RootConstants(b0, num32BitConstants=2, visibility = SHADER_VISIBILITY_PIXEL)"

SamplerState g_pointSampler : register(s0);

cbuffer cb : register(b0)
{
	int g_hdrInputTextureIndex;
	float g_exposure;
}

struct vs_to_ps
{
	float4 pos : SV_POSITION;
	float2 uv : INTERPOLATED_UV_0;
};

vs_to_ps vs_main(uint id : SV_VertexID)
{
	vs_to_ps o;
	o.uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(o.uv * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);
	return o;
}

float4 ps_main(vs_to_ps input) : SV_Target
{
	Texture2D hdrTex = ResourceDescriptorHeap[g_hdrInputTextureIndex];
	float3 luminance = hdrTex.Sample(g_pointSampler, input.uv).rgb;

	// Exposure correction. Computes the exposure normalization from the camera's EV100
	float e = exposure(g_exposure);
	luminance *= e;

	// Tonemapping
	float3 ldrColor = Reinhard(luminance);

	return float4(ldrColor, 1.f);
}