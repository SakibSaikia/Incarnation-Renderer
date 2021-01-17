#define rootsig \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_ALL, filter = FILTER_MIN_MAG_LINEAR_MIP_POINT, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP), " \
    "RootConstants(b0, num32BitConstants=4, visibility = SHADER_VISIBILITY_ALL)," \
    "DescriptorTable(SRV(t0, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_ALL), " \
    "DescriptorTable(UAV(u0, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_ALL), "

struct CbLayout
{
    uint mipIndex;
    uint hdrSpehericalMapBindlessIndex;
    uint outputCubemapBindlessIndex;
    uint cubemapSize;
};

ConstantBuffer<CbLayout> g_computeConstants : register(b0);
Texture2D g_srvBindless2DTextures[] : register(t0);
RWTexture2DArray<float3> g_uavBindless2DTextureArrays[] : register(u0);
SamplerState g_bilinearSampler : register(s0);

static const float PI = 3.14159265f;

// Adapted from https://stackoverflow.com/questions/29678510/convert-21-equirectangular-panorama-to-cube-map

[numthreads(16,16,1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    Texture2D src = g_srvBindless2DTextures[g_computeConstants.hdrSpehericalMapBindlessIndex];
    RWTexture2DArray<float3> dest = g_uavBindless2DTextureArrays[g_computeConstants.outputCubemapBindlessIndex];

    if (dispatchThreadId.x < g_computeConstants.cubemapSize && 
        dispatchThreadId.y < g_computeConstants.cubemapSize)
    {
        float2 faceTransform[6] = {
           float2(0.5f * PI, 0.f),      // +X
           float2(-0.5f * PI, 0.f),     // -X
           float2(0, 0.5f * PI),        // +Y
           float2(0.f, -0.5f * PI),     // -Y
           float2(0.f, 0.f),            // +Z
           float2(PI, 0.f)              // -Z
        };

        // Calculate adjacent (ak) and opposite (an) of the triangle that is spanned from the sphere center to our cube face.
        const float an = sin(0.25f * PI);
        const float ak = cos(0.25f * PI);

        float2 n = (dispatchThreadId.xy + 0.5.xx) / (float)g_computeConstants.cubemapSize.xx;
        n = 2.f * n - 1.xx;
        n *= an;

        for (int i = 0; i < 6; ++i)
        {
            float2 ft = faceTransform[i];
            float2 uv;

            if (ft.y == 0.f)
            {
                // center faces
                uv.x = atan2(n.x, ak);
                uv.y = atan2(n.y * cos(uv.x), ak);
                uv.x += ft.x;
            }
            else if(ft.y < 0.f)
            {
                // bottom face
                float d = length(n);
                uv.y = 0.5f * PI - atan2(d, ak);
                uv.x = 0.5f * PI + atan2(n.y, n.x);
            }
            else
            {
                // top face
                float d = length(n);
                uv.y = -0.5f * PI + atan2(d, ak);
                uv.x = atan2(n.x, n.y);
            }

            // Map from angular coordinates to [-1, 1], respectively.
            uv.x = uv.x / PI;
            uv.y = uv.y / (0.5f * PI);

            // Warp around, if our coordinates are out of bounds. 
            while (uv.y < -1) 
            {
                uv.y += 2;
                uv.x += 1;
            }

            while (uv.y > 1) 
            {
                uv.y -= 2;
                uv.x += 1;
            }

            while (uv.x < -1) 
            {
                uv.x += 2;
            }

            while (uv.x > 1) 
            {
                uv.x -= 2;
            }

            // Map from [-1, 1] to in texture space
            uv.x = 0.5f * uv.x + 0.5f;
            uv.y = 0.5f * uv.y + 0.5f;

            dest[uint3(dispatchThreadId.x, dispatchThreadId.y, i)] = src.SampleLevel(g_bilinearSampler, uv, g_computeConstants.mipIndex).rgb;
        }
    }
    else
    {
        for (int i = 0; i < 6; ++i)
        {
            dest[uint3(dispatchThreadId.x, dispatchThreadId.y, i)] = 1.xxx;
        }
    }
}