#include "lighting/pbr.hlsli"

#define rootsig \
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_MIN_MAG_MIP_POINT, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP), " \
    "RootConstants(b0, num32BitConstants=3, visibility = SHADER_VISIBILITY_PIXEL)"

SamplerState g_pointSampler : register(s0);

cbuffer cb : register(b0)
{
	int g_hdrInputTextureIndex;
	float g_exposure;
	int g_enableNaNCheck;
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
	float3 hdrColor = hdrTex.Sample(g_pointSampler, input.uv).rgb;

#if VIEWMODE == 2 || VIEWMODE == 3 || VIEWMODE == 4
	return float4(hdrColor, 1.f);
#endif

	// Exposure correction. Computes the exposure normalization from the camera's EV100
	float e = Exposure(g_exposure);
	hdrColor *= e;

	// Tonemapping
	float3 ldrColor = ACESFilm(hdrColor);

	if (g_enableNaNCheck != 0)
	{
		if (isnan(hdrColor.x) || isnan(hdrColor.y) || isnan(hdrColor.z))
		{
			return float4(1.f, 0.f, 0.f, 1.f);
		}
		else
		{
			float luminance = dot(ldrColor, float3(0.299, 0.587, 0.114));
			return luminance.xxxx;
		}
	}

	return float4(ldrColor, 1.f);
}