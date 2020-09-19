
float4 vs_main(uint vertexID : SV_VertexID) : SV_Position
{
	if (vertexID == 0)
	{
		return float4(0.f, 0.5f, 0.f, 1.f);
	}
	else if (vertexID == 1)
	{
		return float4(-0.5f, -0.5f, 0.f, 1.f);
	}
	else
	{
		return float4(-0.5f, -0.5f, 0.f, 1.f);
	}
}

float4 ps_main() : SV_Target
{
	return 1.xxxx;
}