#include "pbr.hlsli"
#include "mesh-material.h"

#if RAY_TRACING
	#define TEX_SAMPLE(t,s,uv) (t.SampleLevel(s, uv, 0))
#else
	#define TEX_SAMPLE(t,s,uv) (t.Sample(s,uv))
#endif

namespace Light
{
	enum Type
	{
		Directional,
		Point,
		Spot
	};
}

struct FMaterialProperties
{
	float3 emissive;
	float3 basecolor;
	float3 normalmap;
	float metallic;
	float roughness;
	float ao;
	float aoblend;
};

struct FLight
{
	Light::Type type;
	float3 positionOrDirection;
	float intensity;
	bool shadowcasting;
};

FMaterialProperties EvaluateMaterialProperties(FMaterial mat, float2 uv)
{
	FMaterialProperties output;

#if !RAY_TRACING
	// Alpha clip
	if (mat.m_baseColorTextureIndex != -1)
	{
		Texture2D baseColorTex = ResourceDescriptorHeap[mat.m_baseColorTextureIndex];
		SamplerState baseColorSampler = SamplerDescriptorHeap[mat.m_baseColorSamplerIndex];
		float alpha = TEX_SAMPLE(baseColorTex, baseColorSampler, uv).a;
		clip(alpha - 0.5);
	}
#endif

	// Emissive
	output.emissive = mat.m_emissiveFactor;
	if (mat.m_emissiveTextureIndex != -1)
	{
		Texture2D emissiveTex = ResourceDescriptorHeap[mat.m_emissiveTextureIndex];
		SamplerState emissiveSampler = SamplerDescriptorHeap[mat.m_emissiveSamplerIndex];
		output.emissive *= TEX_SAMPLE(emissiveTex, emissiveSampler, uv).rgb;
	}

	// Base Color
	output.basecolor = mat.m_baseColorFactor;
	if (mat.m_baseColorTextureIndex != -1)
	{
		Texture2D baseColorTex = ResourceDescriptorHeap[mat.m_baseColorTextureIndex];
		SamplerState baseColorSampler = SamplerDescriptorHeap[mat.m_baseColorSamplerIndex];
		output.basecolor *= TEX_SAMPLE(baseColorTex, baseColorSampler, uv).rgb;
	}

	// Normalmap
	if (mat.m_normalTextureIndex != -1)
	{
		Texture2D normalmapTex = ResourceDescriptorHeap[mat.m_normalTextureIndex];
		SamplerState normalmapSampler = SamplerDescriptorHeap[mat.m_normalSamplerIndex];
		float2 normalXY = TEX_SAMPLE(normalmapTex, normalmapSampler, uv).rg;
		float normalZ = sqrt(1.f - dot(normalXY, normalXY));
		output.normalmap = float3(normalXY, normalZ);
	}

	// Metallic/Roughness
	// GLTF specifies metalness in blue channel and roughness in green channel but we swizzle them on import and
	// use a BC5 texture. So, metalness ends up in the red channel and roughness stays on the green channel.
	output.metallic = mat.m_metallicFactor;
	output.roughness = mat.m_roughnessFactor;
	if (mat.m_metallicRoughnessTextureIndex != -1)
	{
		Texture2D metallicRoughnessTex = ResourceDescriptorHeap[mat.m_metallicRoughnessTextureIndex];
		SamplerState metallicRoughnessSampler = SamplerDescriptorHeap[mat.m_metallicRoughnessSamplerIndex];
		float2 metallicRoughnessMap = TEX_SAMPLE(metallicRoughnessTex, metallicRoughnessSampler, uv).rg;
		output.metallic = metallicRoughnessMap.x;
		output.roughness = metallicRoughnessMap.y;
	}

	// Ambient Occlusion
	output.ao = 1.f;
	if (mat.m_aoTextureIndex != -1)
	{
		Texture2D aoTex = ResourceDescriptorHeap[mat.m_aoTextureIndex];
		SamplerState aoSampler = SamplerDescriptorHeap[mat.m_aoSamplerIndex];
		output.ao = TEX_SAMPLE(aoTex, aoSampler, uv).r;
	}

	output.aoblend = mat.m_aoStrength;
	return output;
}

float3 GetDirectRadiance(FLight light, float3 worldPos, float3 albedo, float roughness, float3 N, float D, float3 F, float NoV, float NoH, float VoH, RaytracingAccelerationStructure sceneBvh)
{
	const float3 L = light.positionOrDirection;
	float shadowMask = 1.f;
	float3 radiance = 0.f;

	if (light.shadowcasting)
	{
		RayDesc ray;
		ray.Origin = worldPos;
		ray.Direction = L;
		ray.TMin = 0.1f;
		ray.TMax = 1000.f;

		RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
		q.TraceRayInline(sceneBvh, RAY_FLAG_NONE, 0xff, ray);

		if (!q.Proceed())
		{
			// If Proceed() returns false, it means that traversal is complete. Check status and update shadow.
			shadowMask = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.f : 1.f;
		}
		else
		{
			// This means that further evaluation is needed. For now, set shadow status to true (0.f) for non-opaque
			// triangles. In future, evaluate alpha map for non-opaque geo before setting shadow status
			shadowMask = q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE ? 0.f : 1.f;
		}
	}

	if (shadowMask > 0.f)
	{
		float NoL = saturate(dot(N, L));
		float G = G_Smith_Direct(NoV, NoL, roughness);

		// Diffuse & Specular BRDF
		float3 Fd = albedo * Fd_Lambert();
		float3 Fr = (D * F * G) / (4.f * NoV * NoL);

		float irradiance = light.intensity * NoL;
		radiance += (Fr + (1.f - F) * Fd) * irradiance * shadowMask;
	}

	return radiance;
}
