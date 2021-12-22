#ifndef __RAYTRACING_COMMON_HLSLI_
#define __RAYTRACING_COMMON_HLSLI_

#include "sampling.hlsli"

// Generate a ray in world space for a camera pixel corresponding to an index from the dispatched 2D grid.
RayDesc GenerateCameraRay(uint2 index, float3 cameraPos, float4x4 projectionToWorld)
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
    float randomNoise, 
    float3 hitPosition, 
    float3 normal, 
    float3 reflectance, 
    float metalness, 
    float3 albedo,
    float3x3 tangentToWorld,
    out float3 outAttenuation)
{
    RayDesc defaultRay;
    defaultRay.Origin = hitPosition;
    defaultRay.Direction = normalize(reflect(WorldRayDirection(), normal));
    defaultRay.TMin = 0.001;
    defaultRay.TMax = 10000.0;
    outAttenuation = 0.f;

    if (metalness > randomNoise) // Metal
    {
        if (length(reflectance) > randomNoise)
        {
            RayDesc ray;
            ray.Origin = hitPosition;
            ray.Direction = normalize(reflect(WorldRayDirection(), normal));
            ray.TMin = 0.001;
            ray.TMax = 10000.0;
            outAttenuation = reflectance;
            return ray;
        }
    }
    else // Dielectric
    {
        if (length(reflectance) > randomNoise)
        {
            RayDesc ray;
            ray.Origin = hitPosition;
            ray.Direction = normalize(reflect(WorldRayDirection(), normal));
            ray.TMin = 0.001;
            ray.TMax = 10000.0;
            outAttenuation = reflectance;
            return ray;
        }
        else
        {
            float3 rayDir = SampleDirectionHemisphere(randomNoise, randomNoise);
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