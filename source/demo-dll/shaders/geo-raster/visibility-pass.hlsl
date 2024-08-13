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
	// primitive or meshlet id
	uint id;
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

vs_to_ps vs_primitive_main(uint invocationIndex : SV_VertexID)
{
	vs_to_ps o;
	
    uint primitiveId = g_passCb.id;

	// Load the primitive from the packed primitives buffer using the primitive id
	ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedScenePrimitivesBufferIndex];
    FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(primitiveId * sizeof(FGpuPrimitive));

	ByteAddressBuffer meshTransformsBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedSceneMeshTransformsBufferIndex];
	float4x4 localToWorld = meshTransformsBuffer.Load<float4x4>(primitive.m_meshIndex * sizeof(float4x4));
	localToWorld = mul(localToWorld, g_sceneCb.m_sceneRotation);

    uint vertIndex = MeshMaterial::GetUint(invocationIndex, primitive.m_indexAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
	float3 position = MeshMaterial::GetFloat3(vertIndex, primitive.m_positionAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
	float4 worldPos = mul(float4(position, 1.f), localToWorld);
	o.pos = mul(worldPos, g_viewCb.m_viewProjTransform);
	o.uv = MeshMaterial::GetFloat2(vertIndex, primitive.m_uvAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.objectId = primitiveId;
	o.materialId = primitive.m_materialIndex;

	return o;
}

vs_to_ps vs_meshlet_main(uint invocationIndex : SV_VertexID)
{
    vs_to_ps o;
	
    uint meshletId = g_passCb.id;

	// Load the meshlet from the packed meshlets buffer using the meshlet id
    ByteAddressBuffer meshletsBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedSceneMeshletsBufferIndex];
    FGpuMeshlet meshlet = meshletsBuffer.Load<FGpuMeshlet>(meshletId * sizeof(FGpuMeshlet));
	
	// Meshlet transform
    ByteAddressBuffer meshTransformsBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedSceneMeshTransformsBufferIndex];
    float4x4 localToWorld = meshTransformsBuffer.Load<float4x4>(meshlet.m_meshIndex * sizeof(float4x4));
    localToWorld = mul(localToWorld, g_sceneCb.m_sceneRotation);
	
	// MeshletVertIndex is the index of the vertex within the meshlet eg. 0, 1, 2, etc.
	// The packed triangle index buffer contains one uint for every triangle packed as 10:10:10:2
    uint meshletTriangleIndex = invocationIndex / 3;
    uint triangleVertIndex = invocationIndex % 3;
    ByteAddressBuffer packedTriangleIndexBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedMeshletPrimitiveIndexBufferIndex];
    uint packedMeshletTriangleIndex = packedTriangleIndexBuffer.Load<uint>((meshlet.m_triangleBegin + meshletTriangleIndex) * sizeof(uint));
    uint meshletVertIndex = 0xff & (packedMeshletTriangleIndex >> ((2 - triangleVertIndex) * 10));
	
	// Compute the unique vertex index for the current meshlet vert
    ByteAddressBuffer packedVertexIndexBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedMeshletVertexIndexBufferIndex];
    uint uniqueVertIndex = packedVertexIndexBuffer.Load<uint>((meshlet.m_vertexBegin + meshletVertIndex) * sizeof(uint));
	
	// Read vertex attributes for the vertex buffer(s)
    float3 position = MeshMaterial::GetFloat3(uniqueVertIndex, meshlet.m_positionAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    float4 worldPos = mul(float4(position, 1.f), localToWorld);
    o.pos = mul(worldPos, g_viewCb.m_viewProjTransform);
    o.uv = MeshMaterial::GetFloat2(uniqueVertIndex, meshlet.m_uvAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.objectId = meshletId;
    o.materialId = meshlet.m_materialIndex;

    return o;
	
}

uint ps_primitive_main(vs_to_ps interpolants, uint triangleId : SV_PrimitiveID) : SV_Target
{
	// Evaluate Material
	FMaterial material = MeshMaterial::GetMaterial(interpolants.materialId, g_sceneCb.m_sceneMaterialBufferIndex);
	FMaterialProperties matInfo = EvaluateMaterialProperties(material, interpolants.uv, g_anisoSampler);
	clip(matInfo.opacity - 0.5f);

	return EncodePrimitiveVisibility(interpolants.objectId, triangleId);
}

uint ps_meshlet_main(vs_to_ps interpolants, uint triangleId : SV_PrimitiveID) : SV_Target
{
	// Evaluate Material
    FMaterial material = MeshMaterial::GetMaterial(interpolants.materialId, g_sceneCb.m_sceneMaterialBufferIndex);
    FMaterialProperties matInfo = EvaluateMaterialProperties(material, interpolants.uv, g_anisoSampler);
    clip(matInfo.opacity - 0.5f);

    return EncodeMeshletVisibility(interpolants.objectId, triangleId);
}