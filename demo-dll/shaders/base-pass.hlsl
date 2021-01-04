struct FrameCbLayout
{
	float4x4 sceneRotation;
	uint sceneIndexBufferBindlessIndex;
	uint scenePositionBufferBindlessIndex;
	uint sceneNormalBufferBindlessIndex;
	uint sceneUvBufferBindlessIndex;
	int envmapTextureIndex;
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
	uint uvOffset;
};

struct MaterialCbLayout
{
	float3 emissiveFactor;
	float metallicFactor;
	float3 baseColorFactor;
	float roughnessFactor;
	int baseColorTextureIndex;
	int metallicRoughnessTextureIndex;
	int normalTextureIndex;
	int baseColorSamplerIndex;
	int metallicRoughnessSamplerIndex;
	int normalSamplerIndex;
};

ConstantBuffer<FrameCbLayout> frameConstants : register(b3);
ConstantBuffer<ViewCbLayout> viewConstants : register(b2);
ConstantBuffer<MeshCbLayout> meshConstants : register(b0);
ByteAddressBuffer bindlessBuffers[] : register(t1,space0);

struct vs_to_ps
{
	float4 pos : SV_POSITION;
	float4 normal : INTERPOLATED_WORLD_NORMAL;
	float2 uv : INTERPOLATED_UV_0;
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

	// size of 8 for float2 uv's
	float2 uv = bindlessBuffers[frameConstants.sceneUvBufferBindlessIndex].Load<float2>(8 * (index + meshConstants.uvOffset));

	float4x4 localToWorld = mul(meshConstants.localToWorld, frameConstants.sceneRotation);
	float4 worldPos = mul(float4(position, 1.f), localToWorld);
	float4x4 viewProjTransform = mul(viewConstants.viewTransform, viewConstants.projectionTransform);
	o.pos = mul(worldPos, viewProjTransform);
	o.normal = mul(float4(normal, 0.f), meshConstants.localToWorld);
	o.uv = uv;

	return o;
}

Texture2D bindless2DTextures[] : register(t0,space0);
TextureCube bindlessCubeTextures[] : register(t2,space1);
SamplerState anisoSampler : register(s0);
ConstantBuffer<MaterialCbLayout> materialConstants : register(b1);

float4 ps_main(vs_to_ps input) : SV_Target
{
	float4 normal = normalize(input.normal);
	float4 lightDir = float4(1, 1, -1, 0);
	float3 baseColor = bindless2DTextures[materialConstants.baseColorTextureIndex].Sample(anisoSampler, input.uv).rgb;
	baseColor *= bindlessCubeTextures[frameConstants.envmapTextureIndex].Sample(anisoSampler, normal.xyz).rgb;

	return saturate(dot(lightDir, normal)) * float4(baseColor.rgb, 0.f);
}