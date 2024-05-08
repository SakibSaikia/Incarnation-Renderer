// Reference - http://filmicworlds.com/blog/visibility-buffer-rendering-with-material-graphs/

#include "common/mesh-material.hlsli"
#include "material/common.hlsli"
#include "encoding.hlsli"

#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
#define THREAD_GROUP_SIZE_Y 1
#endif

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=5)," \
    "CBV(b1)," \
    "CBV(b2)," \
    "StaticSampler(s0, filter = FILTER_ANISOTROPIC, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE)"

// See: http://filmicworlds.com/blog/visibility-buffer-rendering-with-material-graphs/
float3 CalcBarycentrics(float4 p0, float4 p1, float4 p2, float2 pixelNdc, float2 res)
{
    float3 invW = rcp(float3(p0.w, p1.w, p2.w));

    float2 ndc0 = p0.xy * invW.x;
    float2 ndc1 = p1.xy * invW.y;
    float2 ndc2 = p2.xy * invW.z;

    float invDet = rcp(determinant(float2x2(ndc2 - ndc1, ndc0 - ndc1)));
    float3 ddx = float3(ndc1.y - ndc2.y, ndc2.y - ndc0.y, ndc0.y - ndc1.y) * invDet * invW;
    float3 ddy = float3(ndc2.x - ndc1.x, ndc0.x - ndc2.x, ndc1.x - ndc0.x) * invDet * invW;
    float ddxSum = dot(ddx, float3(1, 1, 1));
    float ddySum = dot(ddy, float3(1, 1, 1));

    float2 deltaVec = pixelNdc - ndc0;
    float interpInvW = invW.x + deltaVec.x * ddxSum + deltaVec.y * ddySum;
    float interpW = rcp(interpInvW);

    float3 lambda;
    lambda.x = interpW * (invW[0] + deltaVec.x * ddx.x + deltaVec.y * ddy.x);
    lambda.y = interpW * (0.0f + deltaVec.x * ddx.y + deltaVec.y * ddy.y);
    lambda.z = interpW * (0.0f + deltaVec.x * ddx.z + deltaVec.y * ddy.z);

    return lambda;
}

float3 BarycentricInterp(float3 p0, float3 p1, float3 p2, float3 w)
{
    float3x3 p = float3x3(p0, p1, p2);
    return mul(w, p);
}

float2 BarycentricInterp(float2 p0, float2 p1, float2 p2, float3 w)
{
    float3x2 p = float3x2(p0, p1, p2);
    return mul(w, p);
}

float BarycentricInterp(float p0, float p1, float p2, float3 w)
{
    float3 p = float3(p0, p1, p2);
    return dot(w, p);
}

// Properties at each triangle vertex. This corresponds to the interpolants in a VS->PS pipeline
struct FVertexData
{
    float3 m_position;
    float2 m_uv;
    float3 m_normal;
    float4 m_tangentAndSign;
};

// Vertex properties for each vert of the triangle
struct FTriangleData
{
    FVertexData m_vertices[3];
};

struct FPassConstants
{
    uint m_gbuffer0UavIndex;
    uint m_gbuffer1UavIndex;
    uint m_gbuffer2UavIndex;
    uint m_visBufferSrvIndex;
    uint m_colorTargetUavIndex;
};

ConstantBuffer<FPassConstants> g_passCb : register(b0);
ConstantBuffer<FViewConstants> g_viewCb : register(b1);
ConstantBuffer<FSceneConstants> g_sceneCb : register(b2);
SamplerState g_anisoSampler : register(s0);

FTriangleData GetPrimitiveTriangleData(int triIndex, FGpuPrimitive primitive)
{
    FTriangleData o;

    // Use triangle id to retrieve the vertex indices of the triangle
    uint baseTriIndex = triIndex * 3;
    const uint3 vertIndices = MeshMaterial::GetUint3(baseTriIndex, primitive.m_indexAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);

    o.m_vertices[0].m_position = MeshMaterial::GetFloat3(vertIndices.x, primitive.m_positionAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.m_vertices[1].m_position = MeshMaterial::GetFloat3(vertIndices.y, primitive.m_positionAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.m_vertices[2].m_position = MeshMaterial::GetFloat3(vertIndices.z, primitive.m_positionAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);

    o.m_vertices[0].m_uv = MeshMaterial::GetFloat2(vertIndices.x, primitive.m_uvAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.m_vertices[1].m_uv = MeshMaterial::GetFloat2(vertIndices.y, primitive.m_uvAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.m_vertices[2].m_uv = MeshMaterial::GetFloat2(vertIndices.z, primitive.m_uvAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);

    o.m_vertices[0].m_normal = MeshMaterial::GetFloat3(vertIndices.x, primitive.m_normalAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.m_vertices[1].m_normal = MeshMaterial::GetFloat3(vertIndices.y, primitive.m_normalAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.m_vertices[2].m_normal = MeshMaterial::GetFloat3(vertIndices.z, primitive.m_normalAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);

    o.m_vertices[0].m_tangentAndSign = MeshMaterial::GetFloat4(vertIndices.x, primitive.m_tangentAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.m_vertices[1].m_tangentAndSign = MeshMaterial::GetFloat4(vertIndices.y, primitive.m_tangentAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.m_vertices[2].m_tangentAndSign = MeshMaterial::GetFloat4(vertIndices.z, primitive.m_tangentAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);

    return o;
}

FTriangleData GetMeshletTriangleData(int meshletTriangleIndex, FGpuMeshlet meshlet)
{
    FTriangleData o;

    ByteAddressBuffer packedTriangleIndexBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedMeshletPrimitiveIndexBufferIndex];
    uint packedMeshletTriangleIndex = packedTriangleIndexBuffer.Load<uint>((meshlet.m_triangleBegin + meshletTriangleIndex) * sizeof(uint));
    uint meshletTriVertIndices[] =
    {
        0xff & (packedMeshletTriangleIndex >> 20),
        0xff & (packedMeshletTriangleIndex >> 10),
        0xff & (packedMeshletTriangleIndex)
    };
	
	// Compute the unique vertex index for the current meshlet vert
    ByteAddressBuffer packedVertexIndexBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedMeshletVertexIndexBufferIndex];
    uint uniqueVertIndices[] = 
    {
        packedVertexIndexBuffer.Load<uint>((meshlet.m_vertexBegin + meshletTriVertIndices[0]) * sizeof(uint)),
        packedVertexIndexBuffer.Load<uint>((meshlet.m_vertexBegin + meshletTriVertIndices[1]) * sizeof(uint)),
        packedVertexIndexBuffer.Load<uint>((meshlet.m_vertexBegin + meshletTriVertIndices[2]) * sizeof(uint))
    };

    o.m_vertices[0].m_position = MeshMaterial::GetFloat3(uniqueVertIndices[0], meshlet.m_positionAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.m_vertices[1].m_position = MeshMaterial::GetFloat3(uniqueVertIndices[1], meshlet.m_positionAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.m_vertices[2].m_position = MeshMaterial::GetFloat3(uniqueVertIndices[2], meshlet.m_positionAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);

    o.m_vertices[0].m_uv = MeshMaterial::GetFloat2(uniqueVertIndices[0], meshlet.m_uvAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.m_vertices[1].m_uv = MeshMaterial::GetFloat2(uniqueVertIndices[1], meshlet.m_uvAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.m_vertices[2].m_uv = MeshMaterial::GetFloat2(uniqueVertIndices[2], meshlet.m_uvAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);

    o.m_vertices[0].m_normal = MeshMaterial::GetFloat3(uniqueVertIndices[0], meshlet.m_normalAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.m_vertices[1].m_normal = MeshMaterial::GetFloat3(uniqueVertIndices[1], meshlet.m_normalAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.m_vertices[2].m_normal = MeshMaterial::GetFloat3(uniqueVertIndices[2], meshlet.m_normalAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);

    o.m_vertices[0].m_tangentAndSign = MeshMaterial::GetFloat4(uniqueVertIndices[0], meshlet.m_tangentAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.m_vertices[1].m_tangentAndSign = MeshMaterial::GetFloat4(uniqueVertIndices[1], meshlet.m_tangentAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);
    o.m_vertices[2].m_tangentAndSign = MeshMaterial::GetFloat4(uniqueVertIndices[2], meshlet.m_tangentAccessor, g_sceneCb.m_sceneMeshAccessorsIndex, g_sceneCb.m_sceneMeshBufferViewsIndex);

    return o;
}

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x < g_viewCb.m_resX && dispatchThreadId.y < g_viewCb.m_resY)
    {
        Texture2D<uint> visBufferTex = ResourceDescriptorHeap[g_passCb.m_visBufferSrvIndex];
        RWTexture2D<float4> gbufferBaseColor = ResourceDescriptorHeap[g_passCb.m_gbuffer0UavIndex];
        RWTexture2D<float2> gbufferNormals = ResourceDescriptorHeap[g_passCb.m_gbuffer1UavIndex];
        RWTexture2D<float4> gbufferMetallicRoughnessAo = ResourceDescriptorHeap[g_passCb.m_gbuffer2UavIndex];
        RWTexture2D<float3> colorTarget = ResourceDescriptorHeap[g_passCb.m_colorTargetUavIndex];
        
        ByteAddressBuffer meshTransformsBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedSceneMeshTransformsBufferIndex];

        int visBufferValue = visBufferTex[dispatchThreadId.xy];
        if (visBufferValue != 0xFFFE0000)
        {
#if USING_MESHLETS
             // Retrieve meshlet id and triangle id from vis buffer
            uint meshletId, triangleId;
            DecodeMeshletVisibility(visBufferValue, meshletId, triangleId);

            // Use meshlet id to retrieve the meshlet info
            ByteAddressBuffer packedMeshletsBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedSceneMeshletsBufferIndex];
            const FGpuMeshlet meshlet = packedMeshletsBuffer.Load<FGpuMeshlet>(meshletId * sizeof(FGpuMeshlet));

            // Fill the vertex data for the triangle
            FTriangleData tri = GetMeshletTriangleData(triangleId, meshlet);
            
            float4x4 localToWorld = meshTransformsBuffer.Load<float4x4>(meshlet.m_meshIndex * sizeof(float4x4));
            FMaterial material = MeshMaterial::GetMaterial(meshlet.m_materialIndex, g_sceneCb.m_sceneMaterialBufferIndex);
            
#else
            // Retrieve primitive id and triangle id from vis buffer
            uint primitiveId, triangleId;
            DecodePrimitiveVisibility(visBufferValue, primitiveId, triangleId);

            // Use object id to retrieve the primitive info
            ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedScenePrimitivesBufferIndex];
            const FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(primitiveId * sizeof(FGpuPrimitive));

            // Fill the vertex data for the triangle
            FTriangleData tri = GetPrimitiveTriangleData(triangleId, primitive);
            
            float4x4 localToWorld = meshTransformsBuffer.Load<float4x4>(primitive.m_meshIndex * sizeof(float4x4));
            FMaterial material = MeshMaterial::GetMaterial(primitive.m_materialIndex, g_sceneCb.m_sceneMaterialBufferIndex);
#endif

            // Transform the triangle verts to ndc space
            localToWorld = mul(localToWorld, g_sceneCb.m_sceneRotation);
            float4x4 localToClip = mul(localToWorld, g_viewCb.m_viewProjTransform);
            float4 p[3] = {
                mul(float4(tri.m_vertices[0].m_position, 1.f), localToClip),
                mul(float4(tri.m_vertices[1].m_position, 1.f), localToClip),
                mul(float4(tri.m_vertices[2].m_position, 1.f), localToClip)
            };

            // Calculate screen space barycentrics based on pixel NDC
            float2 screenRes = float2(g_viewCb.m_resX, g_viewCb.m_resY);
            float2 pixelNdc = (dispatchThreadId.xy + 0.5.xx) / screenRes;
            pixelNdc.x = 2.f * pixelNdc.x - 1.f;
            pixelNdc.y = -2.f * pixelNdc.y + 1;
            float3 lambda = CalcBarycentrics(
                p[0],
                p[1],
                p[2],
                pixelNdc,
                screenRes);

            float3 N = normalize(BarycentricInterp(tri.m_vertices[0].m_normal, tri.m_vertices[1].m_normal, tri.m_vertices[2].m_normal, lambda));
            float3 T = normalize(BarycentricInterp(tri.m_vertices[0].m_tangentAndSign.xyz, tri.m_vertices[1].m_tangentAndSign.xyz, tri.m_vertices[2].m_tangentAndSign.xyz, lambda));
            float3 B = normalize(cross(N, T) * tri.m_vertices[0].m_tangentAndSign.w);
            float3x3 tangentToWorld = mul(float3x3(T, B, N), (float3x3)localToWorld);

            float2 UV = BarycentricInterp(tri.m_vertices[0].m_uv, tri.m_vertices[1].m_uv, tri.m_vertices[2].m_uv, lambda);

            // Evaluate Material
            FMaterialProperties matInfo = EvaluateMaterialProperties(material, UV, g_anisoSampler);

            if (matInfo.bHasNormalmap)
            {
                N = normalize(mul(matInfo.normalmap, tangentToWorld));
            }

            colorTarget[dispatchThreadId.xy] = matInfo.emissive * 20000.f;
            gbufferBaseColor[dispatchThreadId.xy] = float4(matInfo.basecolor, 0.f);
            gbufferNormals[dispatchThreadId.xy] = OctEncode(N);
            gbufferMetallicRoughnessAo[dispatchThreadId.xy] = float4(matInfo.metallic, matInfo.roughness, matInfo.ao, matInfo.aoblend);
        }
        else
        {
            gbufferBaseColor[dispatchThreadId.xy] = 0;
            gbufferNormals[dispatchThreadId.xy] = 0;
            gbufferMetallicRoughnessAo[dispatchThreadId.xy] = 0;
        }
    }
}