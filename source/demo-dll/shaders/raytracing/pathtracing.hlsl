#include "raytracing/common.hlsli"
#include "lighting/common.hlsli"

GlobalRootSignature k_globalRootsig =
{
    "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED )," \
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
    const int globalMeshBufferViewsIndex = g_globalConstants.sceneMeshBufferViewsIndex;
    const int globalMaterialBufferIndex = g_globalConstants.sceneMaterialBufferIndex;

    float3 hitPosition = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();

    // Get the base index of the hit triangle
    const uint indicesPerTriangle = 3;
    uint baseIndex = PrimitiveIndex() * indicesPerTriangle;

    // Load up 3 indices for the triangle
    const uint3 indices = MeshMaterial::GetUint3(baseIndex, g_hitgroupConstants.indexAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex);

    // Tangent bases
    float3 vertexNormals[3] = 
    {
        MeshMaterial::GetFloat3(indices.x, g_hitgroupConstants.normalAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex),
        MeshMaterial::GetFloat3(indices.y, g_hitgroupConstants.normalAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex),
        MeshMaterial::GetFloat3(indices.z, g_hitgroupConstants.normalAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex)
    };

    float4 vertexTangents[3] =
    {
        MeshMaterial::GetFloat4(indices.x, g_hitgroupConstants.tangentAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex),
        MeshMaterial::GetFloat4(indices.y, g_hitgroupConstants.tangentAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex),
        MeshMaterial::GetFloat4(indices.z, g_hitgroupConstants.tangentAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex)
    };

    float4 packedT = HitAttribute(vertexTangents, attr.barycentrics);
    float3 N = normalize(HitAttribute(vertexNormals, attr.barycentrics));
    float3 T = normalize(packedT.xyz);
    float3 B = cross(N, T) * packedT.w;
    float3x3 TBN = float3x3(T, B, N);

    // UVs
    float2 vertexUVs[3] =
    {
        MeshMaterial::GetFloat2(indices.x, g_hitgroupConstants.uvAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex),
        MeshMaterial::GetFloat2(indices.y, g_hitgroupConstants.uvAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex),
        MeshMaterial::GetFloat2(indices.z, g_hitgroupConstants.uvAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex)
    };

    float2 uv = HitAttribute(vertexUVs, attr.barycentrics);

    // Material 
    FMaterial material = MeshMaterial::GetMaterial(g_hitgroupConstants.materialIndex, globalMaterialBufferIndex);
    FMaterialProperties p = EvaluateMaterialProperties(material, uv);

#if LIGHTING_ONLY
    p.basecolor = 0.5.xxx;
#endif

    N = normalize(mul(p.normalmap, TBN));

    float3 L = normalize(float3(1, 1, -1));
    float3 H = normalize(N + L);
    float3 V = normalize(g_globalConstants.cameraPosition - hitPosition);

    float NoV = saturate(dot(N, V));
    float NoL = saturate(dot(N, L));
    float NoH = saturate(dot(N, H));
    float LoH = saturate(dot(L, H));

    // Remapping
    float3 F0 = p.metallic * p.basecolor + (1.f - p.metallic) * 0.04;
    float3 albedo = (1.f - p.metallic) * p.basecolor;
    float roughness = p.roughness * p.roughness;

    float D = D_GGX(NoH, roughness);
    float3 F = F_Schlick(LoH, F0);
    float G = G_Smith_Direct(NoV, NoL, roughness);

    // Specular BRDF
    float3 Fr = (D * F * G) / (4.f * NoV * NoL);

    // diffuse BRDF
    float3 Fd = albedo * Fd_Lambert();
    // Apply direct lighting
    const float lightIntensity = 100000.f;
    float illuminance = lightIntensity * NoL;
    float3 luminance = 0.f;

    luminance += (Fr + (1.f - F) * Fd) * illuminance;

    payload.color = float4(luminance, 0.f);
}

[shader("miss")]
void msMain(inout RayPayload payload)
{
    TextureCube envmap = ResourceDescriptorHeap[g_globalConstants.envmapTextureIndex];
    payload.color = float4(envmap.SampleLevel(g_envmapSampler, WorldRayDirection(), 0).rgb, 0.f);
}


