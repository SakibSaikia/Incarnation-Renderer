#pragma once

#ifndef __cplusplus
	#define uint32_t	uint
	#define Vector2		float2
	#define Vector3		float3
	#define Vector4		float4
	#define Matrix		float4x4
#else
	#include <SimpleMath.h>
	using namespace DirectX::SimpleMath;
#endif

namespace AlphaMode
{
	enum Type
	{
		Opaque,
		Masked,
		Blend
	};
}


// Corresponds to GLTF BufferView
struct FMeshBufferView
{
	int m_bufferSrvIndex;
	uint32_t m_byteOffset;
	uint32_t m_byteLength;
};

// Corresponds to GLTF Accessor
struct FMeshAccessor
{
	int m_bufferViewIndex;
	uint32_t m_byteOffset;
	uint32_t m_byteStride;
};

struct FGpuPrimitive
{
	Matrix m_localToWorld;
	int m_indexAccessor;
	int m_positionAccessor;
	int m_uvAccessor;
	int m_normalAccessor;
	int m_tangentAccessor;
	int m_materialIndex;
	int m_indicesPerTriangle;
	int m_indexCount;
};

struct FMaterial
{
	Vector3 m_emissiveFactor;
	Vector3 m_baseColorFactor;
	float m_metallicFactor;
	float m_roughnessFactor;
	float m_aoStrength;
	float m_transmissionFactor;
	float m_clearcoatFactor;
	float m_clearcoatRoughnessFactor;

	int m_emissiveTextureIndex;
	int m_baseColorTextureIndex;
	int m_metallicRoughnessTextureIndex;
	int m_normalTextureIndex;
	int m_aoTextureIndex;
	int m_transmissionTextureIndex;
	int m_clearcoatTextureIndex;
	int m_clearcoatRoughnessTextureIndex;
	int m_clearcoatNormalTextureIndex;

	int m_emissiveSamplerIndex;
	int m_baseColorSamplerIndex;
	int m_metallicRoughnessSamplerIndex;
	int m_normalSamplerIndex;
	int m_aoSamplerIndex;
	int m_transmissionSamplerIndex;
	int m_clearcoatSamplerIndex;
	int m_clearcoatRoughnessSamplerIndex;
	int m_clearcoatNormalSamplerIndex;

	int m_alphaMode;
	bool m_doubleSided;
};

namespace Light
{
	enum Type : int
	{
		Directional,
		Point,
		Spot,
		Sphere,
		Disk,
		Rect
	};
}

struct FLight
{
	int m_type;
	Vector3 m_color;
	float m_intensity;
	float m_range;
	Vector2 m_spotAngles;
};

struct FDrawInstanced
{
	uint32_t VertexCount;
	uint32_t InstanceCount;
	uint32_t StartVertexLocation;
	uint32_t StartInstanceLocation;
};

struct FDrawWithRootConstants
{
	uint32_t rootConstants[32];
	FDrawInstanced drawArguments;
};
