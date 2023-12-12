#pragma once

#include <tiny_gltf.h>
#include <mesh-utils.h>

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
	DirectX::BoundingSphere m_boundingSphere;
	std::vector<FInlineMeshlet> m_meshlets;
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
	std::vector<uint32_t> m_visibleList;
	std::vector<Matrix> m_transformList;
	std::vector<DirectX::BoundingBox> m_objectSpaceBoundsList;
	size_t GetCount() const { return m_entityList.size(); }
	void Clear()
	{
		m_entityList.clear();
		m_entityNames.clear();
		m_visibleList.clear();
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

struct FLightProbe
{
	int m_envmapTextureIndex;
	int m_shTextureIndex;
};

struct FModelLoader
{
	std::vector<std::unique_ptr<FShaderBuffer>> m_meshBuffers;
	std::unique_ptr<FShaderBuffer> m_packedMeshBufferViews;
	std::unique_ptr<FShaderBuffer> m_packedMeshAccessors;

protected:
	void LoadMeshBuffers(const tinygltf::Model& model);
	void LoadMeshBufferViews(const tinygltf::Model& model);
	void LoadMeshAccessors(const tinygltf::Model& model);
};

struct FScene : public FModelLoader
{
	void ReloadModel(const std::wstring& gltfFilename);
	void ReloadEnvironment(const std::wstring& hdriFilename);
	void LoadNode(int nodeIndex, tinygltf::Model& model, const Matrix& transform = Matrix::Identity);
	void LoadMesh(int meshIndex, const tinygltf::Model& model, const Matrix& transform);
	void LoadCamera(int meshIndex, const tinygltf::Model& model, const Matrix& transform);
	void Clear();
	int GetDirectionalLight() const;
	void UpdateSunDirection();
	void UpdateDynamicSky(bool bUseAsyncCompute = false);
	size_t GetPunctualLightCount() const;

	// Scene files
	std::wstring m_modelFilename = {};
	std::wstring m_hdriFilename = {};
	std::string m_textureCachePath = {};
	std::string m_modelCachePath = {};

	// Scene entity lists
	using FSceneMeshEntities = TSceneEntities<FMesh>;
	using FSceneLightEntities = TSceneEntities<int>;
	FSceneMeshEntities m_sceneMeshes;
	FSceneMeshEntities m_sceneMeshDecals;
	FSceneLightEntities m_sceneLights;
	std::unique_ptr<FShaderBuffer> m_packedLightIndices; // Index into global light list
	std::vector<FCamera> m_cameras;

	// Scene geo
	std::unique_ptr<FShaderBuffer> m_packedPrimitives;
	std::unique_ptr<FShaderBuffer> m_packedPrimitiveCounts;
	std::unique_ptr<FShaderBuffer> m_packedMeshTransforms;
	std::unordered_map<std::string, std::unique_ptr<FShaderBuffer>> m_blasList;
	std::unique_ptr<FShaderBuffer> m_tlas;
	std::unique_ptr<FShaderBuffer> m_packedMaterials;
	std::vector<FMaterial> m_materialList;
	DirectX::BoundingBox m_sceneBounds; // world space
	size_t m_primitiveCount;
	// See: https://developer.nvidia.com/blog/introduction-turing-mesh-shaders/
	std::unique_ptr<FShaderBuffer> m_packedMeshletVertexIndexBuffer;
	std::unique_ptr<FShaderBuffer> m_packedMeshletPrimitiveIndexBuffer;
	std::unique_ptr<FShaderBuffer> m_packedMeshlets;

	// Lights
	std::vector<FLight> m_globalLightList;

	// Sky
	FLightProbe m_skylight;
	std::shared_ptr<FShaderSurface> m_dynamicSkySH = nullptr;
	std::shared_ptr<FShaderSurface> m_dynamicSkyEnvmap = nullptr;

	// Sun Dir
	Vector3 m_sunDir = { 1, 0.1, 1 };
	Matrix m_originalSunTransform;

	// Transform
	Matrix m_rootTransform;

	static inline float s_loadProgress = 0.f;

	// Note - these should add up to 1.0
	static inline float s_modelLoadTimeFrac = 0.2f;
	static inline float s_materialLoadTimeFrac = 0.3f;
	static inline float s_meshFixupTimeFrac = 0.3f;
	static inline float s_cacheHDRITimeFrac = 0.1f;
	static inline float s_meshBufferLoadTimeFrac = 0.03f;
	static inline float s_meshBufferViewsLoadTimeFrac = 0.01f;
	static inline float s_meshAccessorsLoadTimeFrac = 0.01f;
	static inline float s_lightsLoadTimeFrac = 0.01f;


private:
	void LoadLights(const tinygltf::Model& model);
	void CreateAccelerationStructures(const tinygltf::Model& model);
	void CreateGpuGeometryBuffers();
	void CreateGpuLightBuffers();
	void LoadMaterials(const tinygltf::Model& model);
	FMaterial LoadMaterial(const tinygltf::Model& model, const int materialIndex);
	int LoadTexture(const tinygltf::Image& image, const DXGI_FORMAT srcFormat = DXGI_FORMAT_UNKNOWN, const DXGI_FORMAT compressedFormat = DXGI_FORMAT_UNKNOWN);
	std::pair<int, int> PrefilterNormalRoughnessTextures(const tinygltf::Image& normalmap, const tinygltf::Image& metallicRoughnessmap);
	void ProcessReadbackTexture(FResourceReadbackContext* context, const std::string& filename, const int width, const int height, const size_t mipCount, const DXGI_FORMAT fmt, const int bpp);

private:
	std::vector<concurrency::task<void>> m_loadingJobs;
};