#pragma once

#include <SimpleMath.h>
using namespace DirectX::SimpleMath;

namespace tinygltf
{
	class Model;
	class Image;
	class Sampler;
}

class FController;

struct FRenderMesh
{
	std::string m_name;
	size_t m_indexCount;
	uint32_t m_indexOffset;
	uint32_t m_positionOffset;
	uint32_t m_uvOffset;
	uint32_t m_normalOffset;
	uint32_t m_tangentOffset;
	uint32_t m_bitangentOffset;

	std::string m_materialName;
	Vector3 m_emissiveFactor;
	Vector3 m_baseColorFactor;
	float m_metallicFactor;
	float m_roughnessFactor;
	float m_aoStrength;

	int m_emissiveTextureIndex;
	int m_baseColorTextureIndex;
	int m_metallicRoughnessTextureIndex;
	int m_normalTextureIndex;
	int m_aoTextureIndex;

	int m_emissiveSamplerIndex;
	int m_baseColorSamplerIndex;
	int m_metallicRoughnessSamplerIndex;
	int m_normalSamplerIndex;
	int m_aoSamplerIndex;
};

struct FCamera
{
	std::string m_name;
	Matrix m_viewTransform;
	Matrix m_projectionTransform;
};

struct FLightProbe
{
	int m_envmapTextureIndex;
	int m_shTextureIndex;
};

struct FScene
{
	void ReloadModel(const std::wstring& gltfFilename);
	void ReloadEnvironment(const std::wstring& hdriFilename);
	bool LoadNode(int nodeIndex, tinygltf::Model& model, const Matrix& transform);
	void LoadMesh(int meshIndex, const tinygltf::Model& model, const Matrix& transform);
	void LoadCamera(int meshIndex, const tinygltf::Model& model, const Matrix& transform);
	void Clear();

	// Scene files
	std::wstring m_modelFilename = {};
	std::wstring m_environmentFilename = {};
	std::string m_textureCachePath = {};
	std::string m_modelCachePath = {};

	// Scene entity lists
	std::vector<FRenderMesh> m_meshGeo;
	std::vector<Matrix> m_meshTransforms;
	std::vector<DirectX::BoundingBox> m_meshBounds; // object space
	std::vector<FCamera> m_cameras;

	// Scene geo
	std::unique_ptr<FBindlessShaderResource> m_meshIndexBuffer;
	std::unique_ptr<FBindlessShaderResource> m_meshPositionBuffer;
	std::unique_ptr<FBindlessShaderResource> m_meshUvBuffer;
	std::unique_ptr<FBindlessShaderResource> m_meshNormalBuffer;
	std::unique_ptr<FBindlessShaderResource> m_meshTangentBuffer;
	std::unique_ptr<FBindlessShaderResource> m_meshBitangentBuffer;
	DirectX::BoundingBox m_sceneBounds; // world space

	// Image based lighting
	FLightProbe m_globalLightProbe;

	// Transform
	Matrix m_rootTransform;

private:
	int LoadTexture(const tinygltf::Image& image, const bool srgb);

private:
	uint8_t* m_scratchIndexBuffer;
	uint8_t* m_scratchPositionBuffer;
	uint8_t* m_scratchUvBuffer;
	uint8_t* m_scratchNormalBuffer;
	uint8_t* m_scratchTangentBuffer;
	uint8_t* m_scratchBitangentBuffer;

	size_t m_scratchIndexBufferOffset;
	size_t m_scratchPositionBufferOffset;
	size_t m_scratchUvBufferOffset;
	size_t m_scratchNormalBufferOffset;
	size_t m_scratchTangentBufferOffset;
	size_t m_scratchBitangentBufferOffset;
};

struct FView
{
	void Tick(const float deltaTime, const FController* controller);
	void Reset(const FScene* scene = nullptr);

	Vector3 m_position;
	Vector3 m_right;
	Vector3 m_up;
	Vector3 m_look;
	float m_fov;

	Matrix m_viewTransform;
	Matrix m_projectionTransform;

private:
	void UpdateViewTransform();
};

namespace Demo
{
	const FScene* GetScene();
	const FView* GetView();
	uint32_t GetEnvBrdfSrvIndex();
	std::unique_ptr<FBindlessShaderResource> GenerateEnvBrdfTexture(const uint32_t width, const uint32_t height);
}