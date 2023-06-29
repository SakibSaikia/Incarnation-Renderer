#include "gpu-shared-types.h"
#include "common/mesh-material.hlsli"

#ifndef THREAD_GROUP_SIZE_X
    #define THREAD_GROUP_SIZE_X 1
#endif

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=3)," \
    "CBV(b1)," \
    "CBV(b2)"

struct FPassConstants
{
    uint m_defaultArgsBufferIndex;
    uint m_doubleSidedArgsBufferIndex;
    uint m_countsBufferIndex;
};

ConstantBuffer<FPassConstants> g_passCb : register(b0);
ConstantBuffer<FViewConstants> g_viewCb : register(b1);
ConstantBuffer<FSceneConstants> g_sceneCb : register(b2);

bool FrustumCull(FGpuPrimitive primitive)
{
    ByteAddressBuffer meshTransformsBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedSceneMeshTransformsBufferIndex];
    float4x4 localToWorld = meshTransformsBuffer.Load<float4x4>(primitive.m_meshIndex * sizeof(float4x4));
    localToWorld = mul(localToWorld, g_sceneCb.m_sceneRotation);

    // Gribb-Hartmann frustum plane extraction
    // http://www.cs.otago.ac.nz/postgrads/alexis/planeExtraction.pdf
    // https://fgiesen.wordpress.com/2012/08/31/frustum-planes-from-the-projection-matrix/
    float4x4 M = transpose(mul(localToWorld, g_viewCb.m_cullViewProjTransform));
    float4 nPlane = M[3] - M[2];
    float4 lPlane = M[3] + M[0];
    float4 rPlane = M[3] - M[0];
    float4 bPlane = M[3] + M[1];
    float4 tPlane = M[3] - M[1];

    // Primitive bounds
    const float4 boundsCenter = float4(primitive.m_boundingSphere.xyz, 1.f);
    const float boundsRadius = primitive.m_boundingSphere.w;

    // Frustum test - scale the radius by the plane normal instead of normalizing the plane
    return (dot(boundsCenter, nPlane) + boundsRadius * length(nPlane.xyz) >= 0)
        && (dot(boundsCenter, lPlane) + boundsRadius * length(lPlane.xyz) >= 0)
        && (dot(boundsCenter, rPlane) + boundsRadius * length(rPlane.xyz) >= 0)
        && (dot(boundsCenter, bPlane) + boundsRadius * length(bPlane.xyz) >= 0)
        && (dot(boundsCenter, tPlane) + boundsRadius * length(tPlane.xyz) >= 0);
}

[numthreads(THREAD_GROUP_SIZE_X, 1, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWByteAddressBuffer renderStatsBuffer = ResourceDescriptorHeap[SpecialDescriptors::RenderStatsBufferUavIndex];
    if (dispatchThreadId.x == 0)
    {
        renderStatsBuffer.Store(0, 0);
    }

    GroupMemoryBarrierWithGroupSync();

    uint primId = dispatchThreadId.x;
    if (primId < g_sceneCb.m_primitiveCount)
    {
        ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedScenePrimitivesBufferIndex];
        const FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(primId * sizeof(FGpuPrimitive));

        // Check if the mesh is hidden by user
        ByteAddressBuffer meshVisibilityBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedSceneMeshVisibilityBufferIndex];
        uint visibility = meshVisibilityBuffer.Load<uint>(primitive.m_meshIndex * sizeof(uint));
        if (visibility != 0)
        {
            if (FrustumCull(primitive))
            {
                FIndirectDrawWithRootConstants cmd = (FIndirectDrawWithRootConstants)0;
                cmd.m_rootConstants[0] = primId;
                cmd.m_drawArguments.m_vertexCount = primitive.m_indexCount;
                cmd.m_drawArguments.m_instanceCount = 1;
                cmd.m_drawArguments.m_startVertexLocation = 0;
                cmd.m_drawArguments.m_startInstanceLocation = 0;

                FMaterial material = MeshMaterial::GetMaterial(primitive.m_materialIndex, g_sceneCb.m_sceneMaterialBufferIndex);
                if (material.m_doubleSided)
                {
                    // Append to double-sided args buffer
                    uint currentIndex;
                    const uint doubleSidedArgsCountOffset = sizeof(uint);
                    RWByteAddressBuffer countsBuffer = ResourceDescriptorHeap[g_passCb.m_countsBufferIndex];
                    countsBuffer.InterlockedAdd(doubleSidedArgsCountOffset, 1, currentIndex);

                    RWByteAddressBuffer argsBuffer = ResourceDescriptorHeap[g_passCb.m_doubleSidedArgsBufferIndex];
                    uint destAddress = currentIndex * sizeof(FIndirectDrawWithRootConstants);
                    argsBuffer.Store(destAddress, cmd);
                }
                else
                {
                    // Append to default args buffer
                    uint currentIndex;
                    const uint defaultArgsCountOffset = 0;
                    RWByteAddressBuffer countsBuffer = ResourceDescriptorHeap[g_passCb.m_countsBufferIndex];
                    countsBuffer.InterlockedAdd(defaultArgsCountOffset, 1, currentIndex);

                    RWByteAddressBuffer argsBuffer = ResourceDescriptorHeap[g_passCb.m_defaultArgsBufferIndex];
                    uint destAddress = currentIndex * sizeof(FIndirectDrawWithRootConstants);
                    argsBuffer.Store(destAddress, cmd);
                }
            }
            else
            {
                int previousValue;
                renderStatsBuffer.InterlockedAdd(0, 1, previousValue);
            }
        }
    }
}