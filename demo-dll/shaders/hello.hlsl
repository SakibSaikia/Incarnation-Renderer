struct vs_to_ps
{
	float4 color : COLOR0;
	float4 pos : SV_POSITION;
};


vs_to_ps vs_main(uint vertexID : SV_VertexID)
{
	vs_to_ps o;

	if (vertexID == 0)
	{
		o.pos = float4(0.f, 0.5f, 0.f, 1.f);
		o.color = float4(1.f, 0.f, 0.f, 1.f);
	}
	else if (vertexID == 1)
	{
		o.pos = float4(0.5f, -0.5f, 0.f, 1.f);
		o.color = float4(0.f, 1.f, 0.f, 1.f);
	}
	else
	{
		o.pos = float4(-0.5f, -0.5f, 0.f, 1.f);
		o.color = float4(0.f, 0.f, 1.f, 1.f);
	}

	return o;
}

float4 ps_main(vs_to_ps input) : SV_Target
{
	return input.color;
}