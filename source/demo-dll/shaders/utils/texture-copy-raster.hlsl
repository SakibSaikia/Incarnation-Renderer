#define rootsig \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_MIN_MAG_MIP_POINT, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP), " \
    "RootConstants(b0, num32BitConstants=1, visibility = SHADER_VISIBILITY_PIXEL)," \
    "DescriptorTable(SRV(t0, space = 0, numDescriptors = 1000, flags = DESCRIPTORS_VOLATILE), visibility = SHADER_VISIBILITY_PIXEL)"

SamplerState g_pointSampler : register(s0);
Texture2D g_bindlessTextures[] : register(t0);

cbuffer cb : register(b0)
{
	int textureIndex;
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
	return g_bindlessTextures[textureIndex].Sample(g_pointSampler, input.uv);
}