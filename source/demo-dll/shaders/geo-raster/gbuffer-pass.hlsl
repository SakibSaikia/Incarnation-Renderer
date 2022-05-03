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
    "CBV(b0)," \
    "StaticSampler(s0, filter = FILTER_COMPARISON_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP)"

struct FBarycentrics
{
    float3 m_lambda;
    float3 m_ddx;
    float3 m_ddy;
};

// See: http://filmicworlds.com/blog/visibility-buffer-rendering-with-material-graphs/
FBarycentrics CalcBarycentrics(float4 p0, float4 p1, float4 p2, float2 pixelNdc, float2 res)
{
    FBarycentrics o = (FBarycentrics)0;

    float3 invW = rcp(float3(p0.w, p1.w, p2.w));

    float2 ndc0 = p0.xy * invW.x;
    float2 ndc1 = p1.xy * invW.y;
    float2 ndc2 = p2.xy * invW.z;

    float invDet = rcp(determinant(float2x2(ndc2 - ndc1, ndc0 - ndc1)));
    o.m_ddx = float3(ndc1.y - ndc2.y, ndc2.y - ndc0.y, ndc0.y - ndc1.y) * invDet;
    o.m_ddy = float3(ndc2.x - ndc1.x, ndc0.x - ndc2.x, ndc1.x - ndc0.x) * invDet;

    float2 deltaVec = pixelNdc - ndc0;
    float interpInvW = (invW.x + deltaVec.x * dot(invW, o.m_ddx) + deltaVec.y * dot(invW, o.m_ddy));
    float interpW = rcp(interpInvW);

    o.m_lambda.x = interpW * (invW[0] + deltaVec.x * o.m_ddx.x * invW[0] + deltaVec.y * o.m_ddy.x * invW[0]);
    o.m_lambda.y = interpW * (0.0f + deltaVec.x * o.m_ddx.y * invW[1] + deltaVec.y * o.m_ddy.y * invW[1]);
    o.m_lambda.z = interpW * (0.0f + deltaVec.x * o.m_ddx.z * invW[2] + deltaVec.y * o.m_ddy.z * invW[2]);

    // The derivatives of the barycentric are scaled by 2/winSize to change the scale from NDC units (-1 to 1) to pixel units
    o.m_ddx *= (2.f / res.x);
    o.m_ddy *= (2.f / res.y);

    // NDC is bottom to top whereas window co-ordinates are top to bottom
    o.m_ddy *= -1.0f;

    return o;
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

float BarycentricDeriv(float p0, float p1, float p2, float3 w)
{
    float3 p = float3(p0, p1, p2);
    return dot(w * p, 1.xxx);
}

float2 BarycentricDeriv(float2 p0, float2 p1, float2 p2, float3 w)
{
    float3 pp1 = float3(p0.x, p1.x, p2.x);
    float3 pp2 = float3(p0.y, p1.y, p2.y);
    
    return float2(dot(w * pp1, 1.xxx), dot(w * pp2, 1.xxx));
}

float3 BarycentricDeriv(float3 p0, float3 p1, float3 p2, float3 w)
{
    float3 pp1 = float3(p0.x, p1.x, p2.x);
    float3 pp2 = float3(p0.y, p1.y, p2.y);
    float3 pp3 = float3(p0.z, p1.z, p2.z);

    return float3(dot(w * pp1, 1.xxx), dot(w * pp2, 1.xxx), dot(w * pp3, 1.xxx));
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

cbuffer cb : register(b0)
{
    uint gbuffer0UavIndex;
    uint gbuffer1UavIndex;
    uint gbuffer2UavIndex;
    uint visBufferSrvIndex;
    int sceneMeshAccessorsIndex;
    int sceneMeshBufferViewsIndex;
    int sceneMaterialBufferIndex;
    int scenePrimitivesIndex;
    uint resX;
    uint resY;
    float2 __pad;
    float4x4 viewProjTransform;
    float4x4 sceneRotation;
};

SamplerState g_trilinearSampler : register(s0);

FTriangleData GetTriangleData(int triIndex, FGpuPrimitive primitive)
{
    FTriangleData o;

    // Use triangle id to retrieve the vertex indices of the triangle
    uint baseTriIndex = triIndex * 3;
    const uint3 vertIndices = MeshMaterial::GetUint3(baseTriIndex, primitive.m_indexAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex);

    o.m_vertices[0].m_position = MeshMaterial::GetFloat3(vertIndices.x, primitive.m_positionAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex);
    o.m_vertices[1].m_position = MeshMaterial::GetFloat3(vertIndices.y, primitive.m_positionAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex);
    o.m_vertices[2].m_position = MeshMaterial::GetFloat3(vertIndices.z, primitive.m_positionAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex);

    o.m_vertices[0].m_uv = MeshMaterial::GetFloat2(vertIndices.x, primitive.m_uvAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex);
    o.m_vertices[1].m_uv = MeshMaterial::GetFloat2(vertIndices.y, primitive.m_uvAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex);
    o.m_vertices[2].m_uv = MeshMaterial::GetFloat2(vertIndices.z, primitive.m_uvAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex);

    o.m_vertices[0].m_normal = MeshMaterial::GetFloat3(vertIndices.x, primitive.m_normalAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex);
    o.m_vertices[1].m_normal = MeshMaterial::GetFloat3(vertIndices.y, primitive.m_normalAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex);
    o.m_vertices[2].m_normal = MeshMaterial::GetFloat3(vertIndices.z, primitive.m_normalAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex);

    o.m_vertices[0].m_tangentAndSign = MeshMaterial::GetFloat4(vertIndices.x, primitive.m_tangentAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex);
    o.m_vertices[1].m_tangentAndSign = MeshMaterial::GetFloat4(vertIndices.y, primitive.m_tangentAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex);
    o.m_vertices[2].m_tangentAndSign = MeshMaterial::GetFloat4(vertIndices.z, primitive.m_tangentAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex);

    return o;
}

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x < resX && dispatchThreadId.y < resY)
    {
        Texture2D<uint> visBufferTex = ResourceDescriptorHeap[visBufferSrvIndex];
        RWTexture2D<float4> gbufferBaseColor = ResourceDescriptorHeap[gbuffer0UavIndex];
        RWTexture2D<float4> gbufferNormals = ResourceDescriptorHeap[gbuffer1UavIndex];
        RWTexture2D<float4> gbufferMetallicRoughnessAo = ResourceDescriptorHeap[gbuffer2UavIndex];

        int visBufferValue = visBufferTex[dispatchThreadId.xy];
        if (visBufferValue != 0xFFFE0000)
        {
            // Retrieve object and triangle id for vis buffer
            uint objectId, triangleId;
            DecodeVisibilityBuffer(visBufferValue, objectId, triangleId);

            // Use object id to retrieve the primitive info
            ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[scenePrimitivesIndex];
            const FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(objectId * sizeof(FGpuPrimitive));

            // Fill the vertex data for the triangle
            FTriangleData tri = GetTriangleData(triangleId, primitive);

            // Transform the triangle verts to ndc space
            float4x4 localToWorld = mul(primitive.m_localToWorld, sceneRotation);
            float4x4 localToClip = mul(localToWorld, viewProjTransform);
            float4 p[3] = {
                mul(float4(tri.m_vertices[0].m_position, 1.f), localToClip),
                mul(float4(tri.m_vertices[1].m_position, 1.f), localToClip),
                mul(float4(tri.m_vertices[2].m_position, 1.f), localToClip)
            };

            // Calculate screen space barycentrics based on pixel NDC
            float2 screenRes = float2(resX, resY);
            float2 pixelNdc = (dispatchThreadId.xy + 0.5.xx) / screenRes;
            pixelNdc.x = 2.f * pixelNdc.x - 1.f;
            pixelNdc.y = -2.f * pixelNdc.y + 1;
            FBarycentrics w = CalcBarycentrics(
                p[0],
                p[1],
                p[2],
                pixelNdc,
                screenRes);

            float3 N = normalize(BarycentricInterp(tri.m_vertices[0].m_normal, tri.m_vertices[1].m_normal, tri.m_vertices[2].m_normal, w.m_lambda));
            float3 T = normalize(BarycentricInterp(tri.m_vertices[0].m_tangentAndSign.xyz, tri.m_vertices[1].m_tangentAndSign.xyz, tri.m_vertices[2].m_tangentAndSign.xyz, w.m_lambda));
            float3 B = normalize(cross(N, T) * tri.m_vertices[0].m_tangentAndSign.w);
            float3x3 tangentToWorld = mul(float3x3(T, B, N), (float3x3)localToWorld);

            float2 UV = BarycentricInterp(tri.m_vertices[0].m_uv, tri.m_vertices[1].m_uv, tri.m_vertices[2].m_uv, w.m_lambda);
            float2 ddxUV = BarycentricDeriv(tri.m_vertices[0].m_uv, tri.m_vertices[1].m_uv, tri.m_vertices[2].m_uv, w.m_ddx);
            float2 ddyUV = BarycentricDeriv(tri.m_vertices[0].m_uv, tri.m_vertices[1].m_uv, tri.m_vertices[2].m_uv, w.m_ddy);

            // Evaluate Material
            FMaterial material = MeshMaterial::GetMaterial(primitive.m_materialIndex, sceneMaterialBufferIndex);
            FMaterialProperties matInfo = EvaluateMaterialProperties(material, UV, g_trilinearSampler, ddxUV, ddyUV);

            N = normalize(mul(matInfo.normalmap, tangentToWorld));

            gbufferBaseColor[dispatchThreadId.xy] = float4(matInfo.basecolor, 0.f);
            gbufferNormals[dispatchThreadId.xy] = float4(N, 0.f);
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