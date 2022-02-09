#include "raytracing/common.hlsli"
#include "lighting/common.hlsli"

#define MAX_RECURSION_DEPTH 4

GlobalRootSignature k_globalRootsig =
{
    "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED )," \
    "CBV(b0), " \
    "SRV(t0), " \
    "StaticSampler(s0, space = 1, filter = FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP), " \
    "StaticSampler(s1, space = 1, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE)"
};

TriangleHitGroup k_hitGroup =
{
    "ahsMain",                          // AnyHit Shader
    "chsMain",                          // ClosestHit Shader
};

TriangleHitGroup k_shadowHitGroup =
{
    "ahsShadow",                        // AnyHit Shader
    "",                                 // ClosestHit Shader
};


RaytracingShaderConfig  k_shaderConfig =
{
    40,                                 // max payload size
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
    float3 attenuation;
    uint pixelIndex;
    uint sampleSetIndex;
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
    float cameraAperture;
    float cameraFocalLength;
    int lightCount;
    int destUavIndex;
    float4x4 projectionToWorld;
    float4x4 sceneRotation;
    float4x4 cameraMatrix;
    int envmapTextureIndex;
    int scenePrimitivesIndex;
    int scenePrimitiveCountsIndex;
    uint currentSampleIndex;
    uint sqrtSampleCount;
    int sceneLightPropertiesBufferIndex;
    int sceneLightIndicesBufferIndex;
    int sceneLightsTransformsBufferIndex;
};

ConstantBuffer<GlobalCbLayout> g_globalConstants : register(b0);
SamplerState g_envmapSampler : register(s0, space1);
SamplerState g_trilinearSampler : register(s1, space1);
RaytracingAccelerationStructure g_sceneBvh : register(t0);

[shader("raygeneration")]
void rgsMain()
{
    RWTexture2D<float4> destUav = ResourceDescriptorHeap[g_globalConstants.destUavIndex];

    uint raygenSampleSetIdx = 0;
    const uint sampleIdx = g_globalConstants.currentSampleIndex;
    const uint2 pixelCoord = DispatchRaysIndex().xy;
    const uint pixelIdx = pixelCoord.y * DispatchRaysDimensions().x + pixelCoord.x;
    float2 subPixelJitter = SamplePoint(pixelIdx, sampleIdx, raygenSampleSetIdx, g_globalConstants.sqrtSampleCount);
    float2 apertureSample = SamplePoint(pixelIdx, sampleIdx, raygenSampleSetIdx, g_globalConstants.sqrtSampleCount); // Note: SamplePoint modifies the SampleSetIndex

    RayDesc ray = GenerateCameraRay(
        DispatchRaysIndex().xy + subPixelJitter, 
        g_globalConstants.cameraMatrix, 
        g_globalConstants.projectionToWorld, 
        g_globalConstants.cameraAperture,
        g_globalConstants.cameraFocalLength,
        apertureSample);

    RayPayload payload;
    payload.color = float4(0, 0, 0, 0);
    payload.pathLength = 0;
    payload.attenuation = float3(1.f, 1.f, 1.f);
    payload.pixelIndex = pixelIdx;
    payload.sampleSetIndex = 0;

    // MultiplierForGeometryContributionToHitGroupIndex is explicitly set to 0 because we are using GeometryIndex() to directly index primitive data instead of using hit group records.
    TraceRay(g_sceneBvh, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, ~0, 0, 0, 0, ray, payload);
    destUav[DispatchRaysIndex().xy] = payload.color;
}

[shader("closesthit")]
void chsMain(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.pathLength += 1;
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
    float3x3 tangentToWorld = float3x3(T, B, N);

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
    FMaterialProperties matInfo = EvaluateMaterialProperties(material, uv, g_trilinearSampler);

#if VIEWMODE == 1 // Lighting Only
    matInfo.basecolor = 0.5.xxx;
#elif VIEWMODE == 2 // Roughness
    payload.color = matInfo.roughness.xxxx;
    return;
#elif VIEWMODE == 3 // Metallic
    payload.color = matInfo.metallic.xxxx;
    return;
#elif VIEWMODE == 4 // Base Color
    payload.color = float4(matInfo.basecolor, 1.f);
    return;
#elif VIEWMODE == 5 // Emissive
    payload.color = float4(matInfo.emissive, 1.f);
    return;
#endif

    // Emissive contribution. 
    // Emissive surfaces should not scatter - modulate scattering based on emissive value.
    payload.color.xyz += payload.attenuation * matInfo.emissive * 20000;
    payload.attenuation *= saturate(1.f - matInfo.emissive);

    if (matInfo.bHasNormalmap)
    {
        N = normalize(mul(matInfo.normalmap, tangentToWorld));
    }

    // For primary ray, this is the ray pointing back at the viewer.
    // For secondary rays, this points back to the origin of the bounce (which is the viewer for that ray)
    float3 V = -WorldRayDirection();

#if DIRECT_LIGHTING
    ByteAddressBuffer lightIndicesBuffer = ResourceDescriptorHeap[g_globalConstants.sceneLightIndicesBufferIndex];
    ByteAddressBuffer lightPropertiesBuffer = ResourceDescriptorHeap[g_globalConstants.sceneLightPropertiesBufferIndex];
    ByteAddressBuffer lightTransformsBuffer = ResourceDescriptorHeap[g_globalConstants.sceneLightsTransformsBufferIndex];
    for (int lightIndex = 0; lightIndex < g_globalConstants.lightCount; ++lightIndex)
    {
        int lightId = lightIndicesBuffer.Load<int>(lightIndex * sizeof(int));
        FLight light = lightPropertiesBuffer.Load<FLight>(lightId * sizeof(FLight));
        float4x4 lightTransform = lightTransformsBuffer.Load<float4x4>(lightId * sizeof(float4x4));
        payload.color.xyz += payload.attenuation * GetDirectRadiance(light, lightTransform, hitPosition, matInfo, N, V, g_sceneBvh);
    }
#endif

    if (payload.pathLength < MAX_RECURSION_DEPTH)
    {
        // The secondary bounce ray has reduced contribution to the output radiance as determined by the attenuation
        float3 outAttenuation;
        RayDesc secondaryRay = GenerateIndirectRadianceRay(
            hitPosition, 
            N,
            V,
            matInfo, 
            tangentToWorld, 
            payload.pixelIndex, 
            g_globalConstants.currentSampleIndex, 
            payload.sampleSetIndex,
            g_globalConstants.sqrtSampleCount,
            outAttenuation);

        payload.attenuation *= outAttenuation;
        if (any(payload.attenuation) > 0.001)
        {
            TraceRay(g_sceneBvh, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, ~0, 0, 0, 0, secondaryRay, payload);
        }
    }
}

[shader("anyhit")]
void ahsMain(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    const int globalMeshAccessorsIndex = g_globalConstants.sceneMeshAccessorsIndex;
    const int globalMeshBufferViewsIndex = g_globalConstants.sceneMeshBufferViewsIndex;
    const int globalMaterialBufferIndex = g_globalConstants.sceneMaterialBufferIndex;

    const FGpuPrimitive primitive = MeshMaterial::GetPrimitive(InstanceIndex(), GeometryIndex(), g_globalConstants.scenePrimitivesIndex, g_globalConstants.scenePrimitiveCountsIndex);

    uint baseIndex = PrimitiveIndex() * primitive.m_indicesPerTriangle;
    const uint3 indices = MeshMaterial::GetUint3(baseIndex, primitive.m_indexAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex);

    float2 vertexUVs[3] =
    {
        MeshMaterial::GetFloat2(indices.x, primitive.m_uvAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex),
        MeshMaterial::GetFloat2(indices.y, primitive.m_uvAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex),
        MeshMaterial::GetFloat2(indices.z, primitive.m_uvAccessor, globalMeshAccessorsIndex, globalMeshBufferViewsIndex)
    };

    float2 uv = HitAttribute(vertexUVs, attr.barycentrics);
    FMaterial material = MeshMaterial::GetMaterial(primitive.m_materialIndex, globalMaterialBufferIndex);

    // Alpha test
    if (material.m_baseColorTextureIndex != -1)
    {
        Texture2D baseColorTex = ResourceDescriptorHeap[material.m_baseColorTextureIndex];
        float alpha = baseColorTex.SampleLevel(g_trilinearSampler, uv, 0).a;
        if (alpha < 0.5f)
        {
            IgnoreHit();
        }
    }
}

[shader("miss")]
void msMain(inout RayPayload payload)
{
    TextureCube envmap = ResourceDescriptorHeap[g_globalConstants.envmapTextureIndex];
    payload.color.rgb += payload.attenuation * envmap.SampleLevel(g_envmapSampler, WorldRayDirection(), 0).rgb;
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


