#ifndef __SH_COMMON_HLSLI_
#define __SH_COMMON_HLSLI_

/*
*	Adapted from https://www.gamedev.net/forums/topic/671562-spherical-harmonics-cubemap/
*	Also see: http://www.patapom.com/blog/SHPortal/
* 
*	To compute irradiance, you need to take your incoming radiance and integrate it with a cosine lobe oriented about the surface normal.
*	With spherical harmonics you typically have your incoming radiance represented as a set of SH coefficients(this is what you're computing 
*	when you integrate your cubemap onto the SH basis functions), which means that it makes sense to also represent the cosine lobe with SH. 
*	If you do this, then computing the integral can be done using an SH convolution, which is essentially just a dot product of two sets of 
*	SH coefficients. 
* 
*	==== Signal Encoding ====
*   Any signal whose value is available for a all directions about a sphere can be encoded (or projected) into SH coefficients. This is done
*	by integrating the function with each SH basis function K(l,m). The result gives the SH coeffient for that band l and coeffcient index m. 
* 
*   ==== Signal Decoding ====
*   An approximation of the original signal can be reconstructed by multiplying the coefficients with their respective SH basis function K(l,m)
*	and adding them up.
* 
*	==== Rendering Equation ====
*	The rendering equation states that the outgoing radiance at a point along a certain direction is the integral of the incoming radiance, BRDF 
*	and cosine factor. 
* 
*	The incoming irradiance field can be represented in terms of SH coeffients. The cosine lobe can also be represented in SH in an even simpler
*	form because it is invariant to the azimuth angle - these are the zonal harmonics. In fact, the SH coefficients of the cosine lobe can 
*	be computed analytically.
*	
*	Using the signal convolution property of SH - which states that the integration of the product of two signals is same as the dot product of the
*	SH coefficients of the respective signals - we can now easily compute the irradiance.
*	
*/

// Each SH band encodes higher frequencies and directonality starting with band 0, which is just the ambient term.
// Each band l, has (2l + 1) coefficients ranging from [-l, +l] 
#define SH_NUM_BANDS 3
#define SH_NUM_COEFFICIENTS (SH_NUM_BANDS * SH_NUM_BANDS)
#define SH_PI 3.14159265f

// Handy struct to store evaluated SH values 
struct SH9
{
	float value[SH_NUM_COEFFICIENTS];
};

// L2 SH coefficients for a scalar-valued spherical function
struct SH9Coefficient
{
	float c[SH_NUM_COEFFICIENTS];
};

// L2 SH coefficients for a RGB spherical function
struct SH9ColorCoefficient
{
	float3 c[SH_NUM_COEFFICIENTS];
};

// Spherical harmonic normalization factor constants K(l, m) = sqrt(((2l+1)*(l-m)!)/(4pi * (l+m)!))
static const float K[SH_NUM_COEFFICIENTS] = {
	0.282095f,
	0.488603f,
	0.488603f,
	0.488603f,
	1.092548f,
	1.092548f,
	0.315392f,
	1.092548f,
	0.546274f
};

// Cosine Zonal Harmonic Coefficients
static const float A[SH_NUM_BANDS] = {
	SH_PI,
	2.094395f,
	0.785398f
};

// Analytical expressions for SH values of each SH basis function for the specified unit vector.
SH9 ShEvaluate(float3 dir)
{
	SH9 sh;

	// L0
	sh.value[0] = K[0];

	// L1
	sh.value[1] = K[1] * dir.y;
	sh.value[2] = K[2] * dir.z;
	sh.value[3] = K[3] * dir.x;

	// L2
	sh.value[4] = K[4] * dir.x * dir.y;
	sh.value[5] = K[5] * dir.y * dir.z;
	sh.value[6] = K[6] * (3.0f * dir.z * dir.z - 1.0f);
	sh.value[7] = K[7] * dir.x * dir.z;
	sh.value[8] = K[8] * (dir.x * dir.x - dir.y * dir.y);

	return sh;
}

// SH coefficients of the cosine lobe computed analytically
SH9Coefficient ShCosineLobe(float3 dir)
{
	SH9 sh = ShEvaluate(dir);
	SH9Coefficient result;

	// L0
	result.c[0] = sh.value[0] * A[0];

	// L1
	result.c[1] = sh.value[1] * A[1];
	result.c[2] = sh.value[2] * A[1];
	result.c[3] = sh.value[3] * A[1];

	// L2
	result.c[4] = sh.value[4] * A[2];
	result.c[5] = sh.value[5] * A[2];
	result.c[6] = sh.value[6] * A[2];
	result.c[7] = sh.value[7] * A[2];
	result.c[8] = sh.value[8] * A[2];

	return result;
}

float3 ShIrradiance(float3 normal, SH9ColorCoefficient shRadiance)
{
	// Compute the cosine lobe in SH, oriented about the normal direction
	SH9Coefficient shCosine = ShCosineLobe(normal);

	// Compute the SH dot product to get irradiance
	float3 irradiance = 0.f;
	for (uint i = 0; i < SH_NUM_COEFFICIENTS; ++i)
	{
		irradiance += shRadiance.c[i] * shCosine.c[i];
	}

	return irradiance;
}

#endif 