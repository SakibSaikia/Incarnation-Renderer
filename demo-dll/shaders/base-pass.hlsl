#include "pbr.hlsli"
#include "spherical-harmonics.hlsli"

#define rootsig \
    "RootConstants(b0, num32BitConstants=22, visibility = SHADER_VISIBILITY_VERTEX)," \
    "CBV(b1, space = 0, visibility = SHADER_VISIBILITY_PIXEL"), \
    "CBV(b2, space = 0, visibility = SHADER_VISIBILITY_ALL"), \
    "CBV(b3, space = 0, visibility = SHADER_VISIBILITY_ALL"), \
    "DescriptorTable(SRV(t0, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_PIXEL), " \
    "DescriptorTable(SRV(t1, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_VERTEX), " \
    "DescriptorTable(SRV(t2, space = 1, numDescriptors = 1000), visibility = SHADER_VISIBILITY_PIXEL), " \
	"DescriptorTable(Sampler(s0, space = 0, numDescriptors = 16), visibility = SHADER_VISIBILITY_PIXEL), " \
	"StaticSampler(s1, space = 1, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_COMPARISON_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE), " \

struct LightProbeData
{
	int envmapTextureIndex;
	int shTextureIndex;
};

struct FrameCbLayout
{
	float4x4 sceneRotation;
	int sceneIndexBufferBindlessIndex;
	int scenePositionBufferBindlessIndex;
	int sceneUvBufferBindlessIndex;
	int sceneNormalBufferBindlessIndex;
	int sceneTangentBufferBindlessIndex;
	int sceneBitangentBufferBindlessIndex;
	int envBrdfTextureIndex;
	int _pad0;
	LightProbeData sceneLightProbe;
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
	uint uvOffset;
	uint normalOffset;
	uint tangentOffset;
	uint bitangentOffset;
};

struct MaterialCbLayout
{
	float3 emissiveFactor;
	float metallicFactor;
	float3 baseColorFactor;
	float roughnessFactor;
	float occlusionStrength;
	int emissiveTextureIndex;
	int baseColorTextureIndex;
	int metallicRoughnessTextureIndex;
	int normalTextureIndex;
	int aoTextureIndex;
	int emissiveSamplerIndex;
	int baseColorSamplerIndex;
	int metallicRoughnessSamplerIndex;
	int normalSamplerIndex;
	int aoSamplerIndex;
};

SamplerState g_bindlessSamplers[] : register(s0, space0);
SamplerState g_trilinearSampler : register(s1, space1);
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
	float2 uv : INTERPOLATED_UV_0;
	float4 normal : INTERPOLATED_WORLD_NORMAL;
	float4 tangent : INTERPOLATED_WORLD_TANGENT;
	float4 bitangent : INTERPOLATED_WORLD_BITANGENT;
	float4 worldPos : INTERPOLATED_WORLD_POS;
};

vs_to_ps vs_main(uint vertexId : SV_VertexID)
{
	vs_to_ps o;

	float4x4 localToWorld = mul(g_meshConstants.localToWorld, g_frameConstants.sceneRotation);
	float4x4 viewProjTransform = mul(g_viewConstants.viewTransform, g_viewConstants.projectionTransform);

	// size of 4 for 32 bit indices
	uint index = g_bindlessBuffers[g_frameConstants.sceneIndexBufferBindlessIndex].Load(4*(vertexId + g_meshConstants.indexOffset));

	// size of 12 for float3 positions
	const int bindlessIndexBuffer = g_frameConstants.scenePositionBufferBindlessIndex;
	float3 position = g_bindlessBuffers[bindlessIndexBuffer].Load<float3>(12 * (index + g_meshConstants.positionOffset));
	float4 worldPos = mul(float4(position, 1.f), localToWorld);
	o.worldPos = worldPos;
	o.pos = mul(worldPos, viewProjTransform);

	// size of 8 for float2 uv's
	const int bindlessUvBuffer = g_frameConstants.sceneUvBufferBindlessIndex;
	o.uv = g_bindlessBuffers[bindlessUvBuffer].Load<float2>(8 * (index + g_meshConstants.uvOffset));

	// size of 12 for float3 normals
	const int bindlessNormalBuffer = g_frameConstants.sceneNormalBufferBindlessIndex;
	float3 normal = g_bindlessBuffers[bindlessNormalBuffer].Load<float3>(12 * (index + g_meshConstants.normalOffset));
	o.normal = mul(float4(normal, 0.f), localToWorld);

	// size of 12 for float3 tangents
	const int bindlessTangentBuffer = g_frameConstants.sceneTangentBufferBindlessIndex;
	float3 tangent = g_bindlessBuffers[bindlessTangentBuffer].Load<float3>(12 * (index + g_meshConstants.tangentOffset));
	o.tangent = mul(float4(tangent, 0.f), localToWorld);

	// size of 12 for float3 bitangents
	const int bindlessBitangentBuffer = g_frameConstants.sceneBitangentBufferBindlessIndex;
	float3 bitangent = g_bindlessBuffers[bindlessBitangentBuffer].Load<float3>(12 * (index + g_meshConstants.bitangentOffset));
	o.bitangent = mul(float4(bitangent, 0.f), localToWorld);

	return o;
}

float4 ps_main(vs_to_ps input) : SV_Target
{
	// Alpha clip
	float alpha = g_materialConstants.baseColorTextureIndex != -1 ? g_bindless2DTextures[g_materialConstants.baseColorTextureIndex].Sample(g_bindlessSamplers[g_materialConstants.baseColorSamplerIndex], input.uv).a : 1.f;
	clip(alpha - 0.5);

	// Tangent space transform
	float3 T = normalize(input.tangent.xyz);
	float3 B = normalize(input.bitangent.xyz);
	float3 N = normalize(input.normal.xyz);
	float3x3 TBN = float3x3(T, B, N);

	float3 emissive = g_materialConstants.emissiveTextureIndex != -1 ?
		g_materialConstants.emissiveFactor * g_bindless2DTextures[g_materialConstants.emissiveTextureIndex].Sample(g_bindlessSamplers[g_materialConstants.emissiveSamplerIndex], input.uv).rgb :
		g_materialConstants.emissiveFactor;

#if LIGHTING_ONLY
	float3 baseColor = 0.5.xxx;
#else
	float3 baseColor = g_materialConstants.baseColorTextureIndex != -1 ?
		g_materialConstants.baseColorFactor * g_bindless2DTextures[g_materialConstants.baseColorTextureIndex].Sample(g_bindlessSamplers[g_materialConstants.baseColorSamplerIndex], input.uv).rgb :
		g_materialConstants.baseColorFactor;
#endif

	if (g_materialConstants.normalTextureIndex != -1)
	{
		float2 normalXY = g_bindless2DTextures[g_materialConstants.normalTextureIndex].Sample(g_bindlessSamplers[g_materialConstants.normalSamplerIndex], input.uv).rg;
		float normalZ = sqrt(1.f - dot(normalXY, normalXY));
		N = normalize(mul(float3(normalXY, normalZ), TBN));
	}

	// Note that GLTF specifies metalness in blue channel and roughness in green channel but we swizzle them on import and
	// use a BC5 texture. So, metalness ends up in the red channel and roughness stays on the green channel.
	float2 metallicRoughnessMap = g_materialConstants.metallicRoughnessTextureIndex != -1 ?
		g_bindless2DTextures[g_materialConstants.metallicRoughnessTextureIndex].Sample(g_bindlessSamplers[g_materialConstants.metallicRoughnessSamplerIndex], input.uv).rg :
		1.f.xx;

	float ao = g_materialConstants.aoTextureIndex != -1 ?
		g_bindless2DTextures[g_materialConstants.aoTextureIndex].Sample(g_bindlessSamplers[g_materialConstants.aoSamplerIndex], input.uv).r :
		1.f;

	float aoStrength = g_materialConstants.occlusionStrength;
	float metallic = g_materialConstants.metallicFactor * metallicRoughnessMap.x;
	float perceptualRoughness = g_materialConstants.roughnessFactor * metallicRoughnessMap.y;

	float3 L = normalize(float3(1, 1, -1));
	float3 H = normalize(N + L);
	float3 V = normalize(g_viewConstants.eyePos - input.worldPos.xyz / input.worldPos.w);

	float NoV = saturate(dot(N, V));
	float NoL = saturate(dot(N, L));
	float NoH = saturate(dot(N, H));
	float LoH = saturate(dot(L, H));

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
		luminance += lerp(shDiffuse, ao * shDiffuse, aoStrength);
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
		float3 specularIBL = prefilteredColor * (F0 * envBrdf.x + envBrdf.y);
		luminance += lerp(specularIBL, ao * specularIBL, aoStrength);
	}
#endif

	// Exposure correction. Computes the exposure normalization from the camera's EV100
	float e = exposure(g_viewConstants.exposure);
	luminance *= e;

	// Tonemapping
	float3 ldrColor = Reinhard(luminance) + emissive;

	return float4(ldrColor, 1.f);
}