#pragma once

#include <SimpleMath.h>
#include <ppltasks.h>
#include <mesh-material.h>
#include <spookyhash_api.h>
using namespace DirectX::SimpleMath;

namespace tinygltf
{
	class Model;
	class Image;
	class Sampler;
}

class FController;

// Corresponds to GLTF Primitive
struct FMeshPrimitive
{
	int m_indexAccessor;
	int m_positionAccessor;
	int m_uvAccessor;
	int m_normalAccessor;
	int m_tangentAccessor;
	size_t m_indexCount;
	D3D_PRIMITIVE_TOPOLOGY m_topology;
	int m_materialIndex;
};

// Corresponds to GLTF Mesh
struct FMesh
{
	std::wstring m_name;
	std::vector<FMeshPrimitive> m_primitives;
};

// SOA struct for scene entities
struct FSceneEntities
{
	std::vector<FMesh> m_meshList;
	std::vector<Matrix> m_transformList;
	std::vector<DirectX::BoundingBox> m_objectSpaceBoundsList;

	void Resize(const size_t count)
	{
		m_meshList.resize(count);
		m_transformList.resize(count);
		m_objectSpaceBoundsList.resize(count);
	}

	void Clear()
	{
		m_meshList.clear();
		m_transformList.clear();
		m_objectSpaceBoundsList.clear();
	}

	size_t GetScenePrimitiveCount() const
	{
		size_t primCount = 0;
		for (const auto& mesh : m_meshList)
		{
			primCount += mesh.m_primitives.size();
		}
		return primCount;
	}
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
	void LoadNode(int nodeIndex, tinygltf::Model& model, const Matrix& transform);
	void LoadMesh(int meshIndex, const tinygltf::Model& model, const Matrix& transform);
	void LoadCamera(int meshIndex, const tinygltf::Model& model, const Matrix& transform);
	void Clear();

	// Scene files
	std::wstring m_modelFilename = {};
	std::wstring m_environmentFilename = {};
	std::string m_textureCachePath = {};
	std::string m_modelCachePath = {};

	// Scene entity lists
	FSceneEntities m_entities;
	std::vector<FCamera> m_cameras;

	// Scene geo
	std::vector<std::unique_ptr<FBindlessShaderResource>> m_meshBuffers;
	std::unique_ptr<FBindlessShaderResource> m_packedMeshBufferViews;
	std::unique_ptr<FBindlessShaderResource> m_packedMeshAccessors;
	std::unique_ptr<FBindlessShaderResource> m_packedPrimitives;
	std::unique_ptr<FBindlessShaderResource> m_packedPrimitiveCounts;
	std::unordered_map<std::wstring, std::unique_ptr<FBindlessShaderResource>> m_blasList;
	std::unique_ptr<FBindlessShaderResource> m_tlas;
	std::unique_ptr<FBindlessShaderResource> m_packedMaterials;
	DirectX::BoundingBox m_sceneBounds; // world space

	// Image based lighting
	FLightProbe m_globalLightProbe;

	// Transform
	Matrix m_rootTransform;

private:
	void LoadMeshBuffers(const tinygltf::Model& model);
	void LoadMeshBufferViews(const tinygltf::Model& model);
	void LoadMeshAccessors(const tinygltf::Model& model);
	void CreateAccelerationStructures(const tinygltf::Model& model);
	void CreateGpuPrimitiveBuffers();
	void LoadMaterials(const tinygltf::Model& model);
	FMaterial LoadMaterial(const tinygltf::Model& model, const int materialIndex);
	int LoadTexture(const tinygltf::Image& image, const DXGI_FORMAT srcFormat = DXGI_FORMAT_UNKNOWN, const DXGI_FORMAT compressedFormat = DXGI_FORMAT_UNKNOWN);
	std::pair<int, int> PrefilterNormalRoughnessTextures(const tinygltf::Image& normalmap, const tinygltf::Image& metallicRoughnessmap);
	void ProcessReadbackTexture(FResourceReadbackContext* context, const std::string& filename, const int width, const int height, const size_t mipCount, const DXGI_FORMAT fmt, const int bpp);

private:
	std::vector<concurrency::task<void>> m_loadingJobs;
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
	bool IsRenderingPaused();
	const FScene* GetScene();
	const FView* GetView();
	uint32_t GetEnvBrdfSrvIndex();
	std::unique_ptr<FBindlessShaderResource> GenerateEnvBrdfTexture(const uint32_t width, const uint32_t height);
}