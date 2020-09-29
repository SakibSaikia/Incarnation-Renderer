cbuffer vertexBuffer : register(b0) 
{
    float4x4 ProjectionMatrix; 
};
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
    output.pos = mul(float4(input.pos.xy, 0.f, 1.f), ProjectionMatrix);
    output.col = input.col;
    output.uv  = input.uv;
    return output;
}

cbuffer pixelBuffer : register(b1)
{
    uint srvIndex;
};

SamplerState sampler0 : register(s0);

Texture2D bindlessShaderResources[] : register(t0);

float4 ps_main(PS_INPUT input) : SV_Target
{
    Texture2D texture0 = bindlessShaderResources[srvIndex];
    float4 out_col = input.col * texture0.Sample(sampler0, input.uv);
    return out_col; 
}