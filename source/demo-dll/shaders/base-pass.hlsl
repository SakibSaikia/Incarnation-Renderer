#include "pbr.hlsli"
#include "spherical-harmonics.hlsli"
#include "mesh-material.h"

#define rootsig \
    "RootConstants(b0, num32BitConstants=22, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b1, space = 0, visibility = SHADER_VISIBILITY_ALL"), \
    "CBV(b2, space = 0, visibility = SHADER_VISIBILITY_ALL"), \
    "DescriptorTable(SRV(t0, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_PIXEL), " \
    "DescriptorTable(SRV(t1, space = 1, numDescriptors = 1000), visibility = SHADER_VISIBILITY_ALL), " \
    "DescriptorTable(SRV(t2, space = 2, numDescriptors = 1000), visibility = SHADER_VISIBILITY_PIXEL), " \
	"DescriptorTable(SRV(t3, space = 3, numDescriptors = 1000), visibility = SHADER_VISIBILITY_PIXEL), " \
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
	int sceneMeshAccessorsIndex;
	int sceneMeshBufferViewsIndex;
	int sceneMaterialBufferIndex;
	int envBrdfTextureIndex;
	LightProbeData sceneLightProbe;
	int sceneBvhIndex;
};

struct ViewCbLayout
{
	float4x4 viewTransform;
	float4x4 projectionTransform;
	float3 eyePos;
	float exposure;
};

struct PrimitiveCbLayout
{
	float4x4 localToWorld;
	int indexAccessor;
	int positionAccessor;
	int uvAccessor;
	int normalAccessor;
	int tangentAccessor;
	int materialIndex;
};

SamplerState g_trilinearSampler : register(s1, space1);
ConstantBuffer<PrimitiveCbLayout> g_primitiveConstants : register(b0);
ConstantBuffer<ViewCbLayout> g_viewConstants : register(b1);
ConstantBuffer<FrameCbLayout> g_frameConstants : register(b2);

struct vs_to_ps
{
	float4 pos : SV_POSITION;
	float2 uv : INTERPOLATED_UV_0;
	float4 normal : INTERPOLATED_WORLD_NORMAL;
	float4 tangent : INTERPOLATED_WORLD_TANGENT;
	float4 bitangent : INTERPOLATED_WORLD_BITANGENT;
	float4 worldPos : INTERPOLATED_WORLD_POS;
};

vs_to_ps vs_main(uint index : SV_VertexID)
{
	vs_to_ps o;

	float4x4 localToWorld = mul(g_primitiveConstants.localToWorld, g_frameConstants.sceneRotation);
	float4x4 viewProjTransform = mul(g_viewConstants.viewTransform, g_viewConstants.projectionTransform);

	// index
	uint vertIndex = MeshMaterial::GetUint(index, g_primitiveConstants.indexAccessor, g_frameConstants.sceneMeshAccessorsIndex, g_frameConstants.sceneMeshBufferViewsIndex);

	// position
	float3 position = MeshMaterial::GetFloat3(vertIndex, g_primitiveConstants.positionAccessor, g_frameConstants.sceneMeshAccessorsIndex, g_frameConstants.sceneMeshBufferViewsIndex);
	float4 worldPos = mul(float4(position, 1.f), localToWorld);
	o.worldPos = worldPos;
	o.pos = mul(worldPos, viewProjTransform);

	// uv
	o.uv = MeshMaterial::GetFloat2(vertIndex, g_primitiveConstants.uvAccessor, g_frameConstants.sceneMeshAccessorsIndex, g_frameConstants.sceneMeshBufferViewsIndex);

	// normal
	float3 normal = MeshMaterial::GetFloat3(vertIndex, g_primitiveConstants.normalAccessor, g_frameConstants.sceneMeshAccessorsIndex, g_frameConstants.sceneMeshBufferViewsIndex);
	o.normal = mul(float4(normal, 0.f), localToWorld);

	// tangent
	float4 packedTangent = MeshMaterial::GetFloat4(vertIndex, g_primitiveConstants.tangentAccessor, g_frameConstants.sceneMeshAccessorsIndex, g_frameConstants.sceneMeshBufferViewsIndex);
	float3 tangent = packedTangent.xyz;
	o.tangent = mul(float4(tangent, 0.f), localToWorld);

	// bitangent
	float3 bitangent = cross(normal, tangent) * packedTangent.w;
	o.bitangent = mul(float4(bitangent, 0.f), localToWorld);

	return o;
}

float4 ps_main(vs_to_ps input) : SV_Target
{
	FMaterial mat = MeshMaterial::GetMaterial(g_primitiveConstants.materialIndex, g_frameConstants.sceneMaterialBufferIndex);

	// Alpha clip
	float alpha = mat.m_baseColorTextureIndex != -1 ? g_bindless2DTextures[mat.m_baseColorTextureIndex].Sample(g_bindlessSamplers[mat.m_baseColorSamplerIndex], input.uv).a : 1.f;
	clip(alpha - 0.5);

	// Tangent space transform
	float3 T = normalize(input.tangent.xyz);
	float3 B = normalize(input.bitangent.xyz);
	float3 N = normalize(input.normal.xyz);
	float3x3 TBN = float3x3(T, B, N);

	float3 emissive = mat.m_emissiveTextureIndex != -1 ?
		mat.m_emissiveFactor * g_bindless2DTextures[mat.m_emissiveTextureIndex].Sample(g_bindlessSamplers[mat.m_emissiveSamplerIndex], input.uv).rgb :
		mat.m_emissiveFactor;

#if LIGHTING_ONLY
	float3 baseColor = 0.5.xxx;
#else
	float3 baseColor = mat.m_baseColorTextureIndex != -1 ?
		mat.m_baseColorFactor * g_bindless2DTextures[mat.m_baseColorTextureIndex].Sample(g_bindlessSamplers[mat.m_baseColorSamplerIndex], input.uv).rgb :
		mat.m_baseColorFactor;
#endif

	if (mat.m_normalTextureIndex != -1)
	{
		float2 normalXY = g_bindless2DTextures[mat.m_normalTextureIndex].Sample(g_bindlessSamplers[mat.m_normalSamplerIndex], input.uv).rg;
		float normalZ = sqrt(1.f - dot(normalXY, normalXY));
		N = normalize(mul(float3(normalXY, normalZ), TBN));
	}

	// Note that GLTF specifies metalness in blue channel and roughness in green channel but we swizzle them on import and
	// use a BC5 texture. So, metalness ends up in the red channel and roughness stays on the green channel.
	float2 metallicRoughnessMap = mat.m_metallicRoughnessTextureIndex != -1 ?
		g_bindless2DTextures[mat.m_metallicRoughnessTextureIndex].Sample(g_bindlessSamplers[mat.m_metallicRoughnessSamplerIndex], input.uv).rg :
		1.f.xx;

	float ao = mat.m_aoTextureIndex != -1 ?
		g_bindless2DTextures[mat.m_aoTextureIndex].Sample(g_bindlessSamplers[mat.m_aoSamplerIndex], input.uv).r :
		1.f;

	float aoStrength = mat.m_aoStrength;
	float metallic = mat.m_metallicFactor * metallicRoughnessMap.x;
	float perceptualRoughness = mat.m_roughnessFactor * metallicRoughnessMap.y;

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
	float3 Fr = (D * F * G) / (4.f * NoV * NoL);

	// diffuse BRDF
	float3 Fd = albedo * Fd_Lambert();

	RayDesc ray;
	ray.Origin = input.worldPos.xyz;
	ray.Direction = L;
	ray.TMin = 0.1f;
	ray.TMax = 1000.f;

	RaytracingAccelerationStructure sceneBvh = g_accelerationStructures[g_frameConstants.sceneBvhIndex];
	RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
	q.TraceRayInline(sceneBvh, RAY_FLAG_NONE, 0xff, ray);
	q.Proceed();
	float shadow = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.f : 1.f;

	// Apply direct lighting
	const float lightIntensity = 100000.f;
	float illuminance = lightIntensity * NoL;
	float3 luminance = 0.f;
	
#if DIRECT_LIGHTING
	luminance += (Fr + (1.f - F) * Fd) * illuminance * shadow;
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

		float3 shDiffuse = (1.f - F) * Fd * ShIrradiance(N, shRadiance);
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