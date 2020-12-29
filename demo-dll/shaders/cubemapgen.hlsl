#define rootsig \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_ALL, filter = FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU = TEXTURE_ADDRESS_BORDER, addressV = TEXTURE_ADDRESS_BORDER, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE), " \
    "RootConstants(b0, num32BitConstants=2, visibility = SHADER_VISIBILITY_ALL)," \
    "DescriptorTable(SRV(t0, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_ALL), " \
    "DescriptorTable(UAV(u0, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_ALL), "

struct CbLayout
{
    uint hdrSpehericalMapBindlessIndex;
    uint outputCubemapBindlessIndex;
};

ConstantBuffer<CbLayout> computeConstants : register(b0);
Texture2D srvBindless2DTextures[] : register(t0);
RWTexture2DArray<float3> uavBindless2DTextureArrays[] : register(u0);
SamplerState bilinearSampler : register(s0);

[numthreads(16,16,1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    Texture2D src = srvBindless2DTextures[computeConstants.hdrSpehericalMapBindlessIndex];
    RWTexture2DArray<float3> dest = uavBindless2DTextureArrays[computeConstants.outputCubemapBindlessIndex];

    uint3 cubemapDim;
    dest.GetDimensions(cubemapDim.x, cubemapDim.y, cubemapDim.z);
    float2 uv = dispatchThreadId.xy / float2(cubemapDim.xy);

    dest[uint3(dispatchThreadId.x, dispatchThreadId.y, 0)] = src.SampleLevel(bilinearSampler, uv, 0).rgb;
}