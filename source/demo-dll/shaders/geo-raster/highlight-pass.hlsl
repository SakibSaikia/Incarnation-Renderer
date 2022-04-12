#include "common/mesh-material.hlsli"
#include "gpu-shared-types.h"
#include "encoding.hlsli"

#define rootsig \
	"RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED )," \
    "RootConstants(b0, num32BitConstants=32, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b1, space = 0, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b2, space = 0, visibility = SHADER_VISIBILITY_ALL)"

struct FrameCbLayout
{
	float4x4 sceneRotation;
	int sceneMeshAccessorsIndex;
	int sceneMeshBufferViewsIndex;
	int scenePrimitivesBufferIndex;;
};

struct ViewCbLayout
{
	float4x4 viewTransform;
	float4x4 projectionTransform;
	float3 eyePos;
};

cbuffer cb : register(b0)
{
	uint g_visibilityId;
};

ConstantBuffer<ViewCbLayout> g_viewConstants : register(b1);
ConstantBuffer<FrameCbLayout> g_frameConstants : register(b2);

float4 vs_main(uint index : SV_VertexID) : SV_POSITION
{
	// Retrieve object and triangle id for vis buffer
	uint objectId, triangleId;
	DecodeVisibilityBuffer(g_visibilityId, objectId, triangleId);

	// Use object id to retrieve the primitive info
	ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[g_frameConstants.scenePrimitivesBufferIndex];
	const FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(objectId * sizeof(FGpuPrimitive));

	float4x4 localToWorld = mul(primitive.m_localToWorld, g_frameConstants.sceneRotation);
	float4x4 viewProjTransform = mul(g_viewConstants.viewTransform, g_viewConstants.projectionTransform);

	// index
	uint vertIndex = MeshMaterial::GetUint(index, primitive.m_indexAccessor, g_frameConstants.sceneMeshAccessorsIndex, g_frameConstants.sceneMeshBufferViewsIndex);

	// position
	float3 position = MeshMaterial::GetFloat3(vertIndex, primitive.m_positionAccessor, g_frameConstants.sceneMeshAccessorsIndex, g_frameConstants.sceneMeshBufferViewsIndex);
	float4 worldPos = mul(float4(position, 1.f), localToWorld);
	return mul(worldPos, viewProjTransform);
}

float4 ps_main() : SV_Target
{
	return 1.xxxx;
}