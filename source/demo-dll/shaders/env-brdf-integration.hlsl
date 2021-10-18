// References : 
// https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf
// https://learnopengl.com/PBR/IBL/Specular-IBL

#include "pbr.hlsli"

#ifndef THREAD_GROUP_SIZE_X
#define THREAD_GROUP_SIZE_X 1
#endif

#ifndef THREAD_GROUP_SIZE_Y
#define THREAD_GROUP_SIZE_Y 1
#endif

#define rootsig \
    "RootConstants(b0, num32BitConstants=4, visibility = SHADER_VISIBILITY_ALL)," \
    "DescriptorTable(UAV(u0, space = 0, numDescriptors = 1000, flags = DESCRIPTORS_VOLATILE), visibility = SHADER_VISIBILITY_ALL), "

struct CbLayout
{
    uint outputUavWidth;
    uint outputUavHeight;
    uint outputUavIndex;
    uint sampleCount;
};

ConstantBuffer<CbLayout> g_computeConstants : register(b0);
RWTexture2D<float2> g_uavBindless2DTextures[] : register(u0);

[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void cs_main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x < g_computeConstants.outputUavWidth &&
        dispatchThreadId.y < g_computeConstants.outputUavHeight)
    {
        float2 res = 0.f.xx;
        const float NoV = dispatchThreadId.x / (float)g_computeConstants.outputUavWidth;
        const float Roughness = 1.f - dispatchThreadId.y / (float)g_computeConstants.outputUavHeight;
        const uint NumSamples = g_computeConstants.sampleCount;

        float3 V = float3(sqrt(1.f - NoV * NoV), 0.f, NoV);
        float3 N = float3(0.f, 0.f, 1.f);

        float A = 0.f;
        float B = 0.f;

        for (uint i = 0; i < NumSamples; i++)
        {
            float2 Xi = Hammersley(i, NumSamples);
            float3 H = ImportanceSampleGGX(Xi, Roughness, N);
            float3 L = normalize(2.f * dot(V, H) * H - V);

            float NoL = saturate(L.z);
            float NoH = saturate(H.z);
            float VoH = saturate(dot(V, H));

            float G = G_Smith_IBL(NoV, NoL, Roughness);
            float G_Vis = G * VoH / (NoH * NoV);
            float Fc = pow(1 - VoH, 5);
            A += (1 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }

        RWTexture2D<float2> dest = g_uavBindless2DTextures[g_computeConstants.outputUavIndex];
        dest[uint2(dispatchThreadId.x, dispatchThreadId.y)] = float2(A, B) / NumSamples;
    }
}