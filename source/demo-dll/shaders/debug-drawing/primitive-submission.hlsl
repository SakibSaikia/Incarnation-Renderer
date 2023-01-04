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
	int debugMeshAccessorsIndex;
	int debugMeshBufferViewsIndex;
	int debugPrimitivesBufferIndex;
};

struct DebugPrimitiveDrawData
{
	float4 m_color;
	float4x4 m_transform;
	uint m_shapeType;
	uint3 __pad0;
	uint4 __pad1;
	uint4 __pad2;
};

cbuffer cb1 : register(b1)
{
	float4x4 g_viewProjTransform;
};

ConstantBuffer<DebugPrimitiveDrawData> g_debugDraw : register(b0);
ConstantBuffer<FrameCbLayout> g_frameConstants : register(b2);

float4 vs_main(uint index : SV_VertexID) : SV_POSITION
{
	// Use object id to retrieve the primitive info
	ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[g_frameConstants.debugPrimitivesBufferIndex];
	const FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(g_debugDraw.m_shapeType * sizeof(FGpuPrimitive));

	float4x4 localToWorld = mul(g_debugDraw.m_transform, g_frameConstants.sceneRotation);

	// index
	uint vertIndex = MeshMaterial::GetUint(index, primitive.m_indexAccessor, g_frameConstants.debugMeshAccessorsIndex, g_frameConstants.debugMeshBufferViewsIndex);

	// position
	float3 position = MeshMaterial::GetFloat3(vertIndex, primitive.m_positionAccessor, g_frameConstants.debugMeshAccessorsIndex, g_frameConstants.debugMeshBufferViewsIndex);
	float4 worldPos = mul(float4(position, 1.f), localToWorld);
	return mul(worldPos, g_viewProjTransform);
}

float4 ps_main() : SV_Target
{
	return g_debugDraw.m_color;
}