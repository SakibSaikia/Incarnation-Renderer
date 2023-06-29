#include "common/mesh-material.hlsli"
#include "material/common.hlsli"
#include "gpu-shared-types.h"
#include "encoding.hlsli"

#define rootsig \
	"RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=32, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b1, space = 0, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b2, space = 0, visibility = SHADER_VISIBILITY_ALL)," \
	"StaticSampler(s0, filter = FILTER_ANISOTROPIC, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE)"

struct FPassConstants
{
	uint primId;
};

ConstantBuffer<FPassConstants> g_passCb : register(b0);
ConstantBuffer<FViewConstants> g_viewCb : register(b1);
ConstantBuffer<FSceneConstants> g_sceneCb : register(b2);
SamplerState g_anisoSampler : register(s0);

struct vs_to_ps
{
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD;
	nointerpolation uint objectId : OBJECT_ID;
	nointerpolation uint materialId : MATERIAL_ID;
};

vs_to_ps vs_main(uint index : SV_VertexID)
{
	vs_to_ps o;

	// Load the primitive from the packed primitives buffer using the primitive id
	ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedScenePrimitivesBufferIndex];
	FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(g_passCb.primId * sizeof(FGpuPrimitive));

	ByteAddressBuffer meshTransformsBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedSceneMeshTransformsBufferIndex];
	float4x4 localToWorld = meshTransformsBuffer.Load<float4x4>(primitive.m_meshIndex * sizeof(float4x4));
	localToWorld = mul(localToWorld, g_sceneCb.m_sceneRotation);

	uint vertIndex = MeshMaterial::GetUint(index, primitive.m_indexAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
	float3 position = MeshMaterial::GetFloat3(vertIndex, primitive.m_positionAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
	float4 worldPos = mul(float4(position, 1.f), localToWorld);
	o.pos = mul(worldPos, g_viewCb.m_viewProjTransform);
	o.uv = MeshMaterial::GetFloat2(vertIndex, primitive.m_uvAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
	o.objectId = g_passCb.primId;
	o.materialId = primitive.m_materialIndex;

	return o;
}

uint ps_main(vs_to_ps interpolants, uint triangleId : SV_PrimitiveID) : SV_Target
{
	// Evaluate Material
	FMaterial material = MeshMaterial::GetMaterial(interpolants.materialId, g_sceneCb.m_sceneMaterialBufferIndex);
	FMaterialProperties matInfo = EvaluateMaterialProperties(material, interpolants.uv, g_anisoSampler);
	clip(matInfo.opacity - 0.5f);

	return EncodeVisibilityBuffer(interpolants.objectId, triangleId);
}