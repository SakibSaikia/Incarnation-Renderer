#include "raytracing/common.hlsli"
#include "lighting/common.hlsli"

#define MAX_RECURSION_DEPTH 8

GlobalRootSignature k_globalRootsig =
{
    "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED )," \
    "CBV(b0), " \
    "SRV(t0), " \
    "StaticSampler(s0, space = 1, filter = FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP) "
};

TriangleHitGroup k_hitGroup =
{
    "",                                 // AnyHit Shader
    "chsMain",                          // ClosestHit Shader
};

TriangleHitGroup k_shadowHitGroup =
{
    "ahsShadow",                        // AnyHit Shader
    "",                                 // ClosestHit Shader
};


RaytracingShaderConfig  k_shaderConfig =
{
    24,                                 // max payload size
    8                                   // max attribute size
};

RaytracingPipelineConfig k_pipelineConfig =
{
    MAX_RECURSION_DEPTH                 // max trace recursion depth
};

struct RayPayload
{
    float4 color;
    int pathLength;
    float attenuation;
};

struct ShadowRayPayload
{
    bool hit;
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
    float4x4 sceneRotation;
    int envmapTextureIndex;
    int scenePrimitivesIndex;
    int scenePrimitiveCountsIndex;
    int whiteNoiseTextureIndex;
    int whiteNoiseArrayIndex;
    int whiteNoiseTextureSize;
};

ConstantBuffer<GlobalCbLayout> g_globalConstants : register(b0);
SamplerState g_envmapSampler : register(s0, space1);
RaytracingAccelerationStructure g_sceneBvh : register(t0);

[shader("raygeneration")]
void rgsMain()
{
    RWTexture2D<float4> destUav = ResourceDescriptorHeap[g_globalConstants.destUavIndex];

    RayDesc ray = GenerateCameraRay(DispatchRaysIndex().xy, g_globalConstants.cameraPosition, g_globalConstants.projectionToWorld);
    RayPayload payload;
    payload.color = float4(0, 0, 0, 0);
    payload.pathLength = 0;
    payload.attenuation = 1.f;

    // MultiplierForGeometryContributionToHitGroupIndex is explicitly set to 0 because we are using GeometryIndex() to directly index primitive data instead of using hit group records.
    TraceRay(g_sceneBvh, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, ~0, 0, 0, 0, ray, payload);
    destUav[DispatchRaysIndex().xy] = payload.color;
}

[shader("closesthit")]
void chsMain(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    const int globalMeshAccessorsIndex = g_globalConstants.sceneMeshAccessorsIndex;
    const int globalMeshBufferViewsIndex = g_globalConstants.sceneMeshBufferViewsIndex;
    const int globalMaterialBufferIndex = g_globalConstants.sceneMaterialBufferIndex;

    const FGpuPrimitive primitive = MeshMaterial::GetPrimitive(InstanceIndex(), GeometryIndex(), g_globalConstants.scenePrimitivesIndex, g_globalConstants.scenePrimitiveCountsIndex);

    float4x4 localToWorld = mul(primitive.m_localToWorld, g_globalConstants.sceneRotation);

    float3 hitPosition = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();

    // Get the base index of the hit triangle
    uint baseIndex = PrimitiveIndex() * primitive.m_indicesPerTriangle;

    // Load up 3 indices for the triangle
    const uint3 indices = MeshMaterial::GetUint3(baseIndex, primitive.m_indexAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex);

    // Tangent bases
    float3 vertexNormals[3] = 
    {
        MeshMaterial::GetFloat3(indices.x, primitive.m_normalAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex),
        MeshMaterial::GetFloat3(indices.y, primitive.m_normalAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex),
        MeshMaterial::GetFloat3(indices.z, primitive.m_normalAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex)
    };

    float4 vertexTangents[3] =
    {
        MeshMaterial::GetFloat4(indices.x, primitive.m_tangentAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex),
        MeshMaterial::GetFloat4(indices.y, primitive.m_tangentAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex),
        MeshMaterial::GetFloat4(indices.z, primitive.m_tangentAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex)
    };

    float4 packedT = HitAttribute(vertexTangents, attr.barycentrics);
    float3 N = normalize(HitAttribute(vertexNormals, attr.barycentrics)); 
    float3 T = normalize(packedT.xyz);
    float3 B = cross(N, T) * packedT.w;

    N = mul(float4(N, 0.f), localToWorld).xyz;
    T = mul(float4(T, 0.f), localToWorld).xyz;
    B = mul(float4(B, 0.f), localToWorld).xyz;
    float3x3 TBN = float3x3(T, B, N);

    // UVs
    float2 vertexUVs[3] =
    {
        MeshMaterial::GetFloat2(indices.x, primitive.m_uvAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex),
        MeshMaterial::GetFloat2(indices.y, primitive.m_uvAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex),
        MeshMaterial::GetFloat2(indices.z, primitive.m_uvAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex)
    };

    float2 uv = HitAttribute(vertexUVs, attr.barycentrics);

    // Material 
    FMaterial material = MeshMaterial::GetMaterial(primitive.m_materialIndex, globalMaterialBufferIndex);
    FMaterialProperties matInfo = EvaluateMaterialProperties(material, uv);

#if LIGHTING_ONLY
    matInfo.basecolor = 0.5.xxx;
#endif

    N = normalize(mul(matInfo.normalmap, TBN));

    float3 V = normalize(g_globalConstants.cameraPosition - hitPosition);
    float3 H = normalize(N + V);

    float NoV = saturate(dot(N, V));
    float NoH = saturate(dot(N, H));
    float VoH = saturate(dot(V, H));
    float3 F0 = matInfo.metallic * matInfo.basecolor + (1.f - matInfo.metallic) * 0.04;
    float3 albedo = (1.f - matInfo.metallic) * matInfo.basecolor;
    float roughness = matInfo.roughness;

    float D = D_GGX(NoH, roughness);
    float3 F = F_Schlick(VoH, F0);

#if DIRECT_LIGHTING
    FLight sun;
    sun.type = Light::Directional;
    sun.positionOrDirection = normalize(float3(1, 1, -1));
    sun.intensity = 100000.f;
    sun.shadowcasting = true;

    payload.pathLength += 1;
    payload.color.xyz += payload.attenuation * GetDirectRadiance(sun, hitPosition, albedo, roughness, N, D, F, NoV, NoH, VoH, g_sceneBvh);

    if (payload.pathLength < MAX_RECURSION_DEPTH)
    {
        Texture2DArray whiteNoiseTex = ResourceDescriptorHeap[g_globalConstants.whiteNoiseTextureIndex];
        int2 texelIndex = DispatchRaysIndex().xy % g_globalConstants.whiteNoiseTextureSize;
        float randomNoise = whiteNoiseTex.Load(int4(texelIndex.x, texelIndex.y, g_globalConstants.whiteNoiseArrayIndex, 0)).r;

        // The secondary bounce ray has reduced contribution to the output radiance as determined by the attenuation
        float outAttenuation = 1.f;
        RayDesc secondaryRay = GenerateIndirectRadianceRay(randomNoise, hitPosition, N, outAttenuation);
        payload.attenuation *= outAttenuation;

        TraceRay(g_sceneBvh, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, ~0, 0, 0, 0, secondaryRay, payload);
    }
#endif
}

[shader("miss")]
void msMain(inout RayPayload payload)
{
    TextureCube envmap = ResourceDescriptorHeap[g_globalConstants.envmapTextureIndex];
    payload.color = payload.attenuation * float4(envmap.SampleLevel(g_envmapSampler, WorldRayDirection(), 0).rgb, 0.f);
}

[shader("anyhit")]
void ahsShadow(inout ShadowRayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    // TODO: Evaluate alpha mask before registering hit
    payload.hit = true;
    AcceptHitAndEndSearch();
}

[shader("miss")]
void msShadow(inout ShadowRayPayload payload)
{
    payload.hit = false;
}


