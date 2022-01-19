#ifndef __RAYTRACING_COMMON_HLSLI_
#define __RAYTRACING_COMMON_HLSLI_

#include "sampling.hlsli"

static float2 SamplePoint(uint pixelIdx, uint sampleIdx, inout uint setIdx)
{
    const uint numPixels = DispatchRaysDimensions().x * DispatchRaysDimensions().y;
    const uint permutationIdx = setIdx * numPixels + pixelIdx;
    setIdx += 1;
    return CorrelatedMultiJitteredSampling(sampleIdx, 1024, 1024, permutationIdx);
}

static float SampleRand(uint pixelIdx, uint sampleIdx, inout uint setIdx)
{
    const uint numPixels = DispatchRaysDimensions().x * DispatchRaysDimensions().y;
    const uint permutationIdx = setIdx * numPixels + pixelIdx;
    setIdx += 1;
    return CMJ_RandFloat(sampleIdx, permutationIdx);
}

// Generate a ray in world space for a camera pixel corresponding to an index from the dispatched 2D grid.
RayDesc GenerateCameraRay(float2 index, float3 cameraPos, float4x4 projectionToWorld)
{
    float2 xy = index + 0.5f;
    float2 screenPos = xy / DispatchRaysDimensions().xy * 2.0 - 1.0;
    screenPos.y = -screenPos.y;

    float4 world = mul(float4(screenPos, 0.0001f, 1.f), projectionToWorld);
    world.xyz /= world.w;

    // Set TMin to a non-zero small value to avoid aliasing issues due to floating - point errors.
    // TMin should be kept small to prevent missing geometry at close contact areas.
    RayDesc ray;
    ray.Origin = cameraPos;
    ray.Direction = normalize(world.xyz - cameraPos);
    ray.TMin = 0.001;
    ray.TMax = 10000.0;

    return ray;
}

RayDesc GenerateIndirectRadianceRay(
    float3 hitPosition,
    float3 normal,
    float3 reflectance,
    float metalness,
    float roughness,
    float3 albedo,
    float3x3 tangentToWorld,
    uint pixelIndex,
    uint sampleIndex,
    uint sampleSetIndex,
    out float3 outAttenuation)
{
    RayDesc defaultRay = (RayDesc)0;
    outAttenuation = 0.f;

    if (metalness > 0.5) // Metal
    {
        //if (length(reflectance) > randomNoise)
        {
            float2 ggxSample = SamplePoint(pixelIndex, sampleIndex, sampleSetIndex);
            float3 reflectedRayDir = reflect(WorldRayDirection(), normal);
            float3 rayDir = ImportanceSampleGGX(ggxSample, roughness, reflectedRayDir);

            RayDesc ray;
            ray.Origin = hitPosition;
            ray.Direction = normalize(rayDir);
            ray.TMin = 0.001;
            ray.TMax = 10000.0;
            outAttenuation = reflectance;
            return ray;
        }
    }
    else // Dielectric
    {
        float reflectionProbability = SampleRand(pixelIndex, sampleIndex, sampleSetIndex);

        if (length(reflectance) > reflectionProbability)
        {
            float2 ggxSample = SamplePoint(pixelIndex, sampleIndex, sampleSetIndex);
            float3 reflectedRayDir = reflect(WorldRayDirection(), normal);
            float3 rayDir = ImportanceSampleGGX(ggxSample, roughness, reflectedRayDir);

            RayDesc ray;
            ray.Origin = hitPosition;
            ray.Direction = normalize(rayDir);
            ray.TMin = 0.001;
            ray.TMax = 10000.0;
            outAttenuation = reflectance;
            return ray;
        }
        else
        {
            float2 hemisphereSample = SamplePoint(pixelIndex, sampleIndex, sampleSetIndex);
            float3 rayDir = SampleDirectionHemisphere(hemisphereSample);
            rayDir = normalize(mul(rayDir, tangentToWorld));

            RayDesc ray;
            ray.Origin = hitPosition;
            ray.Direction = rayDir;
            ray.TMin = 0.001;
            ray.TMax = 10000.0;
            outAttenuation = albedo;
            return ray;
        }
    }

    return defaultRay;
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