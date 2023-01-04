#include "common/mesh-material.hlsli"
#include "gpu-shared-types.h"

#define rootsig \
	"RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED )," \
    "RootConstants(b0, num32BitConstants=32, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b1, space = 0, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b2, space = 0, visibility = SHADER_VISIBILITY_ALL)," \
	"StaticSampler(s0, filter = FILTER_ANISOTROPIC, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE)"

struct FrameCbLayout
{
	float4x4 sceneRotation;
};

struct DebugLineDrawData
{
	float4 m_start;
	float4 m_end;
	float4 m_color;
	float4 __pad[5];
};

cbuffer cb1 : register(b1)
{
	float4x4 g_viewProjTransform;
};

ConstantBuffer<DebugLineDrawData> g_debugDraw : register(b0);
ConstantBuffer<FrameCbLayout> g_frameConstants : register(b2);

float4 vs_main(uint index : SV_VertexID) : SV_POSITION
{
	float4 worldPos = index == 0 ? g_debugDraw.m_start : g_debugDraw.m_end;
	worldPos = mul(worldPos, g_frameConstants.sceneRotation);
	return mul(worldPos, g_viewProjTransform);
}

float4 ps_main() : SV_Target
{
	return g_debugDraw.m_color;
}