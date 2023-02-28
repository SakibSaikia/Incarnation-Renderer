#include "common/mesh-material.hlsli"
#include "material/common.hlsli"
#include "gpu-shared-types.h"
#include "encoding.hlsli"

#define rootsig \
	"RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED )," \
    "RootConstants(b0, num32BitConstants=22, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b1, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b2, visibility = SHADER_VISIBILITY_ALL)," \
	"StaticSampler(s0, filter = FILTER_ANISOTROPIC, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE)"

struct FPrimitiveConstants
{
	float4x4 localToWorld;
	int indexAccessor;
	int positionAccessor;
	int uvAccessor;
	int normalAccessor;
	int tangentAccessor;
	int materialIndex;
};

ConstantBuffer<FPrimitiveConstants> g_primCb : register(b0);
ConstantBuffer<FViewConstants> g_viewCb : register(b1);
ConstantBuffer<FSceneConstants> g_sceneCb : register(b2);
SamplerState g_anisoSampler : register(s0);

struct vs_to_ps
{
	float4 pos : SV_POSITION;
	float2 uv : INTERPOLATED_UV_0;
	float4 normal : INTERPOLATED_WORLD_NORMAL;
	float4 tangent : INTERPOLATED_WORLD_TANGENT;
	float4 bitangent : INTERPOLATED_WORLD_BITANGENT;
};

struct gbuffer
{
	float4 basecolor : SV_TARGET0;
	float2 normals : SV_TARGET1;
	float4 metallic_roughness_ao : SV_TARGET2;
};

vs_to_ps vs_main(uint index : SV_VertexID)
{
	vs_to_ps o;

	float4x4 localToWorld = mul(g_primCb.localToWorld, g_sceneCb.m_sceneRotation);

	// index
	uint vertIndex = MeshMaterial::GetUint(index, g_primCb.indexAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);

	// position
	float3 position = MeshMaterial::GetFloat3(vertIndex, g_primCb.positionAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
	float4 worldPos = mul(float4(position, 1.f), localToWorld);
	o.pos = mul(worldPos, g_viewCb.m_viewProjTransform);

	// uv
	o.uv = MeshMaterial::GetFloat2(vertIndex, g_primCb.uvAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);

	// normal
	float3 normal = MeshMaterial::GetFloat3(vertIndex, g_primCb.normalAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
	o.normal = mul(float4(normal, 0.f), localToWorld);

	// tangent
	float4 packedTangent = MeshMaterial::GetFloat4(vertIndex, g_primCb.tangentAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
	float3 tangent = packedTangent.xyz;
	o.tangent = mul(float4(tangent, 0.f), localToWorld);

	// bitangent
	float3 bitangent = cross(normal, tangent) * packedTangent.w;
	o.bitangent = mul(float4(bitangent, 0.f), localToWorld);

	return o;
}

gbuffer ps_main(vs_to_ps input)
{
	FMaterial material = MeshMaterial::GetMaterial(g_primCb.materialIndex, g_sceneCb.m_sceneMaterialBufferIndex);
	FMaterialProperties p = EvaluateMaterialProperties(material, input.uv, g_anisoSampler);

	// Tangent space transform
	float3 T = normalize(input.tangent.xyz);
	float3 B = normalize(input.bitangent.xyz);
	float3 N = normalize(input.normal.xyz);
	float3x3 tangentToWorld = float3x3(T, B, N);

	if (p.bHasNormalmap)
	{
		N = normalize(mul(p.normalmap, tangentToWorld));
	}

	gbuffer output;
	output.basecolor = float4(p.basecolor, p.opacity);
	output.normals = OctEncode(N);
	output.metallic_roughness_ao = float4(p.metallic, p.roughness, p.ao, p.aoblend);
	return output;
}