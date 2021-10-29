#include "raytracing/common.hlsli"
#include "mesh-material.h"

GlobalRootSignature k_globalRootsig =
{
    "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED )," \
    "CBV(b0) " \
};


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
    float3 cameraPosition;
    int destUavIndex;
    float4x4 projectionToWorld;
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

struct MissCbLayout
{
    int envmapIndex;
};

RWTexture2D<float4> g_uavBindless2DTextures[] : register(u0);
ConstantBuffer<GlobalCbLayout> g_globalConstants : register(b0);
ConstantBuffer<HitgroupCbLayout> g_hitgroupConstants : register(b1);
ConstantBuffer<MissCbLayout> g_missConstants : register(b2);
SamplerState g_anisoSampler : register(s0, space1);

[shader("raygeneration")]
void rgsMain()
{
    RWTexture2D<float4> destUav = ResourceDescriptorHeap[g_globalConstants.destUavIndex];
    RaytracingAccelerationStructure sceneBvh = ResourceDescriptorHeap[g_globalConstants.sceneBvhIndex];

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

    payload.color = float4(NoL, NoL, NoL, 1.f);
}

[shader("miss")]
void msMain(inout RayPayload payload)
{
    TextureCube envmap = ResourceDescriptorHeap[g_missConstants.envmapIndex];
    payload.color = float4(envmap.SampleLevel(g_anisoSampler, WorldRayDirection(), 0).rgb, 1.f);
}


