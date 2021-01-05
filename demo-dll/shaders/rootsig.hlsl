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

#define rootsig \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_ANISOTROPIC, maxAnisotropy = 8, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE), " \
    "StaticSampler(s1, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, comparisonFunc = COMPARISON_LESS_EQUAL, addressU = TEXTURE_ADDRESS_BORDER, addressV = TEXTURE_ADDRESS_BORDER, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE), " \
    "RootConstants(b0, num32BitConstants=20, visibility = SHADER_VISIBILITY_VERTEX)," \
    "CBV(b1, space = 0, visibility = SHADER_VISIBILITY_PIXEL"), \
    "CBV(b2, space = 0, visibility = SHADER_VISIBILITY_ALL"), \
    "CBV(b3, space = 0, visibility = SHADER_VISIBILITY_ALL"), \
    "DescriptorTable(SRV(t0, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_PIXEL), " \
    "DescriptorTable(SRV(t1, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_VERTEX), " \
    "DescriptorTable(SRV(t2, space = 1, numDescriptors = 1000), visibility = SHADER_VISIBILITY_PIXEL) "


SamplerState anisoSampler : register(s0);
ConstantBuffer<MeshCbLayout> meshConstants : register(b0);
ConstantBuffer<MaterialCbLayout> materialConstants : register(b1);
ConstantBuffer<ViewCbLayout> viewConstants : register(b2);
ConstantBuffer<FrameCbLayout> frameConstants : register(b3);
Texture2D bindless2DTextures[] : register(t0, space0);
ByteAddressBuffer bindlessBuffers[] : register(t1, space0);
TextureCube bindlessCubeTextures[] : register(t2, space1);
