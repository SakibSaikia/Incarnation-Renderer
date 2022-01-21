#ifndef __RAYTRACING_COMMON_HLSLI_
#define __RAYTRACING_COMMON_HLSLI_

#include "lighting/common.hlsli"

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
    float2 offset = SquareToConcentricDiskMapping(randomSample.x, randomSample.y);
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
    float3 normal,
    float VoH,
    FMaterialProperties matInfo,
    float3x3 tangentToWorld,
    uint pixelIndex,
    uint sampleIndex,
    uint sampleSetIndex,
    uint sqrtSampleCount,
    out float3 outAttenuation)
{
    float3 F0 = matInfo.metallic * matInfo.basecolor + (1.f - matInfo.metallic) * 0.04;
    float3 albedo = (1.f - matInfo.metallic) * matInfo.basecolor;
    float3 F = F_Schlick(VoH, F0);

    RayDesc defaultRay = (RayDesc)0;
    outAttenuation = 0.f;

    if (matInfo.metallic > 0.5) // Metal
    {
        //if (length(reflectance) > randomNoise)
        {
            float2 ggxSample = SamplePoint(pixelIndex, sampleIndex, sampleSetIndex, sqrtSampleCount);
            float3 reflectedRayDir = reflect(WorldRayDirection(), normal);
            float3 rayDir = ImportanceSampleGGX(ggxSample, matInfo.roughness, reflectedRayDir);

            RayDesc ray;
            ray.Origin = hitPosition;
            ray.Direction = normalize(rayDir);
            ray.TMin = 0.001;
            ray.TMax = 10000.0;
            outAttenuation = F;
            return ray;
        }
    }
    else // Dielectric
    {
        float reflectionProbability = SampleRand(pixelIndex, sampleIndex, sampleSetIndex);

        if (length(F) > reflectionProbability)
        {
            float2 ggxSample = SamplePoint(pixelIndex, sampleIndex, sampleSetIndex, sqrtSampleCount);
            float3 reflectedRayDir = reflect(WorldRayDirection(), normal);
            float3 rayDir = ImportanceSampleGGX(ggxSample, matInfo.roughness, reflectedRayDir);

            RayDesc ray;
            ray.Origin = hitPosition;
            ray.Direction = normalize(rayDir);
            ray.TMin = 0.001;
            ray.TMax = 10000.0;
            outAttenuation = F;
            return ray;
        }
        else
        {
            float2 hemisphereSample = SamplePoint(pixelIndex, sampleIndex, sampleSetIndex, sqrtSampleCount);
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