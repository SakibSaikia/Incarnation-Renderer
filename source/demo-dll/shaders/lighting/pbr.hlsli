#ifndef __PBR_HLSLI_
#define __PBR_HLSLI_

#include "common/bxdf-sampling.hlsli"

// When using correlated Smith, make sure that NoL is not clamped to (0.0, 1.0) as otherwise
// this function can generate an inf response when NoL is 0.
float G_SmithGGXCorrelated(float NoV, float NoL, float roughness) 
{
    float a2 = roughness * roughness;
    float GGXV = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float GGXL = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(GGXV + GGXL, 0.00001);
}

float3 F_Schlick(float u, float3 f0, float f90)
{
    return f0 + (f90.xxx - f0) * pow(1.0 - u, 5.0);
}

// u = LoH == VoH by property of half-vector
float3 F_Schlick(float u, float3 f0) 
{
    float f = pow(1.0 - u, 5.0);
    return f + f0 * (1.0 - f);
}

// u = LoH == VoH by property of half-vector
float F_Schlick(float u, float f0, float f90)
{
    return f0 + (f90 - f0) * pow(1.0 - u, 5.0);
}

// Geometry function based on GGX and Schlick-Bechmann approximation
// u == NoV for masking and u == NoL for shadowing
// k  is a remapping of α based on whether we're using the geometry function for either direct lighting or IBL lighting
float G_SchlickGGX(float u, float k)
{
    return u / (u * (1.f - k) + k);
}

// Smith Geometry function for direct lighting
float G_Smith_Direct(float NoV, float NoL, float roughness)
{
    float a2 = roughness * roughness;
    float k = (a2 + 1) * (a2 + 1) / 8.f;
    float masking = G_SchlickGGX(NoV, k);
    float shadowing = G_SchlickGGX(NoL, k);
    return masking * shadowing;
}

// Smith Geometry function for IBL
float G_Smith_IBL(float NoV, float NoL, float roughness)
{
    float a2 = roughness * roughness;
    float k = 0.5f * a2 * a2;
    float masking = G_SchlickGGX(NoV, k);
    float shadowing = G_SchlickGGX(NoL, k);
    return masking * shadowing;
}

// Lambert Diffuse BRDF
float Fd_Lambert() 
{
    return k_InvPi;
}

// Burley Diffuse BRDF
float Fd_Burley(float NoV, float NoL, float LoH, float roughness) 
{
    float f90 = 0.5 + 2.0 * roughness * LoH * LoH;
    float lightScatter = F_Schlick(NoL, 1.0, f90);
    float viewScatter = F_Schlick(NoV, 1.0, f90);
    return lightScatter * viewScatter * k_InvPi;
}

// Computes the exposure normalization factor from the camera's EV100
float Exposure(int ev100) 
{
    return 1.0 / (pow(2.0, ev100) * 1.2);
}

// https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
float3 ACESFilm(float3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 Reinhard(float3 x)
{
    return x / (x + 1.f);
}

#endif