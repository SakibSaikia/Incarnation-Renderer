#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
#define THREAD_GROUP_SIZE_Y 1
#endif

#define rootsig \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_ALL, filter = FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP), " \
    "RootConstants(b0, num32BitConstants=7, visibility = SHADER_VISIBILITY_ALL)," \
    "DescriptorTable(SRV(t0, space = 0, numDescriptors = 1000, flags = DESCRIPTORS_VOLATILE), visibility = SHADER_VISIBILITY_ALL), " \
    "DescriptorTable(UAV(u0, space = 0, numDescriptors = 1000, flags = DESCRIPTORS_VOLATILE), visibility = SHADER_VISIBILITY_ALL), "

struct CbLayout
{
    uint mipIndex;
    uint textureWidth;
    uint textureHeight;
    uint normalMapTextureIndex;
    uint metallicRoughnessTextureIndex;
    uint normalmapUavIndex;
    uint metallicRoughnessUavIndex;
};

ConstantBuffer<CbLayout> g_computeConstants : register(b0);
Texture2D g_srvBindless2DTextures[] : register(t0);
RWTexture2D<float4> g_uavBindless2DTextures[] : register(u0);
SamplerState g_bilinearSampler : register(s0);


[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    Texture2D srcNormalmap = g_srvBindless2DTextures[g_computeConstants.normalMapTextureIndex];
    Texture2D srcMetallicRoughness = g_srvBindless2DTextures[g_computeConstants.metallicRoughnessTextureIndex];
    RWTexture2D<float4> destNormalmap = g_uavBindless2DTextures[g_computeConstants.normalmapUavIndex];
    RWTexture2D<float4> destMetallicRoughness = g_uavBindless2DTextures[g_computeConstants.metallicRoughnessUavIndex];

    // Swizzle metal/roughness to R/G channels because they will be BC5 compressed
    if (g_computeConstants.mipIndex == 0)
    {
        destNormalmap[dispatchThreadId.xy] = srcNormalmap[dispatchThreadId.xy];
        destMetallicRoughness[dispatchThreadId.xy] = srcMetallicRoughness[dispatchThreadId.xy].bgra;
    }
    else
    {
        uint2 mipSize = uint2(g_computeConstants.textureWidth, g_computeConstants.textureHeight) >> g_computeConstants.mipIndex;
        if (dispatchThreadId.x < mipSize.x && dispatchThreadId.y < mipSize.y)
        {
            // Sample all the texels from the base mip level that are within the footprint of current mipmap texel
            // See: https://blog.selfshadow.com/publications/s2013-shading-course/rad/s2013_pbs_rad_notes.pdf
            float3 rAvg = 0.f;
            float metalnessAvg = 0.f;
            const uint texelFootprint = (1u << g_computeConstants.mipIndex);
            const float2 texelPos = texelFootprint * (dispatchThreadId.xy + 0.5f.xx);
            const float2 topLeft = -float2(texelFootprint.xx) / 2.f;
            for (uint y = 0; y < texelFootprint; ++y)
            {
                for (uint x = 0; x < texelFootprint; ++x)
                {
                    float2 offset = topLeft + float2(x, y);
                    float2 samplePos = floor(texelPos + offset);
                    float3 sampleNormal = srcNormalmap[samplePos].xyz;
                    sampleNormal = normalize(2.f * sampleNormal - 1.f);
                    float roughnessAlpha = srcMetallicRoughness[samplePos].g;
                    float metalness = srcMetallicRoughness[samplePos].b;

                    // Avg. metalness using standard box filter
                    metalnessAvg += metalness;

                    // Fit a vMF lobe to NDF for this source texel
                    // Convert normal and roughness to r form for filtering filter
                    // See: https://graphicrants.blogspot.com/2018/05/normal-map-filtering-using-vmf-part-3.html
                    float invLambda = 0.5f * roughnessAlpha * roughnessAlpha;
                    float exp2L = exp(-2.f / invLambda);
                    float cothLambda = invLambda > 0.1f ? (1.f + exp2L) / (1.f - exp2L) : 1.f;
                    float3 r = (cothLambda - invLambda) * sampleNormal;
                    rAvg += r;
                }
            }

            rAvg /= (texelFootprint * texelFootprint);
            metalnessAvg /= (texelFootprint * texelFootprint);

            // Convert back to normal and roughness
            float r2 = clamp(dot(rAvg, rAvg), 1e-8, 1.f);
            float invLambda = rsqrt(r2) * (1.f - r2) / (3.f - r2);
            float roughnessAlpha = sqrt(2.f * invLambda);
            float3 normal = 0.5f * normalize(rAvg) + 0.5f;

            destNormalmap[dispatchThreadId.xy] = float4(normal.xyz, 0.f);
            destMetallicRoughness[dispatchThreadId.xy] = float4(metalnessAvg, roughnessAlpha, 0.f, 0.f);
        }
    }
}