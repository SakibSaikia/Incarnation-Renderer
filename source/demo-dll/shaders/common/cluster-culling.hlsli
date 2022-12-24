#ifndef __CLUSTER_CULLING_HLSLI_
#define __CLUSTER_CULLING_HLSLI_

#define MAX_LIGHT_RANGE (4.f)

struct FFrustum
{
    // .xyz = Plane Normal, .w = d
    float4 m_nearPlane;
    float4 m_farPlane;
    float4 m_leftPlane;
    float4 m_rightPlane;
    float4 m_bottomPlane;
    float4 m_topPlane;
};

// Used to reference the culled light list
struct FLightGridData
{
    uint32_t m_offset;
    uint32_t m_count;
};

// Flattened cluster ID corresponding to location of the cluster in the volume
uint GetClusterId(float3 clusterIndex, uint3 clusterGridSize)
{
    return (clusterGridSize.x * clusterGridSize.y) * clusterIndex.z + clusterGridSize.x * clusterIndex.y + clusterIndex.x;
}

//Gets a pixelId (window coordinates) and returns the cluster it belongs to
uint3 GetPixelCluster(uint2 pixelId, float viewSpaceDepth, float2 clusterGridRes, float2 clusterSliceScaleAndBias)
{
    uint3 cluster;

    // Clusters are evenly distributed in xy direction
    cluster.xy = pixelId / clusterGridRes;

    // See this for z-slice calculation: http://www.aortiz.me/2018/12/21/CG.html
    cluster.z = floor(log(viewSpaceDepth) * clusterSliceScaleAndBias.x + clusterSliceScaleAndBias.y);

    return cluster;
}

// Get the planes that describe the cluster frustum in world space
FFrustum GetClusterFrustum(uint3 clusterIndex, uint3 clusterGridSize, float2 viewSpaceDepthExtent, float4x4 projTransform, float4x4 invViewProjTransform)
{
    // Cluster slices are evenly assigned in NDC space for X & Y directions
    float2 clusterNDC = clusterIndex.xy / (float2) clusterGridSize.xy;
    clusterNDC = 2.xx * clusterNDC - 1.xx;
    clusterNDC.y = -clusterNDC.y;

    float2 Stride = 2.f / (float2) clusterGridSize.xy;

    // Cluster slices should not be evenly assigned for depth in NDC space as it causes skewed placement of clusters due to z-nonlinearity.
    // Instead, use an exponential scheme for slices in view space to counter this effect as per [Tiago Sousa SIGGRAPH 2016]
    const float zNear = viewSpaceDepthExtent.x;
    const float zFar = viewSpaceDepthExtent.y;
    float ViewSpaceClusterDepthExtents[] = {
        zNear * pow(zFar / zNear, clusterIndex.z / (float)clusterGridSize.z),
        zNear * pow(zFar / zNear, (clusterIndex.z + 1.f) / (float)clusterGridSize.z)
    };

    // Project the view space depth extents to NDC space
    float4 NDCNearPoint = mul(float4(0.f, 0.f, ViewSpaceClusterDepthExtents[0], 1.f), projTransform);
    float4 NDCFarPoint = mul(float4(0.f, 0.f, ViewSpaceClusterDepthExtents[1], 1.f), projTransform);

    float4 ProjectedClusterPoints[] = {
        // Near plane points
        float4(clusterNDC.x, clusterNDC.y - Stride.y, NDCNearPoint.z / NDCNearPoint.w, 1.f),
        float4(clusterNDC.x + Stride.x, clusterNDC.y - Stride.y, NDCNearPoint.z / NDCNearPoint.w, 1.f),
        float4(clusterNDC.x + Stride.x, clusterNDC.y, NDCNearPoint.z / NDCNearPoint.w, 1.f),
        float4(clusterNDC.x, clusterNDC.y, NDCNearPoint.z / NDCNearPoint.w, 1.f),
        // Far plane points
        float4(clusterNDC.x, clusterNDC.y - Stride.y, NDCFarPoint.z / NDCFarPoint.w, 1.f),
        float4(clusterNDC.x + Stride.x, clusterNDC.y - Stride.y, NDCFarPoint.z / NDCFarPoint.w, 1.f),
        float4(clusterNDC.x + Stride.x, clusterNDC.y, NDCFarPoint.z / NDCFarPoint.w, 1.f),
        float4(clusterNDC.x, clusterNDC.y, NDCFarPoint.z / NDCFarPoint.w, 1.f),
    };

    // Unproject the NDC points to get frustum in World Space
    float3 P[8];
    for (int i = 0; i < 8; ++i)
    {
        float4 worldPos = mul(ProjectedClusterPoints[i], invViewProjTransform);
        P[i] = worldPos.xyz / worldPos.w;
    }

    // Extract frustum planes from the world space frustum points
    // Equation of plane passing through 3 points A, B and C is given by
    // n = (B - A) X (C - A) and d = -n.A
    FFrustum clusterFrustum;
    clusterFrustum.m_nearPlane.xyz = cross(P[1] - P[0], P[3] - P[0]);
    clusterFrustum.m_farPlane.xyz = cross(P[7] - P[4], P[5] - P[4]);
    clusterFrustum.m_leftPlane.xyz = cross(P[3] - P[0], P[4] - P[0]);
    clusterFrustum.m_rightPlane.xyz = cross(P[5] - P[1], P[2] - P[1]);
    clusterFrustum.m_topPlane.xyz = cross(P[6] - P[2], P[3] - P[2]);
    clusterFrustum.m_bottomPlane.xyz = cross(P[4] - P[0], P[1] - P[0]);

    clusterFrustum.m_nearPlane.w = -dot(clusterFrustum.m_nearPlane.xyz, P[0].xyz);
    clusterFrustum.m_farPlane.w = -dot(clusterFrustum.m_farPlane.xyz, P[4].xyz);
    clusterFrustum.m_leftPlane.w = -dot(clusterFrustum.m_leftPlane.xyz, P[0].xyz);
    clusterFrustum.m_rightPlane.w = -dot(clusterFrustum.m_rightPlane.xyz, P[1].xyz);
    clusterFrustum.m_topPlane.w = -dot(clusterFrustum.m_topPlane.xyz, P[2].xyz);
    clusterFrustum.m_bottomPlane.w = -dot(clusterFrustum.m_bottomPlane.xyz, P[0].xyz);

    return clusterFrustum;
}

// Return true if the bounds are contained within the frustum, else return false.
bool FrustumCull(FFrustum frustum, float4 bounds)
{
    // Light bounds
    const float4 boundsCenter = float4(bounds.xyz, 1.f);
    const float boundsRadius = bounds.w;

    // Frustum test - scale the radius by the plane normal instead of normalizing the plane
    return (dot(boundsCenter, frustum.m_nearPlane) + boundsRadius * length(frustum.m_nearPlane.xyz) >= 0)
        && (dot(boundsCenter, frustum.m_farPlane) + boundsRadius * length(frustum.m_farPlane.xyz) >= 0)
        && (dot(boundsCenter, frustum.m_leftPlane) + boundsRadius * length(frustum.m_leftPlane.xyz) >= 0)
        && (dot(boundsCenter, frustum.m_rightPlane) + boundsRadius * length(frustum.m_rightPlane.xyz) >= 0)
        && (dot(boundsCenter, frustum.m_bottomPlane) + boundsRadius * length(frustum.m_bottomPlane.xyz) >= 0)
        && (dot(boundsCenter, frustum.m_topPlane) + boundsRadius * length(frustum.m_topPlane.xyz) >= 0);
}

#endif // __CLUSTER_CULLING_HLSLI_