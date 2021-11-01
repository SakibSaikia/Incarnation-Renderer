#include "raytracing/common.hlsli"
#include "mesh-material.h"

GlobalRootSignature k_globalRootsig =
{
    "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED )," \
    "CBV(b0), " \
    "SRV(t0), " \
    "StaticSampler(s0, space = 1, filter = FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP) "
};


LocalRootSignature k_hitGroupLocalRootsig =
{
    "RootConstants(b1, num32BitConstants = 6)"
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
    float3 cameraPosition;
    int destUavIndex;
    float4x4 projectionToWorld;
    int envmapTextureIndex;
};

struct HitgroupCbLayout
{
    int indexAccessor;
    int positionAccessor;
    int uvAccessor;
    int normalAccessor;
    int tangentAccessor;
    int materialIndex;
};

ConstantBuffer<GlobalCbLayout> g_globalConstants : register(b0);
ConstantBuffer<HitgroupCbLayout> g_hitgroupConstants : register(b1);
SamplerState g_envmapSampler : register(s0, space1);
RaytracingAccelerationStructure g_sceneBvh : register(t0);

[shader("raygeneration")]
void rgsMain()
{
    RWTexture2D<float4> destUav = ResourceDescriptorHeap[g_globalConstants.destUavIndex];

    RayDesc ray = GenerateCameraRay(DispatchRaysIndex().xy, g_globalConstants.cameraPosition, g_globalConstants.projectionToWorld);
    RayPayload payload = { float4(0, 0, 0, 0) };
    TraceRay(g_sceneBvh, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, ~0, 0, 1, 0, ray, payload);
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

    payload.color = float4(NoL, NoL, NoL, 0.f);
}

[shader("miss")]
void msMain(inout RayPayload payload)
{
    TextureCube envmap = ResourceDescriptorHeap[g_globalConstants.envmapTextureIndex];
    payload.color = float4(envmap.SampleLevel(g_envmapSampler, WorldRayDirection(), 0).rgb, 0.f);
}


