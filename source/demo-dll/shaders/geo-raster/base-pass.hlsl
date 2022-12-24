#include "lighting/common.hlsli"
#include "material/common.hlsli"
#include "common/mesh-material.hlsli"
#include "image-based-lighting/spherical-harmonics/common.hlsli"

#define rootsig \
	"RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED )," \
    "RootConstants(b0, num32BitConstants=22, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b1, space = 0, visibility = SHADER_VISIBILITY_ALL)," \
    "CBV(b2, space = 0, visibility = SHADER_VISIBILITY_ALL)," \
	"StaticSampler(s1, space = 1, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE)," \
	"StaticSampler(s2, space = 1, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_ANISOTROPIC, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE)"

struct LightProbeData
{
	int envmapTextureIndex;
	int shTextureIndex;
};

struct FrameCbLayout
{
	float4x4 sceneRotation;
	int sceneMeshAccessorsIndex;
	int sceneMeshBufferViewsIndex;
	int sceneMaterialBufferIndex;
	int envBrdfTextureIndex;
	LightProbeData sceneLightProbe;
	int sceneBvhIndex;
	int lightCount;
	int sceneLightPropertiesBufferIndex;
	int sceneLightIndicesBufferIndex;
	int sceneLightsTransformsBufferIndex;
};

struct ViewCbLayout
{
	float4x4 viewTransform;
	float4x4 projectionTransform;
	float3 eyePos;
	float exposure;
};

struct PrimitiveCbLayout
{
	float4x4 localToWorld;
	int indexAccessor;
	int positionAccessor;
	int uvAccessor;
	int normalAccessor;
	int tangentAccessor;
	int materialIndex;
};

SamplerState g_trilinearSampler : register(s1, space1);
SamplerState g_anisotropicSampler : register(s2, space1);
ConstantBuffer<PrimitiveCbLayout> g_primitiveConstants : register(b0);
ConstantBuffer<ViewCbLayout> g_viewConstants : register(b1);
ConstantBuffer<FrameCbLayout> g_frameConstants : register(b2);

struct vs_to_ps
{
	float4 pos : SV_POSITION;
	float2 uv : INTERPOLATED_UV_0;
	float4 normal : INTERPOLATED_WORLD_NORMAL;
	float4 tangent : INTERPOLATED_WORLD_TANGENT;
	float4 bitangent : INTERPOLATED_WORLD_BITANGENT;
	float4 worldPos : INTERPOLATED_WORLD_POS;
};

vs_to_ps vs_main(uint index : SV_VertexID)
{
	vs_to_ps o;

	float4x4 localToWorld = mul(g_primitiveConstants.localToWorld, g_frameConstants.sceneRotation);
	float4x4 viewProjTransform = mul(g_viewConstants.viewTransform, g_viewConstants.projectionTransform);

	// index
	uint vertIndex = MeshMaterial::GetUint(index, g_primitiveConstants.indexAccessor, g_frameConstants.sceneMeshAccessorsIndex, g_frameConstants.sceneMeshBufferViewsIndex);

	// position
	float3 position = MeshMaterial::GetFloat3(vertIndex, g_primitiveConstants.positionAccessor, g_frameConstants.sceneMeshAccessorsIndex, g_frameConstants.sceneMeshBufferViewsIndex);
	float4 worldPos = mul(float4(position, 1.f), localToWorld);
	o.worldPos = worldPos;
	o.pos = mul(worldPos, viewProjTransform);

	// uv
	o.uv = MeshMaterial::GetFloat2(vertIndex, g_primitiveConstants.uvAccessor, g_frameConstants.sceneMeshAccessorsIndex, g_frameConstants.sceneMeshBufferViewsIndex);

	// normal
	float3 normal = MeshMaterial::GetFloat3(vertIndex, g_primitiveConstants.normalAccessor, g_frameConstants.sceneMeshAccessorsIndex, g_frameConstants.sceneMeshBufferViewsIndex);
	o.normal = mul(float4(normal, 0.f), localToWorld);

	// tangent
	float4 packedTangent = MeshMaterial::GetFloat4(vertIndex, g_primitiveConstants.tangentAccessor, g_frameConstants.sceneMeshAccessorsIndex, g_frameConstants.sceneMeshBufferViewsIndex);
	float3 tangent = packedTangent.xyz;
	o.tangent = mul(float4(tangent, 0.f), localToWorld);

	// bitangent
	float3 bitangent = cross(normal, tangent) * packedTangent.w;
	o.bitangent = mul(float4(bitangent, 0.f), localToWorld);

	return o;
}

float4 ps_main(vs_to_ps input) : SV_Target
{
	FMaterial material = MeshMaterial::GetMaterial(g_primitiveConstants.materialIndex, g_frameConstants.sceneMaterialBufferIndex);
	FMaterialProperties p = EvaluateMaterialProperties(material, input.uv, g_anisotropicSampler);
	clip(p.opacity - 0.5);

#if VIWEMODE == 1 // Lighting Only
	p.basecolor = 0.5.xxx;
#elif VIEWMODE == 2 // Roughness
	return p.roughness.xxxx;
#elif VIEWMODE == 3 // Metallic
	return p.metallic.xxxx;
#elif VIEWMODE == 4 // Base Color
	return float4(p.basecolor, 1.f);
#elif VIEWMODE == 5 // Emissive
	return float4(p.emissive, 1.f);
#endif

	// Tangent space transform
	float3 T = normalize(input.tangent.xyz);
	float3 B = normalize(input.bitangent.xyz);
	float3 N = normalize(input.normal.xyz);
	float3x3 tangentToWorld = float3x3(T, B, N);

	if (p.bHasNormalmap)
	{
		N = normalize(mul(p.normalmap, tangentToWorld));
	}

	float3 V = normalize(g_viewConstants.eyePos - input.worldPos.xyz / input.worldPos.w);
	float3 radiance = p.emissive * 20000;

#if VIEWMODE == 7 // Reflections
	if (g_frameConstants.sceneLightProbe.envmapTextureIndex != -1)
	{
		TextureCube prefilteredEnvMap = ResourceDescriptorHeap[g_frameConstants.sceneLightProbe.envmapTextureIndex];
		float3 R = normalize(reflect(-V, N));
		float3 reflectionColor = prefilteredEnvMap.SampleLevel(g_trilinearSampler, R, 0).rgb;
		return float4(reflectionColor, 0.f);
	}
#endif
	
#if DIRECT_LIGHTING
	RaytracingAccelerationStructure sceneBvh = ResourceDescriptorHeap[g_frameConstants.sceneBvhIndex];

	ByteAddressBuffer lightIndicesBuffer = ResourceDescriptorHeap[g_frameConstants.sceneLightIndicesBufferIndex];
	ByteAddressBuffer lightPropertiesBuffer = ResourceDescriptorHeap[g_frameConstants.sceneLightPropertiesBufferIndex];
	ByteAddressBuffer lightTransformsBuffer = ResourceDescriptorHeap[g_frameConstants.sceneLightsTransformsBufferIndex];
	for (int lightIndex = 0; lightIndex < g_frameConstants.lightCount; ++lightIndex)
	{
		int lightId = lightIndicesBuffer.Load<int>(lightIndex * sizeof(int));
		FLight light = lightPropertiesBuffer.Load<FLight>(lightId * sizeof(FLight));
		float4x4 lightTransform = lightTransformsBuffer.Load<float4x4>(lightId * sizeof(float4x4));
		radiance += GetDirectRadiance(light, lightTransform, input.worldPos.xyz, p.basecolor, p.metallic, p.roughness, N, V, sceneBvh);
	}
#endif

	// Diffuse IBL
#if DIFFUSE_IBL
	if (g_frameConstants.sceneLightProbe.shTextureIndex != -1)
	{
		SH9Color shRadiance;
		Texture2D shTex = ResourceDescriptorHeap[g_frameConstants.sceneLightProbe.shTextureIndex];

		[UNROLL]
		for (int i = 0; i < SH_COEFFICIENTS; ++i)
		{
			shRadiance.c[i] = shTex.Load(int3(i, 0, 0)).rgb;
		}

		float3 albedo = (1.f - p.metallic) * (1.f - p.transmission) * p.basecolor;
		float3 shDiffuse = /*(1.f - F) * */albedo * Fd_Lambert() * ShIrradiance(N, shRadiance);
		radiance += lerp(shDiffuse, p.ao * shDiffuse, p.aoblend);
	}
#endif

	// Specular IBL
#if SPECULAR_IBL
	if (g_frameConstants.sceneLightProbe.envmapTextureIndex != -1 &&
		g_frameConstants.envBrdfTextureIndex != -1)
	{
		TextureCube prefilteredEnvMap = ResourceDescriptorHeap[g_frameConstants.sceneLightProbe.envmapTextureIndex];
		Texture2D envBrdfTex = ResourceDescriptorHeap[g_frameConstants.envBrdfTextureIndex];

		float texWidth, texHeight, mipCount;
		prefilteredEnvMap.GetDimensions(0, texWidth, texHeight, mipCount);

		// FIXME - The env BRDF texture has a few lines of black near (0,0). So, apply a threshhold here for now.
		float NoV = max(dot(N, V), 0.01);
		float3 F0 = p.metallic * p.basecolor + (1.f - p.metallic) * 0.04;
		float3 R = normalize(reflect(-V, N));
		float3 prefilteredColor = prefilteredEnvMap.SampleLevel(g_trilinearSampler, R, p.roughness * (mipCount - 1)).rgb;
		float2 envBrdf = envBrdfTex.SampleLevel(g_trilinearSampler, float2(NoV, p.roughness), 0.f).rg;
		float3 specularIBL = prefilteredColor * (F0 * envBrdf.x + envBrdf.y);
		radiance += lerp(specularIBL, p.ao * specularIBL, p.aoblend);
	}
#endif

	return float4(radiance, 0.f);
}