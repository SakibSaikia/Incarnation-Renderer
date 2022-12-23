// Based on https://www.shadertoy.com/view/4ldXRS

#include "common/math.hlsli"
#include "gpu-shared-types.h"

#define rootsig \
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)"

SamplerState g_anisoSampler : register(s0);

cbuffer cb : register(b0)
{
	float4x4 g_invParallaxViewProjMatrix;
	FPerezDistribution g_perezConstants;
	float g_turbidity;
	float3 g_sunDir;
}

struct vs_to_ps
{
	float4 pos : SV_POSITION;
	float4 pixelPos : INTERPOLATED_POS;
	float2 uv : INTERPOLATED_UV_0;
};

vs_to_ps vs_main(uint id : SV_VertexID)
{
	vs_to_ps o;

	// generate clip space position
	o.pos.x = (float)(id / 2) * 4.f - 1.f;
	o.pos.y = (float)(id % 2) * 4.f - 1.f;
	o.pos.z = 0.0001f;
	o.pos.w = 1.f;

	// texture coordinates
	o.uv.x = (float)(id / 2) * 2.f;
	o.uv.y = 1.f - (float)(id % 2) * 2.f;

	o.pixelPos = o.pos;

	return o;
}

float3 Yxy2XYZ(float3 Yxy)
{
	float Y = Yxy.r;
	float x = Yxy.g;
	float y = Yxy.b;

	float X = x * (Y / y);
	float Z = (1.0 - x - y) * (Y / y);

	return float3(X, Y, Z);
}

float3 XYZ2RGB(float3 XYZ)
{
	float3 RGB;
	RGB.r = 3.2404542 * XYZ.x - 1.5371385 * XYZ.y - 0.4985314 * XYZ.z;
	RGB.g = -0.9692660 * XYZ.x + 1.8760108 * XYZ.y + 0.0415560 * XYZ.z;
	RGB.b = 0.0556434 * XYZ.x - 0.2040259 * XYZ.y + 1.0572252 * XYZ.z;

	return RGB;
}


float3 Yxy2RGB(float3 Yxy)
{
	float3 XYZ = Yxy2XYZ(Yxy);
	float3 RGB = XYZ2RGB(XYZ);
	return RGB;
}

float3 Perez(float theta, float gamma)
{
	#define A g_perezConstants.A.xyz
	#define B g_perezConstants.B.xyz
	#define C g_perezConstants.C.xyz
	#define D g_perezConstants.D.xyz
	#define E g_perezConstants.E.xyz

	return (1.0 + A * exp(B / cos(theta))) * (1.0 + C * exp(D * gamma) + E * cos(gamma) * cos(gamma));
}

float3 CalculateZenithLuminanceAndChromaticity(float t, float thetaS)
{
	float chi = (4.f / 9.f - t / 120.f) * (k_Pi - 2.f * thetaS);
	float Y_zenith = (4.0453 * t - 4.9710) * tan(chi) - 0.2155 * t + 2.4192;

	float theta2 = thetaS * thetaS;
	float theta3 = theta2 * thetaS;
	float T = t;
	float T2 = t * t;

	float x_zenith =
		(0.00165 * theta3 - 0.00375 * theta2 + 0.00209 * thetaS + 0.0) * T2 +
		(-0.02903 * theta3 + 0.06377 * theta2 - 0.03202 * thetaS + 0.00394) * T +
		(0.11693 * theta3 - 0.21196 * theta2 + 0.06052 * thetaS + 0.25886);

	float y_zenith =
		(0.00275 * theta3 - 0.00610 * theta2 + 0.00317 * thetaS + 0.0) * T2 +
		(-0.04214 * theta3 + 0.08970 * theta2 - 0.04153 * thetaS + 0.00516) * T +
		(0.15346 * theta3 - 0.26756 * theta2 + 0.06670 * thetaS + 0.26688);

	return float3(Y_zenith, x_zenith, y_zenith);
}

float3 CalculateSkyRadianceRGB(float3 s, float3 e, float t)
{
	float thetaS = acos(saturate(dot(s, float3(0, 1, 0)))); // Sun elevation angle
	float thetaE = acos(saturate(dot(e, float3(0, 1, 0))));	// Sky dir elevation angle
	float gammaE = acos(saturate(dot(s, e)));				// Angle between sun and sky dir

	float3 Yxy_zenith = CalculateZenithLuminanceAndChromaticity(t, thetaS);

	float3 F_theta_gamma = Perez(thetaE, gammaE);
	float3 F_zero_thetaS = Perez(0.f, thetaS);

	float3 Yxy = Yxy_zenith * (F_theta_gamma / F_zero_thetaS);
	return Yxy2RGB(Yxy);
}

float4 ps_main(vs_to_ps input) : SV_Target
{
	float4 dir = mul(input.pixelPos, g_invParallaxViewProjMatrix);
	dir /= dir.w;

	float3 radiance = 1000 * CalculateSkyRadianceRGB(g_sunDir, normalize(dir.xyz), g_turbidity);

	return float4(radiance, 0.f);
}