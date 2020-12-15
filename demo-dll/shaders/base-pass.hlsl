struct FrameCbLayout
{
	uint sceneIndexBufferBindlessIndex;
	uint scenePositionBufferBindlessIndex;
	uint sceneNormalBufferBindlessIndex;
};

struct ViewCbLayout
{
	float4x4 viewTransform;
	float4x4 projectionTransform;
};

struct MeshCbLayout
{
	float4x4 localToWorld;
	uint indexOffset;
	uint positionOffset;
	uint normalOffset;
};

ConstantBuffer<FrameCbLayout> frameConstants : register(b2);
ConstantBuffer<ViewCbLayout> viewConstants : register(b1);
ConstantBuffer<MeshCbLayout> meshConstants : register(b0);
ByteAddressBuffer bindlessBuffers[] : register(t1);

struct vs_to_ps
{
	float4 pos : SV_POSITION;
	float4 normal : INTERPOLATED_WORLD_NORMAL;
};

vs_to_ps vs_main(uint vertexId : SV_VertexID)
{
	vs_to_ps o;

	// size of 4 for 32 bit indices
	uint index = bindlessBuffers[frameConstants.sceneIndexBufferBindlessIndex].Load(4*(vertexId + meshConstants.indexOffset));

	// size of 12 for float3 positions
	float3 position = bindlessBuffers[frameConstants.scenePositionBufferBindlessIndex].Load<float3>(12 * (index + meshConstants.positionOffset));

	// size of 12 for float3 normals
	float3 normal = bindlessBuffers[frameConstants.sceneNormalBufferBindlessIndex].Load<float3>(12 * (index + meshConstants.normalOffset));

	float4 worldPos = mul(float4(position, 1.f), meshConstants.localToWorld);
	float4x4 viewProjTransform = mul(viewConstants.viewTransform, viewConstants.projectionTransform);
	o.pos = mul(worldPos, viewProjTransform);
	o.normal = mul(float4(normal, 0.f), meshConstants.localToWorld);

	return o;
}

float4 ps_main(vs_to_ps input) : SV_Target
{
	float4 lightDir = float4(1, 1, 1, 0);
	return saturate(dot(lightDir, input.normal));
}