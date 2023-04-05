#ifndef __MATH_HLSLI_
#define __MATH_HLSLI_

/*              

    Conventions:

    * Left-handed coordinate system
    * World Coordinates: X = right, Y = up, Z = far
    * Polar Coordinates: theta = elevation, phi = azimuth
*/

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

// Conversion from polar angles to rectangular coordinates. 
// World space coordinates are swizzled to make y-up
float3 Polar2Rect(float sint, float cost, float phi, bool bWorldSpace)
{
    float3 p;
    p.x = sint * cos(phi);
    p.y = sint * sin(phi);
    p.z = cost;
    return bWorldSpace ? p.xzy : p;
}

// Conversion from polar angles to rectangular coordinates. 
// World space coordinates are swizzled to make y-up
float3 Polar2Rect(float theta, float phi, bool bWorldSpace)
{
    float sint = sin(theta);
    float cost = cos(theta);
    float sinp = sin(phi);
    float cosp = cos(phi);

    float3 p;
    p.x = sint * cosp;
    p.y = sint * sinp;
    p.z = cost;
    return bWorldSpace ? p.xzy : p;
}

// For a lat-long texture, this converts a given uv to polar coordinates
// which represent a direction about the sphere
float2 UV2Polar(float2 uv)
{
    // Normalized coordinates
    float2 ndc;
    ndc.x = 2.f * uv.x - 1.f;
    ndc.y = -2.f * uv.y + 1.f;

    // Convert to polar angles
    float theta = k_Pi * 0.5f * (ndc.y - 1.f);
    float phi = k_Pi * (1.5f - ndc.x);

    return float2(theta, phi);
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