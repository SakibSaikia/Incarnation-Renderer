#include "pbr.hlsli"
#include "spherical-harmonics.hlsli"

#define rootsig \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_ANISOTROPIC, maxAnisotropy = 8, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE), " \
    "StaticSampler(s1, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_COMPARISON_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE), " \
    "RootConstants(b0, num32BitConstants=20, visibility = SHADER_VISIBILITY_VERTEX)," \
    "CBV(b1, space = 0, visibility = SHADER_VISIBILITY_PIXEL"), \
    "CBV(b2, space = 0, visibility = SHADER_VISIBILITY_ALL"), \
    "CBV(b3, space = 0, visibility = SHADER_VISIBILITY_ALL"), \
    "DescriptorTable(SRV(t0, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_PIXEL), " \
    "DescriptorTable(SRV(t1, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_VERTEX), " \
    "DescriptorTable(SRV(t2, space = 1, numDescriptors = 1000), visibility = SHADER_VISIBILITY_PIXEL) "

struct LightProbeData
{
	int envmapTextureIndex;
	int shTextureIndex;
};

struct FrameCbLayout
{
	float4x4 sceneRotation;
	uint sceneIndexBufferBindlessIndex;
	uint scenePositionBufferBindlessIndex;
	uint sceneNormalBufferBindlessIndex;
	uint sceneUvBufferBindlessIndex;
	LightProbeData sceneLightProbe;
	uint envBrdfTextureIndex;
};

struct ViewCbLayout
{
	float4x4 viewTransform;
	float4x4 projectionTransform;
	float3 eyePos;
	float exposure;
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
SamplerState g_trilinearSampler : register(s1);
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
	o.normal = mul(float4(normal, 0.f), localToWorld);
	o.worldPos = worldPos;
	o.uv = uv;

	return o;
}

float4 ps_main(vs_to_ps input) : SV_Target
{
	float3 N = normalize(input.normal.xyz);
	float3 L = normalize(float3(1, 1, -1));
	float3 H = normalize(N + L);
	float3 V = normalize(g_viewConstants.eyePos - input.worldPos.xyz / input.worldPos.w);

	float NoV = saturate(dot(N, V));
	float NoL = saturate(dot(N, L));
	float NoH = saturate(dot(N, H));
	float LoH = saturate(dot(L, H));

	float3 baseColor = g_materialConstants.baseColorTextureIndex != -1 ? g_materialConstants.baseColorFactor * g_bindless2DTextures[g_materialConstants.baseColorTextureIndex].Sample(g_anisoSampler, input.uv).rgb : g_materialConstants.baseColorFactor;
	float2 metallicRoughnessMap = g_materialConstants.metallicRoughnessTextureIndex != -1 ? g_bindless2DTextures[g_materialConstants.metallicRoughnessTextureIndex].Sample(g_anisoSampler, input.uv).bg : 1.f.xx;
	float metallic = g_materialConstants.metallicFactor * metallicRoughnessMap.x;
	float perceptualRoughness = g_materialConstants.roughnessFactor * metallicRoughnessMap.y;

#if LIGHTING_ONLY
	baseColor = 0.5.xxx;
#endif

	// Remapping
	float3 F0 = metallic * baseColor + (1.f - metallic) * 0.04;
	float3 albedo = (1.f - metallic) * baseColor;
	float roughness = perceptualRoughness * perceptualRoughness;

	float D = D_GGX(NoH, roughness);
	float3 F = F_Schlick(LoH, F0);
	float G = G_Smith_Direct(NoV, NoL, roughness);

	// Specular BRDF
	//float3 Fr = (D * F * G) / (4.f * NoV * NoL);
	float3 Fr = (D * F * G);

	// diffuse BRDF
	float3 Fd = albedo * Fd_Lambert();

	// Apply direct lighting
	const float lightIntensity = 100000.f;
	float illuminance = lightIntensity * NoL;
	float3 luminance = 0.f;
	
#if DIRECT_LIGHTING
	luminance += (Fr + Fd)* illuminance;
#endif

	// Diffuse IBL
#ifdef DIFFUSE_IBL
	if (g_frameConstants.sceneLightProbe.shTextureIndex != -1)
	{
		SH9Color shRadiance;
		Texture2D shTex = g_bindless2DTextures[g_frameConstants.sceneLightProbe.shTextureIndex];

		[UNROLL]
		for (int i = 0; i < SH_COEFFICIENTS; ++i)
		{
			shRadiance.c[i] = shTex.Load(int3(i, 0, 0)).rgb;
		}

		float3 shDiffuse = Fd * ShIrradiance(N, shRadiance);
		luminance += shDiffuse;
	}
#endif

	// Specular IBL
#if SPECULAR_IBL
	if (g_frameConstants.sceneLightProbe.envmapTextureIndex != -1 &&
		g_frameConstants.envBrdfTextureIndex != -1)
	{
		TextureCube prefilteredEnvMap = g_bindlessCubeTextures[g_frameConstants.sceneLightProbe.envmapTextureIndex];
		Texture2D envBrdfTex = g_bindless2DTextures[g_frameConstants.envBrdfTextureIndex];

		float texWidth, texHeight, mipCount;
		prefilteredEnvMap.GetDimensions(0, texWidth, texHeight, mipCount);

		float3 R = reflect(-V, N);
		float3 prefilteredColor = prefilteredEnvMap.SampleLevel(g_trilinearSampler, R, roughness * mipCount).rgb;
		float2 envBrdf = envBrdfTex.SampleLevel(g_trilinearSampler, float2(NoV, roughness), 0.f).rg;
		luminance += prefilteredColor * (F0 * envBrdf.x + envBrdf.y);
	}
#endif

	// Exposure correction. Computes the exposure normalization from the camera's EV100
	float e = exposure(g_viewConstants.exposure);
	luminance *= e;

	// Tonemapping
	float3 ldrColor = Reinhard(luminance);

	return float4(ldrColor, 1.f);
}