#define rootsig \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), " \
    "RootConstants(b0, num32BitConstants=16, visibility = SHADER_VISIBILITY_VERTEX)," \
    "RootConstants(b1, num32BitConstants=1, visibility = SHADER_VISIBILITY_PIXEL)," \
    "DescriptorTable(SRV(t0, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_PIXEL), " \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_MIN_MAG_MIP_LINEAR, maxAnisotropy = 0, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, addressW = TEXTURE_ADDRESS_WRAP, borderColor = STATIC_BORDER_COLOR_TRANSPARENT_BLACK) "

cbuffer vertexBuffer : register(b0) 
{
    float4x4 g_projectionMatrix; 
};
cbuffer pixelBuffer : register(b1)
{
    uint g_textureIndex;
};
SamplerState g_sampler0 : register(s0);
Texture2D g_bindlessTextures[] : register(t0);

struct VS_INPUT
{
    float2 pos : POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};
            
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};
            
PS_INPUT vs_main(VS_INPUT input)
{
    PS_INPUT output;
    output.pos = mul(float4(input.pos.xy, 0.f, 1.f), g_projectionMatrix);
    output.col = input.col;
    output.uv  = input.uv;
    return output;
}

float4 ps_main(PS_INPUT input) : SV_Target
{
    Texture2D texture0 = g_bindlessTextures[g_textureIndex];
    float4 out_col = input.col * texture0.Sample(g_sampler0, input.uv);
    return out_col;
}