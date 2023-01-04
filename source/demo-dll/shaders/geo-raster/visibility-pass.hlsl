#include "common/mesh-material.hlsli"
#include "gpu-shared-types.h"
#include "encoding.hlsli"

#define rootsig \
	"RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=32, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b1, space = 0, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b2, space = 0, visibility = SHADER_VISIBILITY_ALL)"

struct FPassConstants
{
	uint primId;
};

ConstantBuffer<FPassConstants> g_passCb : register(b0);
ConstantBuffer<FViewConstants> g_viewCb : register(b1);
ConstantBuffer<FSceneConstants> g_sceneCb : register(b2);

struct vs_to_ps
{
	float4 pos : SV_POSITION;
	nointerpolation uint objectId : OBJECT_ID;
};

vs_to_ps vs_main(uint index : SV_VertexID)
{
	vs_to_ps o;

	// Load the primitive from the packed primitives buffer using the primitive id
	ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[g_sceneCb.m_scenePrimitivesIndex];
	FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(g_passCb.primId * sizeof(FGpuPrimitive));

	float4x4 localToWorld = mul(primitive.m_localToWorld, g_sceneCb.m_sceneRotation);

	uint vertIndex = MeshMaterial::GetUint(index, primitive.m_indexAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
	float3 position = MeshMaterial::GetFloat3(vertIndex, primitive.m_positionAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
	float4 worldPos = mul(float4(position, 1.f), localToWorld);
	o.pos = mul(worldPos, g_viewCb.m_viewProjTransform);
	o.objectId = g_passCb.primId;

	return o;
}

uint ps_main(vs_to_ps interpolants, uint triangleId : SV_PrimitiveID) : SV_Target
{
	return EncodeVisibilityBuffer(interpolants.objectId, triangleId);
}