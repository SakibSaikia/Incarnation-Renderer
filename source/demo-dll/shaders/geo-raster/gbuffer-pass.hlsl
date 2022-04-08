// Reference - http://filmicworlds.com/blog/visibility-buffer-rendering-with-material-graphs/

#include "common/mesh-material.hlsli"
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
    "StaticSampler(s0, filter = FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP)"

struct FBarycentricData
{
    float3 m_lambda;
    float3 m_ddx;
    float3 m_ddy;
};

// See: https://cg.ivd.kit.edu/publications/2015/dais/DAIS.pdf
// See: https://www.scratchapixel.com/lessons/3d-basic-rendering/rasterization-practical-implementation/perspective-correct-interpolation-vertex-attributes
FBarycentricData CalcBarycentrics(float2 p0, float2 p1, float2 p2, float2 p, float2 res)
{
    FBarycentricData o = (FBarycentricData)0;

    float area = determinant(float2x2(p2 - p1, p0 - p1));
    o.m_lambda.x = determinant(float2x2(p - p1, p2 - p1)) / area;
    o.m_lambda.y = determinant(float2x2(p - p2, p0 - p2)) / area;
    o.m_lambda.z = 1.f - o.m_lambda.x - o.m_lambda.y;

    //o.m_ddx = invD * float3(p1.y - p2.y, p2.y - p0.y, p0.y - p1.y);
    //o.m_ddy = invD * float3(p2.x - p1.x, p0.x - p2.x, p1.x - p0.x);

    // The derivatives of the barycentric are scaled by 2/winSize to change the scale from NDC units (-1 to 1) to pixel units
    o.m_ddx *= (2.f / res.x);
    o.m_ddy *= (2.f / res.y);

    // NDC is bottom to top whereas window co-ordinates are top to bottom
    o.m_ddy *= -1.0f;

    return o;
}

float3 Interpolate(float3 p0, float3 p1, float3 p2, FBarycentricData w)
{
    float3x3 p = float3x3(p0, p1, p2);
    return mul(w.m_lambda, p);
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

SamplerState g_bilinearSampler : register(s0);

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
        const uint vertIndices[3] = {
            MeshMaterial::GetUint(baseTriIndex + 0, primitive.m_indexAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex),
            MeshMaterial::GetUint(baseTriIndex + 1, primitive.m_indexAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex),
            MeshMaterial::GetUint(baseTriIndex + 2, primitive.m_indexAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex)
        };

        const float3 vertPositions[3] = {
            MeshMaterial::GetFloat3(vertIndices[0], primitive.m_positionAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex),
            MeshMaterial::GetFloat3(vertIndices[1], primitive.m_positionAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex),
            MeshMaterial::GetFloat3(vertIndices[2], primitive.m_positionAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex)
        };

        // Transform the triangle verts to ndc space
        float4x4 localToWorld = mul(primitive.m_localToWorld, sceneRotation);
        float4x4 localToClip = localToWorld * viewProjTransform;
        float4 p[3] = {
            mul(float4(vertPositions[0], 1.f), localToClip),
            mul(float4(vertPositions[1], 1.f), localToClip),
            mul(float4(vertPositions[2], 1.f), localToClip)
        };

        // Evaluate triangle barycentrics based on pixel NDC
        float2 screenRes = float2(resX, resY);
        float2 pixelNdc = (dispatchThreadId.xy + 0.5.xx)/ screenRes;
        pixelNdc.x = 2.f * pixelNdc.x - 1.f;
        pixelNdc.y = -2.f * pixelNdc.y + 1;
        FBarycentricData bary = CalcBarycentrics(
            p[0].xy / p[0].w, 
            p[1].xy / p[1].w, 
            p[2].xy / p[2].w, 
            pixelNdc, 
            screenRes);
        

        float4 vertNormals[3] = {
            mul(float4(MeshMaterial::GetFloat3(vertIndices[0], primitive.m_normalAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex), 0.f), localToWorld),
            mul(float4(MeshMaterial::GetFloat3(vertIndices[1], primitive.m_normalAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex), 0.f), localToWorld),
            mul(float4(MeshMaterial::GetFloat3(vertIndices[2], primitive.m_normalAccessor, sceneMeshAccessorsIndex, sceneMeshBufferViewsIndex), 0.f), localToWorld)
        };

        float3 N = Interpolate(vertNormals[0].xyz, vertNormals[1].xyz, vertNormals[2].xyz, bary);

        gbufferNormals[dispatchThreadId.xy] = float4(N.xyz, 0.f);
    }
}