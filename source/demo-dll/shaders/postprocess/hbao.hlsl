/*
    Implementation based on https://github.com/Patapom/GodComplex/blob/master/Tests/TestHBIL/2018%20Mayaux%20-%20Horizon-Based%20Indirect%20Lighting%20(HBIL).pdf
*/

#include "gpu-shared-types.h"
#include "common/math.hlsli"
#include "geo-raster/encoding.hlsli"

#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
#define THREAD_GROUP_SIZE_Y 1
#endif

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants=3)," \
    "CBV(b1)," \
    "CBV(b2)"

static const uint NumSlices = 4;
static const float AzimuthalSliceAngle = k_Pi / NumSlices;

struct FPassConstants
{
    uint m_aoTargetUavIndex;
    uint m_depthTargetSrvIndex;
    uint m_gbufferNormalsSrvIndex;
};

ConstantBuffer<FPassConstants> g_passCb : register(b0);
ConstantBuffer<FViewConstants> g_viewCb : register(b1);
ConstantBuffer<FSceneConstants> g_sceneCb : register(b2);

float GatherAO(float cosHorizonAngleStart)
{
    return 2.f * cosHorizonAngleStart;
}


[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x < g_viewCb.m_resX && dispatchThreadId.y < g_viewCb.m_resY)
    {
        // Pixel NDC
        float2 pixelUV = (dispatchThreadId.xy + 0.5.xx) / float2(g_viewCb.m_resX, g_viewCb.m_resY);
        float2 pixelNDC = 2.f * pixelUV - 1.xx;
        pixelNDC.y *= -1.f;

        // Pixel Depth
        Texture2D<float> depthTex = ResourceDescriptorHeap[g_passCb.m_depthTargetSrvIndex];
        float pixelDepth = depthTex[dispatchThreadId.xy];

        // Pixel Position
        float4 pixelPos = float4(pixelNDC.xy, pixelDepth, 1.f);
        float4 pixelWorldPos = mul(pixelPos, g_viewCb.m_invViewProjTransform);
        pixelWorldPos /= pixelWorldPos.w;
        
        // Construct local "camera" space around pixel
        // Referred to as ω0, ωx, ωy in the paper
        float3 localAt = normalize(g_viewCb.m_eyePos - pixelWorldPos.xyz);
        float3 localRight = normalize(cross(localAt, g_viewCb.m_cameraUpVector));
        float3 localUp = cross(localRight, localAt);

        // Pixel Normal - n
        Texture2D<float2> gbufferNormals = ResourceDescriptorHeap[g_passCb.m_gbufferNormalsSrvIndex];
        const float3 worldNormal = OctDecode(gbufferNormals[dispatchThreadId.xy]);

        // Azimuth angle φ
        float azimuthAngle = 0.f;

        float sumAO = 0.f;
        for (uint i = 0; i < NumSlices; ++i)
        {
            // Tangent vector for the slice plane that represents fixed angle offsets to sample AO around the hemisphere
            // D(φ) = cos(φ)ωx + sin(φ)ωy
            float3 sliceTangentVec = cos(azimuthAngle) * localRight + sin(azimuthAngle) * localUp;

            // Projection of the world normal onto the slice plane 
            // n' = {n.D(φ), n.ω0}
            float2 sliceNormal;
            sliceNormal.x = dot(worldNormal, sliceTangentVec);
            sliceNormal.y = dot(worldNormal, localAt);

            // Project slice direction vector D(φ) onto world normal plane
            float t = -dot(sliceTangentVec, worldNormal) / dot(localAt, worldNormal);
            float cosHorizonAngle = t * rsqrt(1 + t * t);

            sumAO += GatherAO(cosHorizonAngle);

            azimuthAngle += AzimuthalSliceAngle;
        }

        RWTexture2D<float> aoTarget = ResourceDescriptorHeap[g_passCb.m_aoTargetUavIndex];
        aoTarget[dispatchThreadId.xy] = 1.f - saturate(sumAO / 2.f * NumSlices);
    }
}