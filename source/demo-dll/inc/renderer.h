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
	std::vector<FMeshPrimitive> m_primitives;
};

// SOA struct for scene entities
template<class T>
struct TSceneEntities
{
	std::vector<T> m_entityList;
	std::vector<std::string> m_entityNames;
	std::vector<Matrix> m_transformList;
	std::vector<DirectX::BoundingBox> m_objectSpaceBoundsList;

	void Resize(const size_t count)
	{
		m_entityList.resize(count);
		m_entityNames.resize(count);
		m_transformList.resize(count);
		m_objectSpaceBoundsList.resize(count);
	}

	void Clear()
	{
		m_entityList.clear();
		m_entityNames.clear();
		m_transformList.clear();
		m_objectSpaceBoundsList.clear();
	}
};

struct FCamera
{
	std::string m_name;
	Matrix m_viewTransform;
	Matrix m_projectionTransform;
};

struct FLight
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

	Type m_type;
	Vector3 m_color;
	float m_intensity;
	float m_range;
	Vector2 m_spotAngles;
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
	using FSceneMeshEntities = TSceneEntities<FMesh>;
	using FSceneLightEntities = TSceneEntities<int>;
	FSceneMeshEntities m_sceneMeshes;
	FSceneLightEntities m_sceneLights;
	std::vector<FCamera> m_cameras;

	// Scene geo
	std::vector<std::unique_ptr<FBindlessShaderResource>> m_meshBuffers;
	std::unique_ptr<FBindlessShaderResource> m_packedMeshBufferViews;
	std::unique_ptr<FBindlessShaderResource> m_packedMeshAccessors;
	std::unique_ptr<FBindlessShaderResource> m_packedPrimitives;
	std::unique_ptr<FBindlessShaderResource> m_packedPrimitiveCounts;
	std::unordered_map<std::string, std::unique_ptr<FBindlessShaderResource>> m_blasList;
	std::unique_ptr<FBindlessShaderResource> m_tlas;
	std::unique_ptr<FBindlessShaderResource> m_packedMaterials;
	std::vector<FMaterial> m_materialList;
	DirectX::BoundingBox m_sceneBounds; // world space

	// Lights
	std::vector<FLight> m_lights;
	std::unique_ptr<FBindlessShaderResource> m_packedLightProperties;
	std::unique_ptr<FBindlessShaderResource> m_packedLightIndices;
	std::unique_ptr<FBindlessShaderResource> m_packedLightTransforms;
	FLightProbe m_environmentSky;

	// Transform
	Matrix m_rootTransform;

private:
	void LoadMeshBuffers(const tinygltf::Model& model);
	void LoadMeshBufferViews(const tinygltf::Model& model);
	void LoadMeshAccessors(const tinygltf::Model& model);
	void LoadLights(const tinygltf::Model& model);
	void CreateAccelerationStructures(const tinygltf::Model& model);
	void CreateGpuPrimitiveBuffers();
	void CreateGpuLightBuffers();
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
	bool IsRenderingSuspended();
	FScene* GetScene();
	const FView* GetView();
	void ResetPathtraceAccumulation();
	void InitializeRenderer(const uint32_t resX, const uint32_t resY);
	void TeardownRenderer();
}