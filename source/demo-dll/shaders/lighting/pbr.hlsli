// Adapted from google.github.io/filament/Filament.html

#define PI 3.14159265f

// When using correlated Smith, make sure that NoL is not clamped to (0.0, 1.0) as otherwise
// this function can generate an inf response when NoL is 0.
float G_SmithGGXCorrelated(float NoV, float NoL, float roughness) 
{
    float a2 = roughness * roughness;
    float GGXV = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float GGXL = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / (GGXV + GGXL);
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
    return 1.0 / PI;
}

// Burley Diffuse BRDF
float Fd_Burley(float NoV, float NoL, float LoH, float roughness) 
{
    float f90 = 0.5 + 2.0 * roughness * LoH * LoH;
    float lightScatter = F_Schlick(NoL, 1.0, f90);
    float viewScatter = F_Schlick(NoV, 1.0, f90);
    return lightScatter * viewScatter * (1.0 / PI);
}

// Hammersley Low Discrepancy Sequence used for biased Monte Carlo Estimation
// (Quasi-Monte Carlo Integration)
// https://google.github.io/filament/Filament.html#annex/hammersleysequence
float2 Hammersley(uint i, float numSamples)
{
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float2(float(i) / numSamples, float(bits) / exp2(32));
}

// Real Shading in Unreal Engine 4 coursenotes by Brian Karis, Epic Games
float3 ImportanceSampleGGX(float2 Xi, float Roughness, float3 N)
{
    float a = Roughness * Roughness;

    // Construct spherical coordinates from input Low Descrepancy Sequence Xi
    float Phi = 2 * PI * Xi.x;
    float CosTheta = sqrt((1.f - Xi.y) / (1.f + (a * a - 1.f) * Xi.y));
    float SinTheta = sqrt(1.f - CosTheta * CosTheta);

    // Convert from spherical coordinates to cartesian coordinates
    float3 H;
    H.x = SinTheta * cos(Phi);
    H.y = SinTheta * sin(Phi);
    H.z = CosTheta;

    // Convert from tangent space to world space sample vector
    float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);

    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

// https://learnopengl.com/PBR/Theory
float D_GGX(float NoH, float Roughness)
{
    float a = Roughness * Roughness;
    float a2 = a * a;

    float nom = a2;
    float denom = (NoH * NoH * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return nom / denom;
}

// Computes the exposure normalization factor from the camera's EV100
float exposure(int ev100) 
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
    float a = 2.f;
    return x / (x + a.xxx);
}