#include "pbr.hlsli"

#define rootsig \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_ANISOTROPIC, maxAnisotropy = 8, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE), " \
    "StaticSampler(s1, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, comparisonFunc = COMPARISON_LESS_EQUAL, addressU = TEXTURE_ADDRESS_BORDER, addressV = TEXTURE_ADDRESS_BORDER, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE), " \
    "RootConstants(b0, num32BitConstants=20, visibility = SHADER_VISIBILITY_VERTEX)," \
    "CBV(b1, space = 0, visibility = SHADER_VISIBILITY_PIXEL"), \
    "CBV(b2, space = 0, visibility = SHADER_VISIBILITY_ALL"), \
    "CBV(b3, space = 0, visibility = SHADER_VISIBILITY_ALL"), \
    "DescriptorTable(SRV(t0, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_PIXEL), " \
    "DescriptorTable(SRV(t1, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_VERTEX), " \
    "DescriptorTable(SRV(t2, space = 1, numDescriptors = 1000), visibility = SHADER_VISIBILITY_PIXEL) "

struct FrameCbLayout
{
	float4x4 sceneRotation;
	uint sceneIndexBufferBindlessIndex;
	uint scenePositionBufferBindlessIndex;
	uint sceneNormalBufferBindlessIndex;
	uint sceneUvBufferBindlessIndex;
	int envmapTextureIndex;
};

struct ViewCbLayout
{
	float4x4 viewTransform;
	float4x4 projectionTransform;
};

struct MeshCbLayout
{
	float4x4 localToWorld;
	uint indexOffset;
	uint positionOffset;
	uint normalOffset;
	uint uvOffset;
};

struct MaterialCbLayout
{
	float3 emissiveFactor;
	float metallicFactor;
	float3 baseColorFactor;
	float roughnessFactor;
	int baseColorTextureIndex;
	int metallicRoughnessTextureIndex;
	int normalTextureIndex;
	int baseColorSamplerIndex;
	int metallicRoughnessSamplerIndex;
	int normalSamplerIndex;
};

SamplerState g_anisoSampler : register(s0);
ConstantBuffer<MeshCbLayout> g_meshConstants : register(b0);
ConstantBuffer<MaterialCbLayout> g_materialConstants : register(b1);
ConstantBuffer<ViewCbLayout> g_viewConstants : register(b2);
ConstantBuffer<FrameCbLayout> g_frameConstants : register(b3);
Texture2D g_bindless2DTextures[] : register(t0, space0);
ByteAddressBuffer g_bindlessBuffers[] : register(t1, space0);
TextureCube g_bindlessCubeTextures[] : register(t2, space1);

struct vs_to_ps
{
	float4 pos : SV_POSITION;
	float4 normal : INTERPOLATED_WORLD_NORMAL;
	float4 worldPos : INTERPOLATED_WORLD_POS;
	float2 uv : INTERPOLATED_UV_0;
};

float3 Eye()
{
	return float3(
		-g_viewConstants.viewTransform._41, 
		-g_viewConstants.viewTransform._42, 
		-g_viewConstants.viewTransform._43);
}

vs_to_ps vs_main(uint vertexId : SV_VertexID)
{
	vs_to_ps o;

	// size of 4 for 32 bit indices
	uint index = g_bindlessBuffers[g_frameConstants.sceneIndexBufferBindlessIndex].Load(4*(vertexId + g_meshConstants.indexOffset));

	// size of 12 for float3 positions
	float3 position = g_bindlessBuffers[g_frameConstants.scenePositionBufferBindlessIndex].Load<float3>(12 * (index + g_meshConstants.positionOffset));

	// size of 12 for float3 normals
	float3 normal = g_bindlessBuffers[g_frameConstants.sceneNormalBufferBindlessIndex].Load<float3>(12 * (index + g_meshConstants.normalOffset));

	// size of 8 for float2 uv's
	float2 uv = g_bindlessBuffers[g_frameConstants.sceneUvBufferBindlessIndex].Load<float2>(8 * (index + g_meshConstants.uvOffset));

	float4x4 localToWorld = mul(g_meshConstants.localToWorld, g_frameConstants.sceneRotation);
	float4 worldPos = mul(float4(position, 1.f), localToWorld);
	float4x4 viewProjTransform = mul(g_viewConstants.viewTransform, g_viewConstants.projectionTransform);

	o.pos = mul(worldPos, viewProjTransform);
	o.normal = mul(float4(normal, 0.f), g_meshConstants.localToWorld);
	o.worldPos = worldPos;
	o.uv = uv;

	return o;
}

float4 ps_main(vs_to_ps input) : SV_Target
{
	float3 n = normalize(input.normal.xyz);
	float3 l = normalize(float3(1, 1, -1));
	float3 h = normalize(n + l);
	float3 v = normalize(Eye() - input.worldPos.xyz / input.worldPos.w);

	float NoV = abs(dot(n, v)) + 1e-5;
	float NoL = saturate(dot(n, l));
	float NoH = saturate(dot(n, h));
	float LoH = saturate(dot(l, h));

	float3 baseColor = g_materialConstants.baseColorTextureIndex != -1 ? g_materialConstants.baseColorFactor * g_bindless2DTextures[g_materialConstants.baseColorTextureIndex].Sample(g_anisoSampler, input.uv).rgb : g_materialConstants.baseColorFactor;
	float2 metallicRoughnessMap = g_materialConstants.metallicRoughnessTextureIndex != -1 ? g_bindless2DTextures[g_materialConstants.metallicRoughnessTextureIndex].Sample(g_anisoSampler, input.uv).bg : 1.f.xx;
	float metallic = g_materialConstants.metallicFactor * metallicRoughnessMap.x;
	float perceptualRoughness = g_materialConstants.roughnessFactor * metallicRoughnessMap.y;

	// Remapping
	float3 f0 = metallic * baseColor + (1.f - metallic) * 0.04;
	float3 albedo = (1.f - metallic) * baseColor;
	float roughness = perceptualRoughness * perceptualRoughness;

	float D = D_GGX(NoH, roughness);
	float3 F = F_Schlick(LoH, f0);
	float V = V_SmithGGXCorrelated(NoV, NoL, roughness);

	// Specular BRDF
	float3 Fr = D * V * F;

	// diffuse BRDF
	float3 Fd = albedo * Fd_Lambert();

	// Apply lighting
	const float lightIntensity = 100000.f;
	float illuminance = lightIntensity * NoL;
	float3 luminance = (Fr + Fd) * illuminance;

	// Exposure. Computes the exposure normalization from the camera's EV100
	int ev100 = 15;
	float e = exposure(ev100);

	// Normalized luminance
	luminance *= e;

	return float4(luminance, 1.f);
}