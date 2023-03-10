#pragma once

#include <SimpleMath.h>
#include <ppltasks.h>
#include <backend-d3d12.h>
#include <gpu-shared-types.h>
#include <spookyhash_api.h>
#include <common.h>
#include <scene.h>
using namespace DirectX::SimpleMath;

namespace tinygltf
{
	class Model;
	class Image;
	class Sampler;
}

struct FController;

//--------------------------------------------------------------------
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

//--------------------------------------------------------------------
struct FRenderState
{
	FConfig m_config;
	const FScene* m_scene;
	FView m_view;
	FView m_cullingView;
	uint32_t m_resX, m_resY;
	uint32_t m_mouseX, m_mouseY;
};

//--------------------------------------------------------------------
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

//--------------------------------------------------------------------
namespace Renderer
{
	// Note: Not all render passes need to be annotated. If a pass needs to be accessed from 
	// elsewhere for synchronization, add an annotation here and use the SyncObject to coordinate.
	enum AnnotatedPass
	{
		VisibilityPass,
		AnnotatedPassCount
	};

	struct Status
	{
		static void Initialize();
		static void Pause();
		static void Resume();
		static bool IsPaused();
		static inline winrt::com_ptr<D3DFence_t> m_fence;
		static inline uint64_t m_fenceVal = 0;
	};

	void Initialize(const uint32_t resX, const uint32_t resY);
	void Teardown();
	void Render(const FRenderState& renderState);

	void BlockUntilBeginPass(AnnotatedPass pass);
	void BlockUntilEndPass(AnnotatedPass pass);
	void SyncQueueToBeginPass(D3D12_COMMAND_LIST_TYPE queueType, AnnotatedPass pass);
	void SyncQueuetoEndPass(D3D12_COMMAND_LIST_TYPE queueType, AnnotatedPass pass);

	FRenderStats GetRenderStats();
	void ResetPathtraceAccumulation();
	std::unique_ptr<FTexture> GenerateEnvBrdfTexture(const uint32_t width, const uint32_t height);
	std::unique_ptr<FTexture> GenerateWhiteNoiseTextures(const uint32_t width, const uint32_t height, const uint32_t depth);

	// Generate a lat-long sky texutre (spherical projection) using Preetham sky model
	void GenerateDynamicSkyTexture(FCommandList* cmdList, const uint32_t outputUavIndex, const int resX, const int resY, Vector3 sunDir);

	// Convert a lat-long (spherical projection) texture into a cubemap
	void ConvertLatlong2Cubemap(FCommandList* cmdList, const uint32_t srcSrvIndex, const std::vector<uint32_t>& outputUavIndices, const int cubemapRes, const uint32_t numMips);

	// Prefilter a source cubemap using GGX importance sampling 
	void PrefilterCubemap(FCommandList* cmdList, const uint32_t srcCubemapSrvIndex, const std::vector<uint32_t>& outputUavIndices, const int cubemapRes, const uint32_t mipOffset, const uint32_t numMips);

	// Downsample an UAV to half resolution
	void DownsampleUav(FCommandList* cmdList, const int srvUavIndex, const int dstUavIndex, const int dstResX, const int dstResY);
}