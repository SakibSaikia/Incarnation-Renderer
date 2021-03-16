// Adapted from https://google.github.io/filament/Filament.html

static const float PI = 3.14159265f;

float D_GGX(float NoH, float roughness) 
{
    float a = NoH * roughness;
    float k = roughness / (1.0 - NoH * NoH + a * a);
    return k * k * (1.0 / PI);
}

float V_SmithGGXCorrelated(float NoV, float NoL, float roughness) 
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

float3 F_Schlick(float u, float3 f0) 
{
    float f = pow(1.0 - u, 5.0);
    return f + f0 * (1.0 - f);
}

float F_Schlick(float u, float f0, float f90) 
{
    return f0 + (f90 - f0) * pow(1.0 - u, 5.0);
}

float Fd_Lambert() 
{
    return 1.0 / PI;
}

float Fd_Burley(float NoV, float NoL, float LoH, float roughness) 
{
    float f90 = 0.5 + 2.0 * roughness * LoH * LoH;
    float lightScatter = F_Schlick(NoL, 1.0, f90);
    float viewScatter = F_Schlick(NoV, 1.0, f90);
    return lightScatter * viewScatter * (1.0 / PI);
}

// https://google.github.io/filament/Filament.html#annex/hammersleysequence
float2 Hammersley(uint i, float numSamples)
{
    uint bits = i;
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555) << 1) | ((bits & 0xAAAAAAAA) >> 1);
    bits = ((bits & 0x33333333) << 2) | ((bits & 0xCCCCCCCC) >> 2);
    bits = ((bits & 0x0F0F0F0F) << 4) | ((bits & 0xF0F0F0F0) >> 4);
    bits = ((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >> 8);
    return float2(i / numSamples, bits / exp2(32));
}

// Real Shading in Unreal Engine 4 coursenotes by Brian Karis, Epic Games
float3 ImportanceSampleGGX(float2 Xi, float Roughness, float3 N)
{
    float a = Roughness * Roughness;

    float Phi = 2 * PI * Xi.x;
    float CosTheta = sqrt((1 - Xi.y) / (1 + (a * a - 1) * Xi.y));
    float SinTheta = sqrt(1 - CosTheta * CosTheta);

    float3 H;
    H.x = SinTheta * cos(Phi);
    H.y = SinTheta * sin(Phi);
    H.z = CosTheta;

    float3 UpVector = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 TangentX = normalize(cross(UpVector, N));
    float3 TangentY = cross(N, TangentX);

    // Tangent to world space
    return TangentX * H.x + TangentY * H.y + N * H.z;
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