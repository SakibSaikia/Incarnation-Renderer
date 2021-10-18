// Adapted from https://www.gamedev.net/forums/topic/671562-spherical-harmonics-cubemap/

#define SH_BANDS 3
#define SH_COEFFICIENTS 9
#define SH_PI 3.14159265f

struct SH9
{
	float c[SH_COEFFICIENTS];
};

struct SH9Color
{
	float3 c[SH_COEFFICIENTS];
};

static const float shConst[SH_COEFFICIENTS] = {
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

static const float cosineZonalHarmonicCoefficients[SH_BANDS] = {
	SH_PI,
	(2.f * SH_PI) / 3.f,
	0.25f * SH_PI
};

SH9 ShEvaluate(float3 dir)
{
	SH9 sh;

	// Band 0
	sh.c[0] = shConst[0];

	// Band 1
	sh.c[1] = shConst[1] * dir.z;
	sh.c[2] = shConst[2] * dir.y;
	sh.c[3] = shConst[3] * dir.x;

	// Band 2
	sh.c[4] = shConst[4] * dir.x * dir.y;
	sh.c[5] = shConst[5] * dir.y * dir.z;
	sh.c[6] = shConst[6] * (3.0f * dir.z * dir.z - 1.0f);
	sh.c[7] = shConst[7] * dir.x * dir.z;
	sh.c[8] = shConst[8] * (dir.x * dir.x - dir.y * dir.y);

	return sh;
}

SH9 ShEvaluate(float theta, float phi)
{
	float sint = sin(theta);
	float cost = cos(theta);
	float sinp = sin(phi);
	float cosp = cos(phi);

	SH9 sh;

	// Band 0
	sh.c[0] = shConst[0];

	// Band 1
	sh.c[1] = shConst[1] * sinp * sint;
	sh.c[2] = shConst[2] * cost;
	sh.c[3] = shConst[3] * cosp * sint;

	// Band 2
	sh.c[4] = shConst[4] * sinp * cosp * sint * sint;
	sh.c[5] = shConst[5] * sinp * sint * cost;
	sh.c[6] = shConst[6] * (3.f * cost * cost - 1.f);
	sh.c[7] = shConst[7] * cosp * sint * cost;
	sh.c[8] = shConst[8] * (cosp * cosp - sinp * sinp) * sint * sint;

	return sh;
}

SH9 ShCosineLobe(float3 dir)
{
	SH9 sh = ShEvaluate(dir);

	// Band 0
	sh.c[0] *= cosineZonalHarmonicCoefficients[0];

	// Band 1
	sh.c[1] *= cosineZonalHarmonicCoefficients[1];
	sh.c[2] *= cosineZonalHarmonicCoefficients[1];
	sh.c[3] *= cosineZonalHarmonicCoefficients[1];

	// Band 2
	sh.c[4] *= cosineZonalHarmonicCoefficients[2];
	sh.c[5] *= cosineZonalHarmonicCoefficients[2];
	sh.c[6] *= cosineZonalHarmonicCoefficients[2];
	sh.c[7] *= cosineZonalHarmonicCoefficients[2];
	sh.c[8] *= cosineZonalHarmonicCoefficients[2];

	return sh;
}

SH9 ShCosineLobe(float theta, float phi)
{
	SH9 sh = ShEvaluate(theta, phi);

	// Band 0
	sh.c[0] *= cosineZonalHarmonicCoefficients[0];

	// Band 1
	sh.c[1] *= cosineZonalHarmonicCoefficients[1];
	sh.c[2] *= cosineZonalHarmonicCoefficients[1];
	sh.c[3] *= cosineZonalHarmonicCoefficients[1];

	// Band 2
	sh.c[4] *= cosineZonalHarmonicCoefficients[2];
	sh.c[5] *= cosineZonalHarmonicCoefficients[2];
	sh.c[6] *= cosineZonalHarmonicCoefficients[2];
	sh.c[7] *= cosineZonalHarmonicCoefficients[2];
	sh.c[8] *= cosineZonalHarmonicCoefficients[2];

	return sh;
}

float3 ShIrradiance(float3 normal, SH9Color radiance)
{
	// Compute the cosine lobe in SH, oriented about the normal direction
	SH9 shCosine = ShCosineLobe(normal);

	// Compute the SH dot product to get irradiance
	float3 irradiance = 0.f;
	for (uint i = 0; i < SH_COEFFICIENTS; ++i)
	{
		irradiance += radiance.c[i] * shCosine.c[i];
	}

	return irradiance;
}
