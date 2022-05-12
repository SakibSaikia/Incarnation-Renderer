#ifndef __MATERIAL_COMMON_HLSLI_
#define __MATERIAL_COMMON_HLSLI_

#if NO_UV_DERIVATIVES
	#define TEX_SAMPLE(t,s,uv) (t.SampleLevel(s, uv, 0))
#else
	#define TEX_SAMPLE(t,s,uv) (t.Sample(s,uv))
#endif

struct FMaterialProperties
{
	float3 emissive;
	float3 basecolor;
	bool bHasNormalmap;
	float3 normalmap;
	float metallic;
	float roughness;
	float ao;
	float aoblend;
	float transmission;
	float clearcoat;
	float clearcoatRoughness;
	bool bHasClearcoatNormalmap;
	float3 clearcoatNormalmap;
};

FMaterialProperties EvaluateMaterialProperties(FMaterial mat, float2 uv, SamplerState s)
{
	FMaterialProperties output;

//#if !RAY_TRACING
//	// Alpha clip
//	if (mat.m_baseColorTextureIndex != -1)
//	{
//		Texture2D baseColorTex = ResourceDescriptorHeap[mat.m_baseColorTextureIndex];
//		float alpha = TEX_SAMPLE(baseColorTex, s, uv, ddx, ddy).a;
//		clip(alpha - 0.5);
//	}
//#endif

	// Emissive
	output.emissive = mat.m_emissiveFactor;
	if (mat.m_emissiveTextureIndex != -1)
	{
		Texture2D emissiveTex = ResourceDescriptorHeap[NonUniformResourceIndex(mat.m_emissiveTextureIndex)];
		output.emissive *= TEX_SAMPLE(emissiveTex, s, uv).rgb;
	}

	// Base Color
	output.basecolor = mat.m_baseColorFactor;
	if (mat.m_baseColorTextureIndex != -1)
	{
		Texture2D baseColorTex = ResourceDescriptorHeap[NonUniformResourceIndex(mat.m_baseColorTextureIndex)];
		output.basecolor *= TEX_SAMPLE(baseColorTex, s, uv).rgb;
	}

	// Normalmap
	output.bHasNormalmap = mat.m_normalTextureIndex != -1;
	if (output.bHasNormalmap)
	{
		Texture2D normalmapTex = ResourceDescriptorHeap[NonUniformResourceIndex(mat.m_normalTextureIndex)];
		float2 normalXY = TEX_SAMPLE(normalmapTex, s, uv).rg;
		float normalZ = sqrt(1.f - dot(normalXY, normalXY));
		output.normalmap = float3(normalXY, normalZ);
	}

	// Metallic/Roughness
	// GLTF specifies metalness in blue channel and roughness in green channel but we swizzle them on import and
	// use a BC5 texture. So, metalness ends up in the red channel and roughness stays on the green channel.
	output.metallic = mat.m_metallicFactor;
	output.roughness = mat.m_roughnessFactor;
	if (mat.m_metallicRoughnessTextureIndex != -1)
	{
		Texture2D metallicRoughnessTex = ResourceDescriptorHeap[NonUniformResourceIndex(mat.m_metallicRoughnessTextureIndex)];
		float2 metallicRoughnessMap = TEX_SAMPLE(metallicRoughnessTex, s, uv).rg;
		output.metallic = metallicRoughnessMap.x;
		output.roughness = metallicRoughnessMap.y;
	}

	// Ambient Occlusion
	output.aoblend = mat.m_aoStrength;
	output.ao = 1.f;
	if (mat.m_aoTextureIndex != -1)
	{
		Texture2D aoTex = ResourceDescriptorHeap[NonUniformResourceIndex(mat.m_aoTextureIndex)];
		output.ao = TEX_SAMPLE(aoTex, s, uv).r;
	}

	// Transmission
	output.transmission = mat.m_transmissionFactor;
	if (mat.m_transmissionTextureIndex != -1)
	{
		Texture2D transmissionTex = ResourceDescriptorHeap[NonUniformResourceIndex(mat.m_transmissionTextureIndex)];
		output.transmission *= TEX_SAMPLE(transmissionTex, s, uv).r;
	}

	// Clearcoat
	output.clearcoat = mat.m_clearcoatFactor;
	if (mat.m_clearcoatTextureIndex != -1)
	{
		Texture2D clearcoatTex = ResourceDescriptorHeap[NonUniformResourceIndex(mat.m_clearcoatTextureIndex)];
		output.clearcoat *= TEX_SAMPLE(clearcoatTex, s, uv).r;
	}

	output.clearcoatRoughness = mat.m_clearcoatRoughnessFactor;
	if (mat.m_clearcoatRoughnessTextureIndex != -1)
	{
		Texture2D clearcoatRoughnessTex = ResourceDescriptorHeap[NonUniformResourceIndex(mat.m_clearcoatRoughnessTextureIndex)];
		output.clearcoatRoughness *= TEX_SAMPLE(clearcoatRoughnessTex, s, uv).r;
	}

	output.bHasClearcoatNormalmap = mat.m_clearcoatNormalTextureIndex != -1;
	if (output.bHasClearcoatNormalmap)
	{
		Texture2D normalmapTex = ResourceDescriptorHeap[NonUniformResourceIndex(mat.m_clearcoatNormalTextureIndex)];
		float2 normalXY = TEX_SAMPLE(normalmapTex, s, uv).rg;
		float normalZ = sqrt(1.f - dot(normalXY, normalXY));
		output.clearcoatNormalmap = float3(normalXY, normalZ);
	}

	return output;
}

#endif // __MATERIAL_COMMON_HLSLI_