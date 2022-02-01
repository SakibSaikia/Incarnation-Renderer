#ifndef __BXDF_SAMPLING_HLSLI_
#define __BXDF_SAMPLING_HLSLI_

#include "common/uniform-sampling.hlsli"

// ------------------
// References : 
// * Real Shading in Unreal Engine 4 coursenotes by Brian Karis, Epic Games
// * https://www.pbr-book.org/3ed-2018/Light_Transport_I_Surface_Reflection/Sampling_Reflection_Functions
// ------------------

float3 SampleGGX(float2 u, float Roughness)
{
    float a = Roughness * Roughness;

    // Compute GGX distribution sample
    float Phi = 2 * k_Pi * u.x;
    float CosTheta = sqrt((1.f - u.y) / (1.f + (a * a - 1.f) * u.y));
    float SinTheta = sqrt(1.f - CosTheta * CosTheta);

    // Map sampled GGX angles to tangent space normal direction
    float3 H = SphericalDirection(SinTheta, CosTheta, Phi);

    return H;
}

float3 SampleBeckmann(float2 u, float roughness)
{
    // Isotropic distribution
    float a = roughness * roughness;

    // Compute tan2Theta and phi for Beckmann distribution sample
    float logSample = log(1.f - u.x);
    if (isinf(logSample))
    {
        logSample = 0.f;
    }
    float tan2Theta = -a * logSample;
    float phi = u.y * 2.f * k_Pi;

    // Map sampled Beckmann angles to tangent space normal direction
    float cosTheta = 1.f / sqrt(1.f + tan2Theta);
    float sinTheta = sqrt(max(0.f, 1.f - cosTheta * cosTheta));
    float3 H = SphericalDirection(sinTheta, cosTheta, phi);

    return H;
}

#endif // __BXDF_SAMPLING_HLSLI_