#ifndef __SAMPLING_HLSLI_
#define __SAMPLING_HLSLI_

#define PI 3.14159265f

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

float3 SampleDirectionHemisphere(float u1, float u2)
{
    float r = sqrt(max(0.0f, 1.0f - u1 * u1));
    float phi = 2.f * PI * u2;
    return float3(r * cos(phi), r * sin(phi), u1);
}

#endif