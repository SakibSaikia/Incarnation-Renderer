#ifndef __LIGHTING_COMMON_HLSLI_
#define __LIGHTING_COMMON_HLSLI_

#include "pbr.hlsli"
#include "gpu-shared-types.h"
#include "material/common.hlsli"

float3 GetDirectRadiance(FLight light, float4x4 lightTransform, float3 worldPos, float3 basecolor, float metallic, float roughness, float3 N, float3 V, RaytracingAccelerationStructure sceneBvh)
{
	float3 L;
	float3 radianceIn = 0.f;

	// https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_lights_punctual/README.md
	if (light.m_type == Light::Directional)
	{
		float3x3 lightRotation = float3x3(lightTransform[0].xyz, lightTransform[1].xyz, lightTransform[2].xyz);
		L = normalize(mul(float3(0, 0, -1), lightRotation));
		radianceIn = 100 * light.m_intensity * light.m_color;
	}
	else if (light.m_type == Light::Point)
	{
		float3 lightPosition = lightTransform[3].xyz;
		float3 lightVec = lightPosition - worldPos;
		float distanceSquared = dot(lightVec, lightVec);
		float distance = sqrt(distanceSquared);
		L = lightVec / distance;

		// See attenuation calculation in https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_lights_punctual
		float radialAttenuation;
		if (light.m_range > 0.f)
		{
			radialAttenuation = max(min(1.f, 1.f - pow(distance / light.m_range, 4)), 0.f) / distanceSquared;
		}
		else
		{
			radialAttenuation = 1.f / max(distanceSquared, 0.0001);
		}

		// Gltf specifies point light intensity as luminious indensity in candela (lm/sr)
		// Note that if intensity is specified as luminous power, we need an additional divide by 4Pi as in 
		// https://seblagarde.files.wordpress.com/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf
		radianceIn = 10000 * light.m_intensity * light.m_color * radialAttenuation;
	}
	else if (light.m_type == Light::Spot)
	{
		float3x3 lightRotation = float3x3(lightTransform[0].xyz, lightTransform[1].xyz, lightTransform[2].xyz);
		float3 spotPosition = lightTransform[3].xyz;
		float3 spotDirection = normalize(mul(float3(0, 0, -1), lightRotation));
		float3 lightVec = spotPosition - worldPos;
		float distanceSquared = dot(lightVec, lightVec);
		float distance = sqrt(distanceSquared);
		L = lightVec / distance;

		// Angular atttenuation
		float angularAttenuation = 0.f;
		float cd = dot(spotDirection, L);
		float cosineOuterAngle = cos(light.m_spotAngles.y);
		if (cd > cosineOuterAngle)
		{
			float cosineInnerAngle = cos(light.m_spotAngles.x);
			float lightAngleScale = 1.f / max(0.001f, cosineInnerAngle - cosineOuterAngle);
			float lightAngleOffset = -cosineOuterAngle * lightAngleScale;
			angularAttenuation = saturate(cd * lightAngleScale + lightAngleOffset);
			angularAttenuation *= angularAttenuation;
		}

		// Radial attenuation
		float radialAttenuation = 0.f;
		if (angularAttenuation > 0.f)
		{
			if (light.m_range > 0.f)
			{
				float radialAttenuation = max(min(1.f, 1.f - pow(distance / light.m_range, 4)), 0.f) / distanceSquared;
			}
			else
			{
				radialAttenuation = 1.f / max(distanceSquared, 0.0001);
			}
		}

		radianceIn = 10000 * light.m_intensity * light.m_color * angularAttenuation * radialAttenuation;
	}

	float3 radianceOut = 0;
	float NoL = saturate(dot(N, L));
	if (NoL > 0.f && any(radianceIn > 0.f))
	{
		// Shadow ray
		float lightVisibility = 1.f;

#if !PATH_TRACING
		// For raster, trace shadow ray for directional light only
		if (light.m_type == Light::Directional)
#endif
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
				// If Proceed() returns false, it means that traversal is complete. Check status and update light visibility.
				lightVisibility = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.f : 1.f;
			}
			else
			{
				// This means that further evaluation is needed. For now, set light visibility to 0.f for non-opaque
				// triangles. In future, evaluate alpha map for non-opaque geo before setting visibility.
				lightVisibility = q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE ? 0.f : 1.f;
			}
		}

		if (lightVisibility > 0.f)
		{
			float NoV = saturate(dot(N, V));

			float3 F0 = metallic * basecolor + (1.f - metallic) * 0.04;
			float3 albedo = (1.f - metallic) * basecolor;

			float3 H = normalize(L + V);
			float NoH = saturate(dot(N, H));
			float VoH = saturate(dot(V, H));

			float D = GGX(NoH, roughness);
			float3 F = F_Schlick(VoH, F0);
			float G = G_SmithGGXCorrelated(NoV, NoL, roughness);

			// Diffuse & Specular BRDF
			float3 Fd = albedo * Fd_Lambert();
			float3 Fr = (D * F * G) / max(4.f * NoV * NoL, 0.001);

			float3 irradiance = radianceIn * NoL;
			radianceOut = (Fr + (1.f - F) * Fd) * irradiance * lightVisibility;
		}
	}

	return radianceOut;
}

float3 GetSkyRadiance(float3 radianceIn, float3 L, float3 worldPos, FMaterialProperties matInfo, float3 N, float3 V, RaytracingAccelerationStructure sceneBvh)
{
	float3 radianceOut = 0;
	float NoL = saturate(dot(N, L));

	if (NoL > 0.f && any(radianceIn > 0.f))
	{
		// Shadow ray
		float lightVisibility = 1.f;
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
				// If Proceed() returns false, it means that traversal is complete. Check status and update light visibility.
				lightVisibility = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.f : 1.f;
			}
			else
			{
				// This means that further evaluation is needed. For now, set light visibility to 0.f for non-opaque
				// triangles. In future, evaluate alpha map for non-opaque geo before setting visibility.
				lightVisibility = q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE ? 0.f : 1.f;
			}
		}

		if (lightVisibility > 0.f)
		{
			float NoV = dot(N, V);

			float3 F0 = matInfo.metallic * matInfo.basecolor + (1.f - matInfo.metallic) * 0.04;
			float3 albedo = (1.f - matInfo.metallic) * matInfo.basecolor;

			float3 H = normalize(L + V);
			float NoH = max(0.0001f, dot(N, H));
			float VoH = max(0.0001f, dot(V, H));

			float D = GGX(NoH, matInfo.roughness);
			float3 F = F_Schlick(VoH, F0);
			float G = G_Smith_Direct(NoV, NoL, matInfo.roughness);

			// Diffuse & Specular BRDF
			float3 Fd = albedo * Fd_Lambert();
			float3 Fr = (D * F * G) / max(4.f * NoV * NoL, 0.001);

			float3 irradiance = radianceIn * NoL;
			radianceOut = (Fr + (1.f - F) * Fd) * irradiance * lightVisibility;
		}
	}

	return radianceOut / CosineHemispherePdf(L.z);
}

#endif