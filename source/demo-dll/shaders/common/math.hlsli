#ifndef __MATH_HLSLI_
#define __MATH_HLSLI_

/*              

    Conventions:

    * Left-handed coordinate system
    * World Coordinates: X = right, Y = up, Z = far
    * Tangent Space: X = Tangent, Y = Bitangent, Z = Normal
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

enum CoordinateSpace
{
    World,
    Tangent
};

// For a lat-long texture, this converts a given uv to polar coordinates which represent a direction about the sphere.
// 
// A latlong image maps a direction's azimuth to the horizontal coordinate and its elevation to the vertical coordinate of the image. 
// The top edge of the image corresponds to straight up, and the bottom edge corresponds to straight down. The center of the image corresponds to the Z (forward) axis.
// For example, an elevation of 0 degrees points straight up (World Y Axis), and an azimuth of 0 degrees points straight forward (World Z Axis).
// 
// See: https://vgl.ict.usc.edu/Data/HighResProbes/
float2 LatlongUV2Polar(float2 uv)
{
    float theta = k_Pi * uv.y;
    float phi = k_Pi * (uv.x * 2.f - 1.f);

    return float2(theta, phi);
}

// Conversion from polar angles to rectangular coordinates. 
// World space coordinates are swizzled to make y-up
float3 Polar2Cartesian(float sint, float cost, float phi, CoordinateSpace type)
{
    float3 p;
    p.x = sint * sin(phi);
    p.y = sint * cos(phi);
    p.z = cost;
    return type == CoordinateSpace::World ? p.xzy : p;
}

// Conversion from polar angles to rectangular coordinates. 
// World space coordinates are swizzled to make y-up
float3 Polar2Cartesian(float theta, float phi, CoordinateSpace type)
{
    float sint = sin(theta);
    float cost = cos(theta);
    float sinp = sin(phi);
    float cosp = cos(phi);

    float3 p;
    p.x = sint * sinp;
    p.y = sint * cosp;
    p.z = cost;
    return type == CoordinateSpace::World ? p.xzy : p;
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