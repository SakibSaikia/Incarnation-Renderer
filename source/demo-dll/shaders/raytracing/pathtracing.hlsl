#include "raytracing/common.hlsli"
#include "mesh-material.h"

#define global_rootsig \
    "CBV(b0, space = 0), " \
    "DescriptorTable(SRV(t0, space = 0, numDescriptors = 1000), " \
    "DescriptorTable(SRV(t1, space = 1, numDescriptors = 1000), " \
    "DescriptorTable(SRV(t2, space = 2, numDescriptors = 1000), " \
    "DescriptorTable(SRV(t3, space = 3, numDescriptors = 1000), " \
    "DescriptorTable(Sampler(s0, space = 0, numDescriptors = 16)"


LocalRootSignature k_hitGroupLocalRootsig =
{
    "RootConstants(b1, num32BitConstants = 6)"
};

LocalRootSignature k_missShaderLocalRootsig =
{
    "RootConstants(b2, num32BitConstants = 1),"
    "StaticSampler(s0, space = 1, filter = FILTER_ANISOTROPIC, maxAnisotropy = 8, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP)"
};

TriangleHitGroup k_hitGroup =
{
    "",                                 // AnyHit Shader
    "chsMain",                          // ClosestHit Shader
};

SubobjectToExportsAssociation  k_hitGroupLocalRootsigAssociation =
{
    "k_hitGroupLocalRootsig",           // subobject name
    "k_hitGroup"                        // export association 
};

SubobjectToExportsAssociation  k_missShaderlocalRootsigAssociation =
{
    "k_missShaderLocalRootsig",         // subobject name
    "msMain"                            // export association 
};

RaytracingShaderConfig  k_shaderConfig =
{
    16,                                 // max payload size
    8                                   // max attribute size
};

RaytracingPipelineConfig k_pipelineConfig =
{
    1                                   // max trace recursion depth
};

struct RayPayload
{
    float4 color;
};

struct GlobalCbLayout
{
    int sceneMeshAccessorsIndex;
    int sceneMeshBufferViewsIndex;
    int sceneMaterialBufferIndex;
    int sceneBvhIndex;
    Vector3 cameraPosition;
    int _pad0;
    Matrix projectionToWorld;
};

struct HitgroupCbLayout
{
    int m_indexAccessor;
    int m_positionAccessor;
    int m_uvAccessor;
    int m_normalAccessor;
    int m_tangentAccessor;
    int m_materialIndex;
};

struct MissCbLayout
{
    int envmapIndex;
};

ConstantBuffer<GlobalCbLayout> g_globalConstants : register(b0);
ConstantBuffer<HitgroupCbLayout> g_hitgroupConstants : register(b1);
ConstantBuffer<MissCbLayout> g_missConstants : register(b2);
SamplerState g_anisoSampler : register(s0, space1);

[shader("raygeneration")]
void rgsMain()
{
    RWTexture2DArray<float4> destUav = g_uavBindless2DTextureArrays[g_globalConstants.destUavIndex];
    RaytracingAccelerationStructure sceneBvh = g_accelerationStructures[g_globalConstants.sceneBvhIndex];

    RayDesc ray = GenerateCameraRay(DispatchRaysIndex().xy, g_globalConstants.cameraPosition, g_globalConstants.projectionToWorld);
    RayPayload payload = { float4(0, 0, 0, 0) };
    TraceRay(sceneBvh, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, ~0, 0, 1, 0, ray, payload);
    destUav[DispatchRaysIndex().xy] = payload.color;
}

[shader("closesthit")]
void chsMain(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    const int globalMeshAccessorsIndex = g_globalConstants.sceneMeshAccessorsIndex;
    const int globalMehsBufferViewsIndex = g_globalConstants.sceneMeshBufferViewsIndex;

    float3 hitPosition = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();

    // Get the base index of the hit triangle
    const uint indicesPerTriangle = 3;
    uint baseIndex = PrimitiveIndex() * indicesPerTriangle;

    // Load up 3 indices for the triangle
    const uint3 indices = MeshMaterial::GetUint(baseIndex, g_hitgroupConstants.indexAccessor, globalMeshAccessorsIndex, globalMehsBufferViewsIndex);

    // Retrieve corresponding vertex normals for the triangle vertices
    float3 vertexNormals[3] = 
    {
        MeshMaterial::GetFloat3(indices.x, g_hitgroupConstants.normalAccessor, globalMeshAccessorsIndex, globalMehsBufferViewsIndex),
        MeshMaterial::GetFloat3(indices.y, g_hitgroupConstants.normalAccessor, globalMeshAccessorsIndex, globalMehsBufferViewsIndex),
        MeshMaterial::GetFloat3(indices.z, g_hitgroupConstants.normalAccessor, globalMeshAccessorsIndex, globalMehsBufferViewsIndex)
    };

    // Compute the triangle's normal
    float3 N = HitAttribute(vertexNormals, attr.barycentrics);
    float3 L = normalize(float3(1, 1, -1));
    float NoL = saturate(dot(N, L));

    payload.color = NoL;
}

[shader("miss")]
void msMain(inout RayPayload payload)
{
    payload.color = g_bindlessCubeTextures[g_missConstants.envmapTextureIndex].Sample(g_anisoSampler, WorldRayDirection()).rgb;
}


