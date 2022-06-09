#include "gpu-shared-types.h"

#ifndef THREAD_GROUP_SIZE_X
    #define THREAD_GROUP_SIZE_X 1
#endif

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)"

cbuffer cb : register(b0)
{
    uint g_argsBufferIndex;
    uint g_countsBufferIndex;
    uint g_primitivesBufferIndex;
    uint g_primitiveCount;
    float4x4 g_viewProjTransform;
};

bool FrustumCull(FGpuPrimitive primitive)
{
    // Gribb-Hartmann frustum plane extraction
    // http://www.cs.otago.ac.nz/postgrads/alexis/planeExtraction.pdf
    // https://fgiesen.wordpress.com/2012/08/31/frustum-planes-from-the-projection-matrix/
    float4x4 M = transpose(mul(primitive.m_localToWorld, g_viewProjTransform));
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
    uint primId = dispatchThreadId.x;
    if (primId < g_primitiveCount)
    {
        ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[g_primitivesBufferIndex];
        const FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(primId * sizeof(FGpuPrimitive));

        if (FrustumCull(primitive))
        {
            FDrawWithRootConstants cmd = (FDrawWithRootConstants)0;
            cmd.rootConstants[0] = primId;
            cmd.drawArguments.VertexCount = primitive.m_indexCount;
            cmd.drawArguments.InstanceCount = 1;
            cmd.drawArguments.StartVertexLocation = 0;
            cmd.drawArguments.StartInstanceLocation = 0;

            uint currentIndex;
            RWByteAddressBuffer countsBuffer = ResourceDescriptorHeap[g_countsBufferIndex];
            countsBuffer.InterlockedAdd(0, 1, currentIndex);

            RWByteAddressBuffer argsBuffer = ResourceDescriptorHeap[g_argsBufferIndex];
            uint destAddress = currentIndex * sizeof(FDrawWithRootConstants);
            argsBuffer.Store(destAddress, cmd);
        }
    }
}