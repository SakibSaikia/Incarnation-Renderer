// Reference: https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/
#include "lighting/pbr.hlsli"

#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
#define THREAD_GROUP_SIZE_Y 1
#endif

#define rootsig \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)," \
    "StaticSampler(s0, filter = FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE)"

struct CbLayout
{
    float4x4 invViewProj;
    float4x4 prevViewProj;
    uint hdrSceneColorTextureIndex;
    uint taaAccumulationUavIndex;
    uint taaAccumulationSrvIndex;
    uint depthTextureIndex;
    uint resX;
    uint resY;
    uint historyIndex;
    float exposure;
};

ConstantBuffer<CbLayout> g_constants : register(b0);
SamplerState g_bilinearSampler : register(s0);

// Find the position of the pixel in the previous frame by unprojecting to 
// world space and then reprojecting using the previous frame's view projection matrix
float2 Reproject(float2 dtid)
{
    Texture2D depthTex = ResourceDescriptorHeap[g_constants.depthTextureIndex];
    float2 uv = (dtid.xy + 0.5.xx) / float2(g_constants.resX, g_constants.resY);

    float4 clipspacePos;
    clipspacePos.x = 2.f * uv.x - 1.f;
    clipspacePos.y = -2.f * uv.y + 1.f;
    clipspacePos.z = depthTex.Load(uint3(dtid.x, dtid.y, 0)).r;
    clipspacePos.w = 1.f;

    float4 worldspacePos = mul(clipspacePos, g_constants.invViewProj);

    float4 prevClipspacePos = mul(worldspacePos, g_constants.prevViewProj);
    prevClipspacePos /= prevClipspacePos.w;

    float2 reprojectedUV;
    reprojectedUV.x = 0.5 * prevClipspacePos.x + 0.5;
    reprojectedUV.y = -0.5 * prevClipspacePos.y + 0.5;

    return reprojectedUV;
}

// Sample a 3x3 neighborhood of the current color texture and clamp the previous color
// to this range to account for disocclusions
float3 ColorClamp(float2 dtid, float3 prevColor)
{
    Texture2D hdrSceneColorTex = ResourceDescriptorHeap[g_constants.hdrSceneColorTextureIndex];
    float3 minColor = 10000.f, maxColor = -10000.f;
    for(int x = -1; x <= 1; ++x)
    {
        for(int y = -1; y <= 1; ++y)
        {
            float3 color = hdrSceneColorTex.Load(uint3(dtid.x + x, dtid.y + y, 0)).rgb;
            minColor = min(minColor, color);
            maxColor = max(maxColor, color);
        }
    }

    return clamp(prevColor, minColor, maxColor);
}

// Convert color to LDR space for filtering and calculate weights which are 
// use to convert back to HDR after blending
float CalcLuminanceWeight(float3 hdrColor)
{
    float luminance = dot(hdrColor, float3(0.299, 0.587, 0.114));
    return 1.f / (1.f + luminance);
}


[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    Texture2D hdrSceneColorTex = ResourceDescriptorHeap[g_constants.hdrSceneColorTextureIndex];
    Texture2D historyColorTex = ResourceDescriptorHeap[g_constants.taaAccumulationSrvIndex];
    RWTexture2D<float3> taaAccumulationBuffer = ResourceDescriptorHeap[g_constants.taaAccumulationUavIndex];

    if (dispatchThreadId.x < g_constants.resX && dispatchThreadId.y < g_constants.resY)
    {
        float3 currentColor = hdrSceneColorTex.Load(uint3(dispatchThreadId.x, dispatchThreadId.y, 0)).rgb;

        if (g_constants.historyIndex == 0)
        {
            taaAccumulationBuffer[dispatchThreadId.xy] = currentColor;
        }
        else
        {
            // Use reprojected UVs to sample previous frame's color
            float2 reprojectedUV = Reproject(dispatchThreadId.xy);
            float3 previousColor = historyColorTex.SampleLevel(g_bilinearSampler, reprojectedUV, 0).rgb;

            // Apply neighborhood clamping
            float3 clampedPreviousColor = ColorClamp(dispatchThreadId.xy, previousColor);

            // Combine blend weights with luminance weights
            // https://graphicrants.blogspot.com/2013/12/tone-mapping.html
            float currentWeight = 0.1 * CalcLuminanceWeight(currentColor);
            float previousWeight = 0.9 * CalcLuminanceWeight(clampedPreviousColor);

            // Blend in LDR space and then covert back to HDR
            float3 output = currentColor * currentWeight + clampedPreviousColor * previousWeight;
            output /= (currentWeight + previousWeight);

            taaAccumulationBuffer[dispatchThreadId.xy] = output;
        }
    }
}