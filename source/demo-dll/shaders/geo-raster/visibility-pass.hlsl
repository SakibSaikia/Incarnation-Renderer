#include "common/mesh-material.hlsli"
#include "gpu-shared-types.h"
#include "encoding.hlsli"

#define rootsig \
	"RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=8, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b1, space = 0, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b2, space = 0, visibility = SHADER_VISIBILITY_ALL)"

struct FrameCbLayout
{
	float4x4 sceneRotation;
	int sceneMeshAccessorsIndex;
	int sceneMeshBufferViewsIndex;
	int primitivesBufferIndex;
};

struct ViewCbLayout
{
	float4x4 viewProjTransform;
};

struct PrimitiveCbLayout
{
	uint id;
};

ConstantBuffer<PrimitiveCbLayout> g_primitiveConstants : register(b0);
ConstantBuffer<ViewCbLayout> g_viewConstants : register(b1);
ConstantBuffer<FrameCbLayout> g_frameConstants : register(b2);

struct vs_to_ps
{
	float4 pos : SV_POSITION;
	nointerpolation uint objectId : OBJECT_ID;
};

vs_to_ps vs_main(uint index : SV_VertexID)
{
	vs_to_ps o;

	// Load the primitive from the packed primitives buffer using the primitive id
	ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[g_frameConstants.primitivesBufferIndex];
	FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(g_primitiveConstants.id * sizeof(FGpuPrimitive));

	float4x4 localToWorld = mul(primitive.m_localToWorld, g_frameConstants.sceneRotation);

	uint vertIndex = MeshMaterial::GetUint(index, primitive.m_indexAccessor, g_frameConstants.sceneMeshAccessorsIndex, g_frameConstants.sceneMeshBufferViewsIndex);
	float3 position = MeshMaterial::GetFloat3(vertIndex, primitive.m_positionAccessor, g_frameConstants.sceneMeshAccessorsIndex, g_frameConstants.sceneMeshBufferViewsIndex);
	float4 worldPos = mul(float4(position, 1.f), localToWorld);
	o.pos = mul(worldPos, g_viewConstants.viewProjTransform);
	o.objectId = g_primitiveConstants.id;

	return o;
}

uint ps_main(vs_to_ps interpolants, uint triangleId : SV_PrimitiveID) : SV_Target
{
	return EncodeVisibilityBuffer(interpolants.objectId, triangleId);
}