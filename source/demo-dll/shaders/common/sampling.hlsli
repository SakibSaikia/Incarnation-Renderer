#ifndef __SAMPLING_HLSLI_
#define __SAMPLING_HLSLI_

#include "common/math.hlsli"

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

// Real Shading in Unreal Engine 4 coursenotes by Brian Karis, Epic Games
float3 ImportanceSampleGGX(float2 Xi, float Roughness, float3 N)
{
    float a = Roughness * Roughness;

    // Construct spherical coordinates from input Low Descrepancy Sequence Xi
    float Phi = 2 * k_Pi * Xi.x;
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

float3 UniformSampleHemisphere(float2 u)
{
    float r = sqrt(max(0.0f, 1.0f - u.x * u.x));
    float phi = 2.f * k_Pi * u.y;
    return float3(r * cos(phi), r * sin(phi), u.x);
}

float UniformHemispherePdf()
{
    return k_Inv2Pi;
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

// Maps a value inside the square [0,1]x[0,1] to a value in a disk of radius 1 using concentric squares.
// This mapping preserves area, bi continuity, and minimizes deformation.
// Based off the algorithm "A Low Distortion Map Between Disk and Square" by Peter Shirley and Kenneth Chiu.
float2 SquareToConcentricDiskMapping(float x, float y)
{
    float phi = 0.0f;
    float r = 0.0f;

    float a = 2.0f * x - 1.0f;
    float b = 2.0f * y - 1.0f;

    if (a > -b)                      // region 1 or 2
    {
        if (a > b)                   // region 1, also |a| > |b|
        {
            r = a;
            phi = k_PiOver4 * (b / a);
        }
        else                        // region 2, also |b| > |a|
        {
            r = b;
            phi = k_PiOver4 * (2.0f - (a / b));
        }
    }
    else                            // region 3 or 4
    {
        if (a < b)                   // region 3, also |a| >= |b|, a != 0
        {
            r = -a;
            phi = k_PiOver4 * (4.0f + (b / a));
        }
        else                        // region 4, |b| >= |a|, but a==0 and b==0 could occur.
        {
            r = -b;
            if (b != 0)
                phi = k_PiOver4 * (6.0f - (a / b));
            else
                phi = 0;
        }
    }

    float2 result;
    result.x = r * cos(phi);
    result.y = r * sin(phi);
    return result;
}

#endif