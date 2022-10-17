#pragma once

#include <SimpleMath.h>
#include <ppltasks.h>
#include <gpu-shared-types.h>
#include <spookyhash_api.h>
#include <common.h>
using namespace DirectX::SimpleMath;

namespace tinygltf
{
	class Model;
	class Image;
	class Sampler;
}

struct FController;

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
	size_t GetCount() const { return m_entityList.size(); }
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

	// Scene files
	std::wstring m_modelFilename = {};
	std::wstring m_environmentFilename = {};
	std::string m_textureCachePath = {};
	std::string m_modelCachePath = {};

	// Scene entity lists
	using FSceneMeshEntities = TSceneEntities<FMesh>;
	using FSceneLightEntities = TSceneEntities<int>;
	FSceneMeshEntities m_sceneMeshes;
	FSceneMeshEntities m_sceneMeshDecals;
	FSceneLightEntities m_sceneLights;
	std::unique_ptr<FShaderBuffer> m_packedLightIndices; // Index into global light list
	std::unique_ptr<FShaderBuffer> m_packedLightTransforms;
	std::vector<FCamera> m_cameras;

	// Scene geo
	std::unique_ptr<FShaderBuffer> m_packedPrimitives;
	std::unique_ptr<FShaderBuffer> m_packedPrimitiveCounts;
	std::unordered_map<std::string, std::unique_ptr<FShaderBuffer>> m_blasList;
	std::unique_ptr<FShaderBuffer> m_tlas;
	std::unique_ptr<FShaderBuffer> m_packedMaterials;
	std::vector<FMaterial> m_materialList;
	DirectX::BoundingBox m_sceneBounds; // world space
	size_t m_primitiveCount;

	// Lights
	std::vector<FLight> m_globalLightList;
	std::unique_ptr<FShaderBuffer> m_packedGlobalLightProperties;
	FLightProbe m_environmentSky;

	// Transform
	Matrix m_rootTransform;

private:
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

struct FRenderState
{
	FConfig m_config;
	bool m_suspendRendering;
	FScene* m_scene;
	FView m_view;
	FView m_cullingView;
	uint32_t m_mouseX, m_mouseY;
};

struct FDebugDraw : public FModelLoader
{
	static const size_t MaxCommands = 256;

	struct PassDesc
	{
		FShaderSurface* colorTarget;
		FShaderSurface* depthTarget;
		uint32_t resX;
		uint32_t resY;
		const FScene* scene;
		const FView* view;
		FConfig renderConfig;
	};

	void Initialize();
	void DrawPrimitive(DebugShape::Type shapeType, Color color, Matrix transform, bool bPersistent = false);
	void Flush(const PassDesc& passDesc);

private:
	FMeshPrimitive m_shapePrimitives[DebugShape::Count];
	std::unique_ptr<FShaderBuffer> m_packedPrimitives;
	std::unique_ptr<FShaderBuffer> m_packedPrimitiveIndexCounts;

	// Maintain a list of debug draw commands on the CPU-side that are copied over to the GPU and sorted when Flush() is called.
	concurrency::concurrent_vector<FDebugDrawCmd> m_queuedCommands;
	std::unique_ptr<FShaderBuffer> m_queuedCommandsBuffer;
	std::unique_ptr<FShaderBuffer> m_indirectPrimitiveArgsBuffer;
	std::unique_ptr<FShaderBuffer> m_indirectPrimitiveCountsBuffer;
	std::unique_ptr<FShaderBuffer> m_indirectLineArgsBuffer;
	std::unique_ptr<FShaderBuffer> m_indirectLineCountsBuffer;
};

namespace Demo
{
	FRenderState GetRenderState();
	FRenderStatsBuffer GetRenderStats();
	FDebugDraw* GetDebugRenderer();
	void ResetPathtraceAccumulation();
	void InitializeRenderer(const uint32_t resX, const uint32_t resY);
	void LoadDebugModels();
	void TeardownRenderer();
}