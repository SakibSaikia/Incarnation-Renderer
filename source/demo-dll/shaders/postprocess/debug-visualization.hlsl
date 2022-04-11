#include "common/math.hlsli"
#include "common/color-space.hlsli"
#include "common/uniform-sampling.hlsli"
#include "geo-raster/encoding.hlsli"

#define rootsig \
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_MIN_MAG_MIP_POINT, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP), " \
    "RootConstants(b0, num32BitConstants=4, visibility = SHADER_VISIBILITY_PIXEL)"

SamplerState g_pointSampler : register(s0);

cbuffer cb : register(b0)
{
	int g_visbufferTextureIndex;
	int g_viewmode;
	uint g_resX;
	uint g_resY;
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
	if (g_viewmode == 8 || g_viewmode == 9)
	{
		Texture2D<int> visbufferTex = ResourceDescriptorHeap[g_visbufferTextureIndex];
		int visbufferValue = visbufferTex.Load(int3(input.uv.x * g_resX, input.uv.y * g_resY, 0));

		uint objectId, triangleId;
		DecodeVisibilityBuffer(visbufferValue, objectId, triangleId);
		return g_viewmode == 8 ? 
			hsv2rgb(float3(Halton(objectId, 3) * 360.f, 0.85f, 0.95f)).rgbr :
			hsv2rgb(float3(Halton(triangleId, 5) * 360.f, 0.85f, 0.95f)).rgbr;
	}

	return 0.xxxx;
}