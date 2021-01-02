#define rootsig \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_ALL, filter = FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU = TEXTURE_ADDRESS_BORDER, addressV = TEXTURE_ADDRESS_BORDER, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE), " \
    "RootConstants(b0, num32BitConstants=3, visibility = SHADER_VISIBILITY_ALL)," \
    "DescriptorTable(SRV(t0, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_ALL), " \
    "DescriptorTable(UAV(u0, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_ALL), "

struct CbLayout
{
    uint hdrSpehericalMapBindlessIndex;
    uint outputCubemapBindlessIndex;
    uint cubemapSize;
};

ConstantBuffer<CbLayout> computeConstants : register(b0);
Texture2D srvBindless2DTextures[] : register(t0);
RWTexture2DArray<float3> uavBindless2DTextureArrays[] : register(u0);
SamplerState bilinearSampler : register(s0);

static const float PI = 3.14159265f;

[numthreads(16,16,1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    Texture2D src = srvBindless2DTextures[computeConstants.hdrSpehericalMapBindlessIndex];
    RWTexture2DArray<float3> dest = uavBindless2DTextureArrays[computeConstants.outputCubemapBindlessIndex];

    if (dispatchThreadId.x < computeConstants.cubemapSize && 
        dispatchThreadId.y < computeConstants.cubemapSize)
    {
        float2 destUV = dispatchThreadId.xy / (float)computeConstants.cubemapSize.xx;
        float2 pos = 2.f * destUV - 1.xx;

        float3 faces[6] = {
            float3(1.f, pos.x, -pos.y),
            float3(-1.f, pos.x, pos.y),
            float3(pos.x, 1.f, -pos.y),
            float3(pos.x, -1.f, pos.y),
            float3(pos.x, pos.y, 1.f),
            float3(-pos.x, pos.y, -1.f)
        };

        for (int i = 0; i < 6; ++i)
        {
            float3 p = faces[i];
            float longitude = atan2(p.y, p.x);
            float latitude = atan2(p.z, length(p.xy));
            float2 srcUV = float2((longitude + PI) / (2 * PI), (latitude + 0.5 * PI) / PI);
            dest[uint3(dispatchThreadId.x, dispatchThreadId.y, i)] = src.SampleLevel(bilinearSampler, srcUV, 0).rgb;
        }
    }
    else
    {
        for (int i = 0; i < 6; ++i)
        {
            dest[uint3(dispatchThreadId.x, dispatchThreadId.y, i)] = 0.xxx;
        }
    }
}