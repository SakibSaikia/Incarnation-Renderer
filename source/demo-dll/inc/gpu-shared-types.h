#pragma once

#ifndef __cplusplus
	#define uint32_t	uint
	#define Vector2		float2
	#define Vector3		float3
	#define Vector4		float4
	#define Matrix		float4x4
	#define Color		float4
#else
	#include <SimpleMath.h>
	using namespace DirectX::SimpleMath;
#endif

namespace SpecialDescriptors
{
	enum Type
	{
		RenderStatsBufferUavIndex,
		DebugDrawIndirectPrimitiveArgsUavIndex,
		DebugDrawIndirectPrimitiveCountUavIndex,
		DebugDrawIndirectLineArgsUavIndex,
		DebugDrawIndirectLineCountUavIndex,
		DebugPrimitiveIndexCountSrvIndex,
		Count
	};
};

#ifdef __cplusplus
static_assert(SpecialDescriptors::Count < 100, "See reserved descriptors in enum class DescriptorRange");
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

namespace DebugShape
{
	enum Type
	{
		Cube,
		Icosphere,
		Sphere,
		Cylinder,
		Cone,
		Plane,
		Count
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
	Vector4 m_boundingSphere;
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

// Global scene constants
struct FSceneConstants
{
	Matrix m_sceneRotation;
	Vector3 m_sunDir;
	uint32_t m_primitiveCount;
	uint32_t m_sceneMeshAccessorsIndex;
	uint32_t m_sceneMeshBufferViewsIndex;
	uint32_t m_scenePrimitivesIndex;
	uint32_t m_sceneMaterialBufferIndex;
	uint32_t m_lightCount;
	uint32_t m_packedLightIndicesBufferIndex;
	uint32_t m_packedLightTransformsBufferIndex;
	uint32_t m_packedGlobalLightPropertiesBufferIndex;
	uint32_t m_sceneBvhIndex;
	uint32_t m_envmapTextureIndex;
	uint32_t m_skylightProbeIndex;
	uint32_t m_envBrdfTextureIndex;
	uint32_t m_sunIndex;
};

// Global view constants
struct FViewConstants
{
	Matrix m_viewTransform;
	Matrix m_projTransform;
	Matrix m_viewProjTransform;
	Matrix m_invViewProjTransform;
	Matrix m_invViewProjTransform_ParallaxCorrected;
	Matrix m_prevViewProjTransform;
	Matrix m_invProjTransform;
	Matrix m_cullViewProjTransform;
	Vector3 m_eyePos;
	float m_exposure;
	float m_aperture;
	float m_focalLength;
	float m_nearPlane;
	uint32_t m_resX;
	uint32_t m_resY;
	uint32_t m_mouseX;
	uint32_t m_mouseY;
	uint32_t m_viewmode;
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

// Corresponds to D3D12_DRAW_ARGUMENTS
struct FDrawInstanced
{
	uint32_t m_vertexCount;
	uint32_t m_instanceCount;
	uint32_t m_startVertexLocation;
	uint32_t m_startInstanceLocation;
};

struct FIndirectDrawWithRootConstants
{
	uint32_t m_rootConstants[32];
	FDrawInstanced m_drawArguments;

#ifdef __cplusplus
	static D3DCommandSignature_t* GetCommandSignature(D3DRootSignature_t* rootsig)
	{
		D3D12_INDIRECT_ARGUMENT_DESC argumentDescs[2] = {};
		argumentDescs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
		argumentDescs[0].Constant.RootParameterIndex = 0;
		argumentDescs[0].Constant.DestOffsetIn32BitValues = 0;
		argumentDescs[0].Constant.Num32BitValuesToSet = 32;
		argumentDescs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

		D3D12_COMMAND_SIGNATURE_DESC commandSignatureDesc = {};
		commandSignatureDesc.pArgumentDescs = argumentDescs;
		commandSignatureDesc.NumArgumentDescs = 2;
		commandSignatureDesc.ByteStride = sizeof(FIndirectDrawWithRootConstants);

		return RenderBackend12::CacheCommandSignature(commandSignatureDesc, rootsig);
	}
#endif
};

struct FRenderStatsBuffer
{
	int m_culledPrimitives;
	int m_culledLights;
};

struct FDebugDrawCmd
{
	Color m_color;
	Matrix m_transform;
	uint32_t m_shapeType;
	bool m_persistent;
};

// Used for Preetham sky model
struct FPerezDistribution
{
	Vector4 A, B, C, D, E;
};