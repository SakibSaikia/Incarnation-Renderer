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

// Computes the exposure normalization factor from the camera's EV100
float exposure(int ev100) 
{
    return 1.0 / (pow(2.0, ev100) * 1.2);
}