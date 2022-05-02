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
    float p = float3(p0, p1, p2);
    return dot(w, p);
}

float BarycentricDeriv(float p0, float p1, float p2, float3 w)
{
    float p = float3(p0, p1, p2);
    return dot(w * p, 1.xxx);
}

float2 BarycentricDeriv(float2 p0, float2 p1, float2 p2, float3 w)
{
    float3 pp1 = float3(p0.x, p1.x, p2.x);
    float3 pp2 = float3(p0.y, p1.y, p2.y);
    
    return float2(dot(w * pp1, 1.xxx), dot(w * pp2, 1.xxx));
}

float2 BarycentricDeriv(float3 p0, float3 p1, float3 p2, float3 w)
{
    float3 pp1 = float3(p0.x, p1.x, p2.x);
    float3 pp2 = float3(p0.y, p1.y, p2.y);
    float3 pp3 = float3(p0.z, p1.z, p2.z);

    return float3(dot(w * pp1, 1.xxx), dot(w * pp2, 1.xxx), dot(w * pp3, 1.xxx));
}

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

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x < resX && dispatchThreadId.y < resY)
    {
        Texture2D<uint> visBufferTex = ResourceDescriptorHeap[visBufferSrvIndex];
        RWTexture2D<float4> gbufferBaseColor = ResourceDescriptorHeap[gbuffer0UavIndex];
        RWTexture2D<float4> gbufferNormals = ResourceDescriptorHeap[gbuffer1UavIndex];
        RWTexture2D<float4> gbufferMetallicRoughnessAo = ResourceDescriptorHeap[gbuffer2UavIndex];

        // Retrieve object and triangle id for vis buffer
        uint objectId, triangleId;
        DecodeVisibilityBuffer(visBufferTex[dispatchThreadId.xy], objectId, triangleId);

        // Use object id to retrieve the primitive info
        ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[scenePrimitivesIndex];
        const FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(objectId * sizeof(FGpuPrimitive));
        
        // Use triangle id to retrieve the vertex indices of the triangle
        uint baseTriIndex = triangleId * 3;
        const uint3 vertIndices = MeshMaterial::GetUint3(baseTriIndex, primitive.m_indexAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex);

        const float3 vertPositions[3] = {
            MeshMaterial::GetFloat3(vertIndices.x, primitive.m_positionAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex),
            MeshMaterial::GetFloat3(vertIndices.y, primitive.m_positionAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex),
            MeshMaterial::GetFloat3(vertIndices.z, primitive.m_positionAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex)
        };

        // Transform the triangle verts to ndc space
        float4x4 localToWorld = mul(primitive.m_localToWorld, sceneRotation);
        float4x4 localToClip = mul(localToWorld, viewProjTransform);
        float4 p[3] = {
            mul(float4(vertPositions[0], 1.f), localToClip),
            mul(float4(vertPositions[1], 1.f), localToClip),
            mul(float4(vertPositions[2], 1.f), localToClip)
        };

        // Calculate screen space barycentrics based on pixel NDC
        float2 screenRes = float2(resX, resY);
        float2 pixelNdc = (dispatchThreadId.xy + 0.5.xx) / screenRes;
        pixelNdc.x = 2.f * pixelNdc.x - 1.f;
        pixelNdc.y = -2.f * pixelNdc.y + 1;
        FBarycentrics lambda = CalcBarycentrics(
            p[0], 
            p[1], 
            p[2], 
            pixelNdc, 
            screenRes);

        float3 vertNormals[3] = {
            MeshMaterial::GetFloat3(vertIndices.x, primitive.m_normalAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex),
            MeshMaterial::GetFloat3(vertIndices.y, primitive.m_normalAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex),
            MeshMaterial::GetFloat3(vertIndices.z, primitive.m_normalAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex),
        };

        float4 vertTangents[3] = {
           MeshMaterial::GetFloat4(vertIndices.x, primitive.m_tangentAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex),
           MeshMaterial::GetFloat4(vertIndices.y, primitive.m_tangentAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex),
           MeshMaterial::GetFloat4(vertIndices.z, primitive.m_tangentAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex)
        };

        float3 N = normalize(BarycentricInterp(vertNormals[0], vertNormals[1], vertNormals[2], lambda.m_lambda));
        float3 T = normalize(BarycentricInterp(vertTangents[0].xyz, vertTangents[1].xyz, vertTangents[2].xyz, lambda.m_lambda));
        float3 B = normalize(cross(N, T) * vertTangents[0].w);
        float3x3 tangentToWorld = mul(float3x3(T, B, N), (float3x3)localToWorld);

        float2 vertexUVs[3] = {
            MeshMaterial::GetFloat2(vertIndices.x, primitive.m_uvAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex),
            MeshMaterial::GetFloat2(vertIndices.y, primitive.m_uvAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex),
            MeshMaterial::GetFloat2(vertIndices.z, primitive.m_uvAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex),
        };
        float2 UV = BarycentricInterp(vertexUVs[0], vertexUVs[1], vertexUVs[2], lambda.m_lambda);
        float2 ddxUV = BarycentricDeriv(vertexUVs[0], vertexUVs[1], vertexUVs[2], lambda.m_ddx);
        float2 ddyUV = BarycentricDeriv(vertexUVs[0], vertexUVs[1], vertexUVs[2], lambda.m_ddy);

        // Evaluate Material
        FMaterial material = MeshMaterial::GetMaterial(primitive.m_materialIndex, sceneMaterialBufferIndex);
        FMaterialProperties matInfo = EvaluateMaterialProperties(material, UV, g_trilinearSampler, ddxUV, ddyUV);

        N = normalize(mul(matInfo.normalmap, tangentToWorld));

        gbufferNormals[dispatchThreadId.xy] = float4(N, 0.f);
    }
}