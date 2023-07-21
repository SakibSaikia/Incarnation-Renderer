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
    "RootConstants(b0, num32BitConstants=4)," \
    "CBV(b1)," \
    "CBV(b2)"

static const uint NumSlices = 4;
static const uint MaxTracesPerSlice = 4;
static const float AzimuthalSliceAngle = k_Pi / NumSlices;
static const float TerminateTraceThreshold = k_Pi / 36.f; // 5 degrees
static const float MaxTraceLength = 1.f;

struct FPassConstants
{
    uint m_aoTargetUavIndex;
    uint m_bentNormalTargetUavIndex;
    uint m_depthTargetSrvIndex;
    uint m_gbufferNormalsSrvIndex;
};

ConstantBuffer<FPassConstants> g_passCb : register(b0);
ConstantBuffer<FViewConstants> g_viewCb : register(b1);
ConstantBuffer<FSceneConstants> g_sceneCb : register(b2);

bool RayHit(float3 origin, float3 dir)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.1f;
    ray.TMax = MaxTraceLength;

    RaytracingAccelerationStructure sceneBvh = ResourceDescriptorHeap[g_sceneCb.m_sceneBvhIndex];

    RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
    q.TraceRayInline(sceneBvh, RAY_FLAG_NONE, 0xff, ray);

    if (!q.Proceed())
    {
        return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    }
    else
    {
        // This means that further evaluation is needed. For now, consider translucent triangles as a hit.
        return q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE;
    }
}

float ComputeHorizonAngle(float horizonAngleStart, float3 ws_pixelPos, float3 ws_sliceX, float3 ws_sliceY)
{
    float minHorizonAngle = 0.f;
    float maxHorizonAngle = horizonAngleStart;
    int traceIdx = 0;
    float newHorizonAngle;

    // Binary search
    while (traceIdx < MaxTracesPerSlice && (maxHorizonAngle - minHorizonAngle) > TerminateTraceThreshold)
    {
        newHorizonAngle = minHorizonAngle + 0.5f * (maxHorizonAngle - minHorizonAngle);
        float3 ws_rayDir = cos(newHorizonAngle) * ws_sliceY + sin(newHorizonAngle) * ws_sliceX;

        if (RayHit(ws_pixelPos, ws_rayDir))
        {
            maxHorizonAngle = newHorizonAngle;
        }
        else
        {
            minHorizonAngle = newHorizonAngle;
        }

        traceIdx++;
    }

    return maxHorizonAngle;
}

void GatherAO(float horizonAngleStart, float3 ws_pixelPos, float3 ws_sliceX, float3 ws_sliceY, float2 ss_projectedNormal, out float ao, out float3 bentNormal)
{
    float theta1 = ComputeHorizonAngle(horizonAngleStart, ws_pixelPos, ws_sliceX, ws_sliceY);     // front horizon angle
    float theta0 = -ComputeHorizonAngle(horizonAngleStart, ws_pixelPos, -ws_sliceX, ws_sliceY);   // back horizon angle

    float ss_nx = 0.5f * (theta1 - theta0 + sin(theta0) * cos(theta0) - sin(theta1) * cos(theta1));
    float ss_ny = 0.5f * (2.f - cos(theta0) * cos(theta0) - cos(theta1) * cos(theta1));

    ao = ss_projectedNormal.x * ss_nx + ss_projectedNormal.y * ss_ny;
    bentNormal = normalize(ws_sliceX * ss_nx + ws_sliceY * ss_ny);
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
        float4 ndc_pixelPos = float4(pixelNDC.xy, pixelDepth, 1.f);
        float4 ws_pixelPos = mul(ndc_pixelPos, g_viewCb.m_invViewProjTransform);
        ws_pixelPos /= ws_pixelPos.w;
        
        // Construct local "camera" space around pixel
        // Referred to as ω0, ωx, ωy in the paper
        float3 ws_localAt = normalize(g_viewCb.m_eyePos - ws_pixelPos.xyz);
        float3 ws_localRight = normalize(cross(ws_localAt, g_viewCb.m_cameraUpVector));
        float3 ws_localUp = cross(ws_localRight, ws_localAt);

        // Pixel Normal - n
        Texture2D<float2> gbufferNormals = ResourceDescriptorHeap[g_passCb.m_gbufferNormalsSrvIndex];
        const float3 ws_normal = OctDecode(gbufferNormals[dispatchThreadId.xy]);

        // Azimuth angle φ
        float azimuthAngle = 0.f;

        float sumAO = 0.f;
        float3 avgBentNormal = 0.f;
        for (uint i = 0; i < NumSlices; ++i)
        {
            // Tangent vector for the slice plane that represents fixed angle offsets to sample AO around the hemisphere
            // D(φ) = cos(φ)ωx + sin(φ)ωy
            float3 ws_sliceTangentVec = cos(azimuthAngle) * ws_localRight + sin(azimuthAngle) * ws_localUp;

            // Projection of the world normal onto the slice plane 
            // n' = {n.D(φ), n.ω0}
            float2 ss_projectedNormal;
            ss_projectedNormal.x = dot(ws_normal, ws_sliceTangentVec);
            ss_projectedNormal.y = dot(ws_normal, ws_localAt);

            // Project slice direction vector D(φ) onto world normal plane
            float t = -dot(ws_sliceTangentVec, ws_normal) / dot(ws_localAt, ws_normal);

            // Horizon angle is the angle between ws_localUp and the projected slice vector into the normal plane
            // Note: that front and back horizon angles are the same, just different sign. (although the perspective distorts the perception in Fig 6 of the paper)
            float horizonAngle = acos(t * rsqrt(1 + t * t));

            float outAO;
            float3 outBentNormal;
            GatherAO(horizonAngle, ws_pixelPos.xyz, ws_sliceTangentVec, ws_localAt, ss_projectedNormal, outAO, outBentNormal);

            sumAO += outAO;
            avgBentNormal += outBentNormal;

            azimuthAngle += AzimuthalSliceAngle;
        }

        // For each slice we are computing AO for both front and back horizon angles.
        // So, we have to divide the total AO by two times the number of slices.
        RWTexture2D<float> aoTarget = ResourceDescriptorHeap[g_passCb.m_aoTargetUavIndex];
        aoTarget[dispatchThreadId.xy] = saturate(sumAO / (float)NumSlices);

        RWTexture2D<float2> bentNormalTarget = ResourceDescriptorHeap[g_passCb.m_bentNormalTargetUavIndex];
        bentNormalTarget[dispatchThreadId.xy] = OctEncode(normalize(avgBentNormal));
    }
}