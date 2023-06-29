#include "common/mesh-material.hlsli"
#include "gpu-shared-types.h"
#include "encoding.hlsli"

#define rootsig \
	"RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED )," \
    "RootConstants(b0, num32BitConstants=32, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b1, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b2, visibility = SHADER_VISIBILITY_ALL), " \
	"StaticSampler(s0, filter = FILTER_ANISOTROPIC, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE)"

struct FPassConstants
{
	uint m_objectId;
	uint m_indexOffset;
};

ConstantBuffer<FPassConstants> g_passCb : register(b0);
ConstantBuffer<FViewConstants> g_viewCb : register(b1);
ConstantBuffer<FSceneConstants> g_sceneCb : register(b2);


float4 vs_main(uint index : SV_VertexID) : SV_POSITION
{
	// Use object id to retrieve the primitive info
	ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedScenePrimitivesBufferIndex];
	const FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(g_passCb.m_objectId * sizeof(FGpuPrimitive));

	ByteAddressBuffer meshTransformsBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedSceneMeshTransformsBufferIndex];
	float4x4 localToWorld = meshTransformsBuffer.Load<float4x4>(primitive.m_meshIndex * sizeof(float4x4));
	localToWorld = mul(localToWorld, g_sceneCb.m_sceneRotation);

	// index
	uint vertIndex = MeshMaterial::GetUint(g_passCb.m_indexOffset + index, primitive.m_indexAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);

	// position
	float3 position = MeshMaterial::GetFloat3(vertIndex, primitive.m_positionAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
	float4 worldPos = mul(float4(position, 1.f), localToWorld);
	return mul(worldPos, g_viewCb.m_viewProjTransform);
}

float4 ps_main() : SV_Target
{
	return 1.xxxx;
}