#ifndef __UNIFORM_SAMPLING_HLSLI_
#define __UNIFORM_SAMPLING_HLSLI_

#include "common/math.hlsli"

// ------------------
// References : 
// * https://www.pbr-book.org/3ed-2018/Monte_Carlo_Integration/2D_Sampling_with_Multidimensional_Transformations
// ------------------

// Distorts areas on the disk. Use concentric mapping instead!
float2 UniformSampleDisk(float2 u)
{
    float r = sqrt(u.x);
    float theta = 2.f * k_Pi * u.y;
    return r * float2(cos(theta), sin(theta));
}

// Area preserving transformation from square to disk based off the algorithm "A Low Distortion Map Between Disk and Square" by Peter Shirley and Kenneth Chiu.
float2 ConcentricSampleDisk(float2 u)
{
    // Map uniform random numbers to [-1,1]
    float2 uOffset = 2.f * u - 1.xx;

    // Handle degeneracy at the origin
    if (uOffset.x == 0.f && uOffset.y == 0.f)
    {
        return 0.xx;
    }

    // Apply concentric mapping to point
    float theta, r;
    if (abs(uOffset.x) > abs(uOffset.y))
    {
        r = uOffset.x;
        theta = k_PiOver4 * (uOffset.y / uOffset.x);
    }
    else
    {
        r = uOffset.y;
        theta = k_PiOver2 - k_PiOver4 * (uOffset.x / uOffset.y);
    }

    return r * float2(cos(theta), sin(theta));
}

float3 UniformSampleHemisphere(float2 u)
{
    float z = u.x;
    float r = sqrt(max(0.f, 1.f - z * z));
    float phi = 2.f * k_Pi * u.y;
    return float3(r * cos(phi), r * sin(phi), z);
}

float UniformHemispherePdf()
{
    return k_Inv2Pi;
}

// Use Malley's mathod to generate points on a disk and project them up to the unit hemisphere
float3 CosineSampleHemisphere(float2 u)
{
    float2 d = ConcentricSampleDisk(u);
    float z = sqrt(max(0.f, 1.f - d.x * d.x - d.y * d.y));
    return float3(d.x, d.y, z);
}

float CosineHemispherePdf(float cosTheta)
{
    return cosTheta * k_InvPi;
}

float3 UniformSampleSphere(float2 u)
{
    float z = 1.f - 2.f * u.x;
    float r = sqrt(max(0.f, 1.f - z * z));
    float phi = 2.f * k_Pi * u.y;
    return float3(r * cos(phi), r * sin(phi), z);
}

float UniformSpherePdf()
{
    return k_Inv4Pi;
}

float3 UniformSampleCone(float2 u, float cosThetaMax)
{
    float cosTheta = (1.f - u.x) + u.x * cosThetaMax;
    float sinTheta = sqrt(1.f - cosTheta * cosTheta);
    float phi = u.y * 2.f * k_Pi;
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float UniformConePdf(float cosThetaMax)
{
    return 1.f / (2.f * k_Pi * (1.f - cosThetaMax));
}

// Returns barycentric coordinates u & v. 
// w = 1.f - u - v
// The pdf is one over the triangles area.
float2 UniformSampleTriangle(float2 u)
{
    float s = sqrt(u.x);
    return float2(1.f - s, u.y * s);
}

// A pseudorandom foating point number generator.
// Maps an integer value to a pseudorandom foating point number in the [0,1) interval where the sequence is determined by a second integer.
// https://graphics.pixar.com/library/MultiJitteredSampling/paper.pdf
float RandFloat(uint i, uint p)
{
    i ^= p;
    i ^= i >> 17;
    i ^= i >> 10; i *= 0xb36534e5;
    i ^= i >> 12;
    i ^= i >> 21; i *= 0x93fc4795;
    i ^= 0xdf6e307f;
    i ^= i >> 17; i *= 1 | p >> 18;
    return i * (1.0f / 4294967808.0f);
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

uint CMJ_Permute(uint i, uint l, uint p)
{
    uint w = l - 1;
    w |= w >> 1;
    w |= w >> 2;
    w |= w >> 4;
    w |= w >> 8;
    w |= w >> 16;
    do
    {
        i ^= p; i *= 0xe170893d;
        i ^= p >> 16;
        i ^= (i & w) >> 4;
        i ^= p >> 8; i *= 0x0929eb3f;
        i ^= p >> 23;
        i ^= (i & w) >> 1; i *= 1 | p >> 27;
        i *= 0x6935fa69;
        i ^= (i & w) >> 11; i *= 0x74dcb303;
        i ^= (i & w) >> 2; i *= 0x9e501cc3;
        i ^= (i & w) >> 2; i *= 0xc860a3df;
        i &= w;
        i ^= i >> 5;
    } while (i >= l);
    return (i + p) % l;
}

float CMJ_RandFloat(uint i, uint p)
{
    i ^= p;
    i ^= i >> 17;
    i ^= i >> 10; i *= 0xb36534e5;
    i ^= i >> 12;
    i ^= i >> 21; i *= 0x93fc4795;
    i ^= 0xdf6e307f;
    i ^= i >> 17; i *= 1 | p >> 18;
    return i * (1.0f / 4294967808.0f);
}

// Returns a 2D sample from a particular pattern using correlated multi-jittered sampling [Kensler 2013]
// See: https://graphics.pixar.com/library/MultiJitteredSampling/paper.pdf
float2 CorrelatedMultiJitteredSampling(uint sampleIdx, uint numSamplesX, uint numSamplesY, uint pattern)
{
    uint N = numSamplesX * numSamplesY;
    sampleIdx = CMJ_Permute(sampleIdx, N, pattern * 0x51633e2d);
    uint sx = CMJ_Permute(sampleIdx % numSamplesX, numSamplesX, pattern * 0x68bc21eb);
    uint sy = CMJ_Permute(sampleIdx / numSamplesX, numSamplesY, pattern * 0x02e5be93);
    float jx = CMJ_RandFloat(sampleIdx, pattern * 0x967a889b);
    float jy = CMJ_RandFloat(sampleIdx, pattern * 0x368cc8b7);
    return float2((sx + (sy + jx) / numSamplesY) / numSamplesX, (sampleIdx + jy) / N);
}

#endif // __UNIFORM_SAMPLING_HLSLI_