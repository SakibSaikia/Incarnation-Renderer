#ifndef __RAYTRACING_COMMON_HLSLI_
#define __RAYTRACING_COMMON_HLSLI_

#include "lighting/common.hlsli"

static const float k_rayOffset = 0.001f;

static float2 SamplePoint(uint pixelIdx, uint sampleIdx, inout uint setIdx, uint sqrtSampleCount)
{
    const uint numPixels = DispatchRaysDimensions().x * DispatchRaysDimensions().y;
    const uint permutationIdx = setIdx * numPixels + pixelIdx;
    setIdx += 1;
    return CorrelatedMultiJitteredSampling(sampleIdx, sqrtSampleCount, sqrtSampleCount, permutationIdx);
}

static float SampleRand(uint pixelIdx, uint sampleIdx, inout uint setIdx)
{
    const uint numPixels = DispatchRaysDimensions().x * DispatchRaysDimensions().y;
    const uint permutationIdx = setIdx * numPixels + pixelIdx;
    setIdx += 1;
    return CMJ_RandFloat(sampleIdx, permutationIdx);
}

// Generate a ray in world space for a camera pixel corresponding to an index from the dispatched 2D grid.
RayDesc GenerateCameraRay(float2 index, float4x4 cameraMatrix, float4x4 projectionToWorld, float aperture, float focalLength, float2 randomSample)
{
    float2 xy = index + 0.5f;
    float2 screenPos = xy / DispatchRaysDimensions().xy * 2.0 - 1.0;
    screenPos.y = -screenPos.y;

    // Use primary ray to determine focal point
    float3 cameraPos = cameraMatrix[3].xyz;
    float4 world = mul(float4(screenPos, 0.0001f, 1.f), projectionToWorld);
    world.xyz /= world.w;
    float3 primaryRayDir = normalize(world.xyz - cameraPos);
    float3 focalPoint = cameraPos + focalLength * primaryRayDir;

    // Generate secondary ray used for tracing by sampling a disk around the camera origin based on the aperture
    float3 cameraRight = cameraMatrix[0].xyz;
    float3 cameraUp = cameraMatrix[1].xyz;
    float2 offset = ConcentricSampleDisk(randomSample);
    float3 rayOrigin = cameraPos + aperture * offset.x * cameraRight + aperture * offset.y * cameraUp;

    // Set TMin to a non-zero small value to avoid aliasing issues due to floating - point errors.
    // TMin should be kept small to prevent missing geometry at close contact areas.
    RayDesc ray;
    ray.Origin = rayOrigin;
    ray.Direction = normalize(focalPoint - rayOrigin);
    ray.TMin = 0.001;
    ray.TMax = 10000.0;

    return ray;
}

RayDesc GenerateIndirectRadianceRay(
    float3 hitPosition,
    float3 N,
    float3 V,
    FMaterialProperties matInfo,
    float3x3 tangentToWorld,
    uint pixelIndex,
    uint sampleIndex,
    uint sampleSetIndex,
    uint sqrtSampleCount,
    out float3 outAttenuation)
{
    // Sample from both a cosine-weighted distribution as well as the microfacet distribution
    // based on whether u.x is is less than or greater than 0.5 and then remap u.x to cover
    // the entire [0,1] range. The pdf for this sampling strategy is the avg of the 2 pdf's used.
    // See: https://www.pbr-book.org/3ed-2018/Light_Transport_I_Surface_Reflection/Sampling_Reflection_Functions 
    float2 u = SamplePoint(pixelIndex, sampleIndex, sampleSetIndex, sqrtSampleCount);
    float3 L, H;
    float t = 0.5f;
    if (u.x < t)
    {
        // Cosine sample hemisphere
        u.x = 2.f * u.x;
        L = CosineSampleHemisphere(u);
        L = normalize(mul(L, tangentToWorld));
        H = normalize(L + V);
    }
    else
    {
        // GGX sample half vectors around the normal based on roughness.
        // Reflect the ray direction (V) about H to get the new direction for indirect lighting L
        u.x = 2.f * (u.x - 0.5f);
        H = SampleGGX(u, matInfo.roughness);
        H = normalize(mul(H, tangentToWorld));
        L = normalize(reflect(-V, H));
    }

    float NoL = max(dot(N, L), 0.001);
    float NoV = max(dot(N, V), 0.001);
    float NoH = max(dot(N, H), 0.001);
    float VoH = max(dot(V, H), 0.001);

    // Avg. the two pdf's. Note that the GGX distribution gives the distribution of normals around the 
    // half vector, but the reflection integral is with respect to the incoming light vector. These
    // distributions are not the same and we must transform the half vector PDF to the light direction PDF
    // by dividing by 4 * VoH. See: https://www.pbr-book.org/3ed-2018/Light_Transport_I_Surface_Reflection/Sampling_Reflection_Functions
    float pdf = t * CosineHemispherePdf(NoL) + (1.f - t) * GGXPdf(NoH, matInfo.roughness) / (4.f * VoH);

    // Eavluate BRDF
    float3 F0 = matInfo.metallic * matInfo.basecolor + (1.f - matInfo.metallic) * 0.04;
    float3 albedo = (1.f - matInfo.metallic) * matInfo.basecolor;
    float D = GGX(NoH, matInfo.roughness);
    float3 F = F_Schlick(VoH, F0);
    float G = G_Smith_Direct(NoV, NoL, matInfo.roughness);

    // Diffuse & Specular BRDF
    float3 Fd = albedo * Fd_Lambert();
    float3 Fr = (D * F * G) / max(4.f * NoV * NoL, 0.001);
    float3 brdf = (Fr + (1.f - F) * Fd);

    // Importance-sampled ray
    RayDesc ray;
    ray.Origin = hitPosition;
    ray.Direction = L;
    ray.TMin = k_rayOffset;
    ray.TMax = 10000.0;
    outAttenuation = brdf * NoL / pdf;
    return ray;
}

// Retrieve attribute at a hit position interpolated from the hit's barycentrics.
float2 HitAttribute(float2 vertexAttribute[3], float2 barycentrics)
{
    return vertexAttribute[0] +
        barycentrics.x * (vertexAttribute[1] - vertexAttribute[0]) +
        barycentrics.y * (vertexAttribute[2] - vertexAttribute[0]);
}

float3 HitAttribute(float3 vertexAttribute[3], float2 barycentrics)
{
    return vertexAttribute[0] +
        barycentrics.x * (vertexAttribute[1] - vertexAttribute[0]) +
        barycentrics.y * (vertexAttribute[2] - vertexAttribute[0]);
}

float4 HitAttribute(float4 vertexAttribute[3], float2 barycentrics)
{
    return vertexAttribute[0] +
        barycentrics.x * (vertexAttribute[1] - vertexAttribute[0]) +
        barycentrics.y * (vertexAttribute[2] - vertexAttribute[0]);
}

#endif