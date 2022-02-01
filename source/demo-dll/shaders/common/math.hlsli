#ifndef __MATH_HLSLI_
#define __MATH_HLSLI_

// From https://www.pbr-book.org/3ed-2018/Utilities/Main_Include_File
static const float k_Pi = 3.14159265358979323846;
static const float k_InvPi = 0.31830988618379067154;
static const float k_Inv2Pi = 0.15915494309189533577;
static const float k_Inv4Pi = 0.07957747154594766788;
static const float k_PiOver2 = 1.57079632679489661923;
static const float k_PiOver4 = 0.78539816339744830961;
static const float k_Sqrt2 = 1.41421356237309504880;

float Radians(float deg) 
{
    return (k_Pi / 180) * deg;
}

float Degrees(float rad) 
{
    return (180 / k_Pi) * rad;
}

// Conversion from spherical coordinates to cartesian coordinates
float3 SphericalDirection(float sinTheta, float cosTheta, float phi)
{
    return float3(
        sinTheta * cos(phi),
        sinTheta * sin(phi),
        cosTheta);
}

// Returns tangent basis around world normal direction N
float3x3 TangentToWorld(float3 N)
{
    float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T = normalize(cross(up, N));
    float3 B = cross(N, T);

    return float3x3(T, B, N);
}

#endif // __MATH_HLSLI_