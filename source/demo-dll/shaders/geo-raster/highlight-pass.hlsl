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
	uint m_triangleId;
};

ConstantBuffer<FPassConstants> g_passCb : register(b0);
ConstantBuffer<FViewConstants> g_viewCb : register(b1);
ConstantBuffer<FSceneConstants> g_sceneCb : register(b2);


float4 vs_main(uint invocationIndex : SV_VertexID) : SV_POSITION
{
#if USING_MESHLETS
	// Use object id to retrieve the meshlet info
	ByteAddressBuffer meshletsBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedSceneMeshletsBufferIndex];
	const FGpuMeshlet meshlet = meshletsBuffer.Load<FGpuMeshlet>(g_passCb.m_objectId * sizeof(FGpuMeshlet));
    uint meshIndex = meshlet.m_meshIndex;
	
    // MeshletVertIndex is the index of the vertex within the meshlet eg. 0, 1, 2, etc.
	// The packed triangle index buffer contains one uint for every triangle packed as 10:10:10:2
    uint meshletTriangleIndex = g_passCb.m_triangleId + invocationIndex / 3;
    uint triangleVertIndex = invocationIndex % 3;
    
	ByteAddressBuffer packedTriangleIndexBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedMeshletPrimitiveIndexBufferIndex];
    uint packedMeshletTriangleIndex = packedTriangleIndexBuffer.Load<uint>((meshlet.m_triangleBegin + meshletTriangleIndex) * sizeof(uint));
    uint meshletVertIndex = 0xff & (packedMeshletTriangleIndex >> ((2 - triangleVertIndex) * 10));
    
    // Compute the unique vertex index for the current meshlet vert
    ByteAddressBuffer packedVertexIndexBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedMeshletVertexIndexBufferIndex];
    uint vertIndex = packedVertexIndexBuffer.Load<uint>((meshlet.m_vertexBegin + meshletVertIndex) * sizeof(uint));
    int positionAccessor = meshlet.m_positionAccessor;
    
#else
	// Use object id to retrieve the primitive info
	ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedScenePrimitivesBufferIndex];
	const FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(g_passCb.m_objectId * sizeof(FGpuPrimitive));
    uint meshIndex = primitive.m_meshIndex;
	
	// vert index
    uint indexOffset = g_passCb.m_triangleId * 3;
    uint vertIndex = MeshMaterial::GetUint(indexOffset + invocationIndex, primitive.m_indexAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    int positionAccessor = primitive.m_positionAccessor;
#endif

    ByteAddressBuffer meshTransformsBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedSceneMeshTransformsBufferIndex];
	float4x4 localToWorld = meshTransformsBuffer.Load<float4x4>(meshIndex * sizeof(float4x4));
	localToWorld = mul(localToWorld, g_sceneCb.m_sceneRotation);
    
	// position
    float3 position = MeshMaterial::GetFloat3(vertIndex, positionAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
	float4 worldPos = mul(float4(position, 1.f), localToWorld);
	return mul(worldPos, g_viewCb.m_viewProjTransform);
}

float4 ps_main() : SV_Target
{
	return 1.xxxx;
}