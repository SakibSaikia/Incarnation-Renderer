#include <demo.h>
#include <backend-d3d12.h>
#include <profiling.h>
#include <renderer.h>
#include <ppltasks.h>
#include <sstream>
#include <imgui.h>
#include <dxcapi.h>
#include <random>
#include <algorithm>
#include <tiny_gltf.h>
#include "render-jobs/job-sync.h"

namespace Renderer
{
	std::unique_ptr<RenderJob::Sync> s_jobSync;
	std::unique_ptr<FTexture> s_envBRDF;
	std::unique_ptr<FShaderSurface> s_taaAccumulationBuffer;
	std::unique_ptr<FShaderSurface> s_pathtraceHistoryBuffer;
	std::unique_ptr<FShaderBuffer> s_renderStatsBuffer;
	std::vector<Vector2> s_pixelJitterValues;
	Matrix s_prevViewProjectionTransform;
	uint32_t s_pathtraceCurrentSampleIndex = 1;
	FRenderStats s_renderStats;
	FDebugDraw s_debugDrawing;
	FCommandList::Sync s_renderPassSync[AnnotatedPassCount];	
}

// Render Jobs
#include "render-jobs/environmentmap.inl"
#include "render-jobs/msaa-resolve.inl"
#include "render-jobs/taa-resolve.inl"
#include "render-jobs/ui-pass.inl"
#include "render-jobs/path-tracing.inl"
#include "render-jobs/tonemap.inl"
#include "render-jobs/update-tlas.inl"
#include "render-jobs/visibility-pass.inl"
#include "render-jobs/gbuffer-pass.inl"
#include "render-jobs/debug-visualization.inl"
#include "render-jobs/highlight-pass.inl"
#include "render-jobs/batch-culling.inl"
#include "render-jobs/light-culling.inl"
#include "render-jobs/sky-lighting.inl"
#include "render-jobs/direct-lighting.inl"
#include "render-jobs/clustered-lighting.inl"
#include "render-jobs/dynamic-sky.inl"

namespace
{
	float Halton(uint64_t sampleIndex, uint32_t base)
	{
		float result = 0.f;
		float f = 1.f;

		while (sampleIndex > 0)
		{
			f = f / base;
			result += f * (sampleIndex % base);
			sampleIndex = sampleIndex / base;
		}

		return result;
	}
}
void Renderer::Status::Initialize()
{
	AssertIfFailed(RenderBackend12::GetDevice()->CreateFence(m_fenceVal, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.put())));
}

void Renderer::Status::Pause()
{
	m_fence->Signal(++m_fenceVal);
	RenderBackend12::FlushGPU();
}

void Renderer::Status::Resume()
{
	RenderBackend12::FlushGPU();
	m_fence->Signal(++m_fenceVal);
}

bool Renderer::Status::IsPaused()
{
	// Odd fence values mean that renderer is paused, and even signifies on
	return m_fence && (m_fence->GetCompletedValue() & 0x1);
}


FRenderStats Renderer::GetRenderStats()
{
	return s_renderStats;
}

void Renderer::Initialize(const uint32_t resX, const uint32_t resY)
{
	Status::Initialize();
	s_jobSync = std::make_unique<RenderJob::Sync>();
	s_envBRDF = Renderer::GenerateEnvBrdfTexture(512, 512);

	s_pathtraceHistoryBuffer.reset(RenderBackend12::CreateNewShaderSurface(L"hdr_history_buffer_rt", FShaderSurface::Type::UAV, FResource::Allocation::Persistent(), DXGI_FORMAT_R11G11B10_FLOAT, resX, resY, 1, 1));
	s_taaAccumulationBuffer.reset(RenderBackend12::CreateNewShaderSurface(L"taa_accumulation_buffer_raster", FShaderSurface::Type::UAV, FResource::Allocation::Persistent(), DXGI_FORMAT_R11G11B10_FLOAT, resX, resY, 1, 1));
	s_renderStatsBuffer.reset(RenderBackend12::CreateNewShaderBuffer(L"render_stats_buffer", FShaderBuffer::Type::Raw, FResource::AccessMode::GpuReadWrite, FResource::Allocation::Persistent(), sizeof(FRenderStats), false, nullptr, nullptr, SpecialDescriptors::RenderStatsBufferUavIndex));

	// Generate Pixel Jitter Values
	for (int sampleIdx = 0; sampleIdx < 16; ++sampleIdx)
	{
		Vector2 pixelJitter = { Halton(sampleIdx, 2), Halton(sampleIdx, 3) };
		pixelJitter = 2.f * (pixelJitter - Vector2(0.5, 0.5)) / Vector2(resX, resY);
		s_pixelJitterValues.push_back(pixelJitter);
	}

	s_debugDrawing.Initialize();
}

void Renderer::BlockUntilBeginPass(AnnotatedPass pass)
{
	const FFenceMarker& gpuBeginMarker = s_renderPassSync[pass].m_fenceMarkers[(uint32_t)FCommandList::SyncPoint::GpuBegin];
	gpuBeginMarker.Wait();
}

void Renderer::BlockUntilEndPass(AnnotatedPass pass)
{
	const FFenceMarker& gpuFinishMarker = s_renderPassSync[pass].m_fenceMarkers[(uint32_t)FCommandList::SyncPoint::GpuFinish];
	gpuFinishMarker.Wait();
}

void Renderer::SyncQueueToBeginPass(D3D12_COMMAND_LIST_TYPE queueType, AnnotatedPass pass)
{
	using namespace Renderer;
	SCOPED_COMMAND_QUEUE_EVENT(queueType, "cross_queue_sync", 0);

	D3DCommandQueue_t* cmdQueue = RenderBackend12::GetCommandQueue(queueType);
	const FFenceMarker& gpuBeginMarker = s_renderPassSync[pass].m_fenceMarkers[(uint32_t)FCommandList::SyncPoint::GpuBegin];
	gpuBeginMarker.Wait(cmdQueue);
}

void Renderer::SyncQueuetoEndPass(D3D12_COMMAND_LIST_TYPE queueType, AnnotatedPass pass)
{
	using namespace Renderer;
	SCOPED_COMMAND_QUEUE_EVENT(queueType, "cross_queue_sync", 0);

	D3DCommandQueue_t* cmdQueue = RenderBackend12::GetCommandQueue(queueType);
	const FFenceMarker& gpuFinishMarker = s_renderPassSync[pass].m_fenceMarkers[(uint32_t)FCommandList::SyncPoint::GpuFinish];
	gpuFinishMarker.Wait(cmdQueue);
}

std::unique_ptr<FTexture> Renderer::GenerateEnvBrdfTexture(const uint32_t width, const uint32_t height)
{
	// Compute CL
	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"hdr_preprocess", D3D12_COMMAND_LIST_TYPE_DIRECT);
	D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

	std::unique_ptr<FShaderSurface> brdfUav{ RenderBackend12::CreateNewShaderSurface(L"env_brdf_uav", FShaderSurface::Type::UAV, FResource::Allocation::Transient(cmdList->GetFence(FCommandList::SyncPoint::GpuFinish)), DXGI_FORMAT_R16G16_FLOAT, width, height) };

	{
		SCOPED_COMMAND_LIST_EVENT(cmdList, "integrate_env_bdrf", 0);

		// Descriptor Heaps
		D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
		d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

		// Root Signature
		std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
			L"brdf_integration_rootsig",
			cmdList,
			FRootSignature::Desc{ L"image-based-lighting/split-sum-approx/brdf-integration.hlsl", L"rootsig", L"rootsig_1_1" });

		d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

		// PSO
		IDxcBlob* csBlob = RenderBackend12::CacheShader({
			L"image-based-lighting/split-sum-approx/brdf-integration.hlsl",
			L"cs_main",
			L"THREAD_GROUP_SIZE_X=16 THREAD_GROUP_SIZE_Y=16" ,
			L"cs_6_6" });

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = rootsig->m_rootsig;
		psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
		psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
		psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

		D3DPipelineState_t* pso = RenderBackend12::FetchComputePipelineState(psoDesc);
		d3dCmdList->SetPipelineState(pso);

		struct
		{
			uint32_t uavWidth;
			uint32_t uavHeight;
			uint32_t uavIndex;
			uint32_t numSamples;
		} rootConstants = { width, height, brdfUav->m_descriptorIndices.UAVs[0], 1024 };

		d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);

		// Dispatch
		size_t threadGroupCount = std::max<size_t>(std::ceil(width / 16), 1);
		d3dCmdList->Dispatch(threadGroupCount, threadGroupCount, 1);
	}

	// Copy from UAV to destination texture
	brdfUav->m_resource->Transition(cmdList, brdfUav->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE);
	std::unique_ptr<FTexture> brdfTex{ RenderBackend12::CreateNewTexture(L"env_brdf_tex", FTexture::Type::Tex2D, FResource::Allocation::Persistent(), DXGI_FORMAT_R16G16_FLOAT, width, height, 1, 1, D3D12_RESOURCE_STATE_COPY_DEST) };
	d3dCmdList->CopyResource(brdfTex->m_resource->m_d3dResource, brdfUav->m_resource->m_d3dResource);
	brdfTex->m_resource->Transition(cmdList, brdfTex->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

	return std::move(brdfTex);
}

std::unique_ptr<FTexture> Renderer::GenerateWhiteNoiseTextures(const uint32_t width, const uint32_t height, const uint32_t depth)
{
	const uint32_t numSamples = width * height * depth;
	std::vector<uint8_t> noiseSamples(numSamples);
	std::default_random_engine generator;
	std::uniform_int_distribution<uint32_t> distribution(0, 255);
	for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
	{
		noiseSamples[sampleIndex] = (uint8_t)distribution(generator);
	}

	std::vector<DirectX::Image> noiseImages(depth);
	for (int arrayIndex = 0, sampleOffset = 0; arrayIndex < depth; ++arrayIndex)
	{
		DirectX::Image& img = noiseImages[arrayIndex];
		img.width = width;
		img.height = height;
		img.format = DXGI_FORMAT_R8_UNORM;
		img.rowPitch = 1 * img.width;
		img.slicePitch = img.rowPitch * img.height;
		img.pixels = (uint8_t*)noiseSamples.data() + sampleOffset;
		sampleOffset += img.slicePitch;
	}

	FResourceUploadContext uploader{ numSamples };
	std::unique_ptr<FTexture> noiseTexArray{ RenderBackend12::CreateNewTexture(L"white_noise_array", FTexture::Type::Tex2DArray, FResource::Allocation::Persistent(), DXGI_FORMAT_R8_UNORM, width, height, 1, depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, noiseImages.data(), &uploader) };
	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_noise_texture", D3D12_COMMAND_LIST_TYPE_DIRECT);
	uploader.SubmitUploads(cmdList);
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

	return std::move(noiseTexArray);
}

// Convert a lat-long (spherical projection) texture into a cubemap
void Renderer::ConvertLatlong2Cubemap(FCommandList* cmdList, const uint32_t srcSrvIndex, const std::vector<uint32_t>& outputUavIndices, const int cubemapRes, const uint32_t numMips)
{
	SCOPED_COMMAND_LIST_EVENT(cmdList, "cubemap_gen", 0);
	D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

	// Descriptor Heaps
	D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
	d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

	// Root Signature
	std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
		L"cubemapgen_rootsig",
		cmdList,
		FRootSignature::Desc{ L"content-pipeline/cubemapgen.hlsl", L"rootsig", L"rootsig_1_1" });

	d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

	// PSO
	IDxcBlob* csBlob = RenderBackend12::CacheShader({
		L"content-pipeline/cubemapgen.hlsl",
		L"cs_main",
		L"THREAD_GROUP_SIZE_X=16 THREAD_GROUP_SIZE_Y=16",
		L"cs_6_6" });

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = rootsig->m_rootsig;
	psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
	psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
	psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

	D3DPipelineState_t* pso = RenderBackend12::FetchComputePipelineState(psoDesc);
	d3dCmdList->SetPipelineState(pso);

	// Convert from sperical map to cube map
	for (uint32_t mipIndex = 0; mipIndex < numMips; ++mipIndex)
	{
		uint32_t mipSize = cubemapRes >> mipIndex;

		struct
		{
			uint32_t mipIndex;
			uint32_t hdrTextureIndex;
			uint32_t cubemapUavIndex;
			uint32_t mipSize;
		} rootConstants = { mipIndex, srcSrvIndex, outputUavIndices[mipIndex], (uint32_t)mipSize };

		d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);

		// Dispatch
		size_t threadGroupCount = std::max<size_t>(std::ceil(mipSize / 16), 1);
		d3dCmdList->Dispatch(threadGroupCount, threadGroupCount, 1);
	}
}

// Prefilter a source cubemap using GGX importance sampling 
void Renderer::PrefilterCubemap(FCommandList* cmdList, const uint32_t srcCubemapSrvIndex, const std::vector<uint32_t>& outputUavIndices, const int cubemapRes, const uint32_t mipOffset, const uint32_t numMips)
{
	SCOPED_COMMAND_LIST_EVENT(cmdList, "prefilter_envmap", 0);
	D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

	// Descriptor Heaps
	D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
	d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

	// Root Signature
	std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
		L"split_sum_rootsig",
		cmdList,
		FRootSignature::Desc{ L"image-based-lighting/split-sum-approx/prefilter.hlsl", L"rootsig", L"rootsig_1_1" });

	d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

	// PSO
	IDxcBlob* csBlob = RenderBackend12::CacheShader({
		L"image-based-lighting/split-sum-approx/prefilter.hlsl",
		L"cs_main",
		L"THREAD_GROUP_SIZE_X=16 THREAD_GROUP_SIZE_Y=16" ,
		L"cs_6_6" });

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = rootsig->m_rootsig;
	psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
	psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
	psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

	D3DPipelineState_t* pso = RenderBackend12::FetchComputePipelineState(psoDesc);
	d3dCmdList->SetPipelineState(pso);

	for (uint32_t mipIndex = mipOffset; mipIndex < numMips; ++mipIndex)
	{
		uint32_t mipSize = cubemapRes >> mipIndex;

		for (uint32_t faceIndex = 0; faceIndex < 6; ++faceIndex)
		{

			struct
			{
				uint32_t mipSize;
				uint32_t cubemapSize;
				uint32_t faceIndex;
				uint32_t envmapSrvIndex;
				uint32_t uavIndex;
				uint32_t sampleCount;
				float roughness;
			} rootConstants = { mipSize, (uint32_t)cubemapRes, faceIndex, srcCubemapSrvIndex, outputUavIndices[mipIndex], 1024, mipIndex / (float)numMips };

			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);

			// Dispatch
			size_t threadGroupCount = std::max<size_t>(std::ceil(mipSize / 16), 1);
			d3dCmdList->Dispatch(threadGroupCount, threadGroupCount, 1);
		}
	}
}

void FDebugDraw::Initialize()
{
	std::unordered_map<std::string, DebugShape::Type> primitiveNameMapping =
	{	
		{"Cube", DebugShape::Cube},
		{"Icosphere", DebugShape::Icosphere},
		{"Sphere", DebugShape::Sphere},
		{"Cylinder", DebugShape::Cylinder},
		{"Cone", DebugShape::Cone},
		{"Plane", DebugShape::Plane}
	};

	// Debug models
	std::string modelFilepath = GetFilepathA("debug-primitives.gltf");
	tinygltf::TinyGLTF loader;

	std::string errors, warnings;
	tinygltf::Model model;
	bool ok = loader.LoadASCIIFromFile(&model, &errors, &warnings, modelFilepath);
	if (!ok)
	{
		if (!warnings.empty())
		{
			printf("Warn: %s\n", warnings.c_str());
		}

		if (!errors.empty())
		{
			printf("Error: %s\n", errors.c_str());
			DebugAssert(ok, "Failed to parse glTF");
		}
	}

	LoadMeshBuffers(model);
	LoadMeshBufferViews(model);
	LoadMeshAccessors(model);

	for (tinygltf::Scene& scene : model.scenes)
	{
		for (const int nodeIndex : scene.nodes)
		{
			const tinygltf::Node& node = model.nodes[nodeIndex];

			if (node.mesh != -1)
			{
				const tinygltf::Primitive& primitive = model.meshes[node.mesh].primitives[0];
				FMeshPrimitive& newPrimitive = m_shapePrimitives[primitiveNameMapping[node.name]];

				// Index data
				const tinygltf::Accessor& indexAccessor = model.accessors[primitive.indices];
				newPrimitive.m_indexCount = indexAccessor.count;
				newPrimitive.m_indexAccessor = primitive.indices;

				// Position data
				auto posIt = primitive.attributes.find("POSITION");
				DebugAssert(posIt != primitive.attributes.cend());
				newPrimitive.m_positionAccessor = posIt->second;

				// UV data
				auto uvIt = primitive.attributes.find("TEXCOORD_0");
				newPrimitive.m_uvAccessor = (uvIt != primitive.attributes.cend() ? uvIt->second : -1);

				// Normal data
				auto normalIt = primitive.attributes.find("NORMAL");
				newPrimitive.m_normalAccessor = (normalIt != primitive.attributes.cend() ? normalIt->second : -1);

				// Tangent data
				auto tangentIt = primitive.attributes.find("TANGENT");
				newPrimitive.m_tangentAccessor = (tangentIt != primitive.attributes.cend() ? tangentIt->second : -1);

				// Topology
				switch (primitive.mode)
				{
				case TINYGLTF_MODE_POINTS:
					newPrimitive.m_topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
					break;
				case TINYGLTF_MODE_LINE:
					newPrimitive.m_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
					break;
				case TINYGLTF_MODE_LINE_STRIP:
					newPrimitive.m_topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
					break;
				case TINYGLTF_MODE_TRIANGLES:
					newPrimitive.m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
					break;
				case TINYGLTF_MODE_TRIANGLE_STRIP:
					newPrimitive.m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
					break;
				default:
					DebugAssert(false);
				}
			}
		}
	}

	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_debug_primitives", D3D12_COMMAND_LIST_TYPE_DIRECT);

	// Packed buffer that contains an array of debug primitives
	{
		std::vector<FGpuPrimitive> primitives;
		std::vector<uint32_t> primitiveIndexCounts;
		for (int primitiveIndex = 0; primitiveIndex < DebugShape::Count; ++primitiveIndex)
		{
			const FMeshPrimitive& primitive = m_shapePrimitives[primitiveIndex];
			FGpuPrimitive newPrimitive = {};
			newPrimitive.m_localToWorld = Matrix::Identity;
			newPrimitive.m_boundingSphere = Vector4(0.f, 0.f, 0.f, 1.f);
			newPrimitive.m_indexAccessor = primitive.m_indexAccessor;
			newPrimitive.m_positionAccessor = primitive.m_positionAccessor;
			newPrimitive.m_uvAccessor = primitive.m_uvAccessor;
			newPrimitive.m_normalAccessor = primitive.m_normalAccessor;
			newPrimitive.m_tangentAccessor = primitive.m_tangentAccessor;
			newPrimitive.m_materialIndex = primitive.m_materialIndex;
			newPrimitive.m_indexCount = primitive.m_indexCount;
			newPrimitive.m_indicesPerTriangle = 3;
			primitives.push_back(newPrimitive);

			primitiveIndexCounts.push_back(primitive.m_indexCount);
		}

		const size_t uploadBufferSize = primitives.size() * sizeof(FGpuPrimitive) + primitiveIndexCounts.size() * sizeof(uint32_t);
		FResourceUploadContext uploader{ uploadBufferSize };

		m_packedPrimitives.reset(RenderBackend12::CreateNewShaderBuffer(
			L"debug_primitives",
			FShaderBuffer::Type::Raw,
			FResource::AccessMode::GpuReadOnly,
			FResource::Allocation::Persistent(),
			primitives.size() * sizeof(FGpuPrimitive),
			false,
			(const uint8_t*)primitives.data(),
			&uploader));

		m_packedPrimitiveIndexCounts.reset(RenderBackend12::CreateNewShaderBuffer(
			L"debug_primitive_index_counts",
			FShaderBuffer::Type::Raw,
			FResource::AccessMode::GpuReadOnly,
			FResource::Allocation::Persistent(),
			primitiveIndexCounts.size() * sizeof(uint32_t),
			false,
			(const uint8_t*)primitiveIndexCounts.data(),
			&uploader,
			-1,
			SpecialDescriptors::DebugPrimitiveIndexCountSrvIndex));

		uploader.SubmitUploads(cmdList);

		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
	}

	// Buffer that contains queued debug draw commands
	m_queuedCommandsBuffer.reset(RenderBackend12::CreateNewShaderBuffer(L"debug_draw_commands", FShaderBuffer::Type::Raw, FResource::AccessMode::GpuReadWrite, FResource::Allocation::Persistent(), MaxCommands * sizeof(FDebugDrawCmd)));

	// Indirect draw buffers
	m_indirectPrimitiveArgsBuffer.reset(RenderBackend12::CreateNewShaderBuffer(L"debug_draw_prim_args_buffer", FShaderBuffer::Type::Raw, FResource::AccessMode::GpuReadWrite, FResource::Allocation::Persistent(), MaxCommands * sizeof(FIndirectDrawWithRootConstants), false, nullptr, nullptr, SpecialDescriptors::DebugDrawIndirectPrimitiveArgsUavIndex));
	m_indirectPrimitiveCountsBuffer.reset(RenderBackend12::CreateNewShaderBuffer(L"debug_draw_prim_counts_buffer", FShaderBuffer::Type::Raw, FResource::AccessMode::GpuReadWrite, FResource::Allocation::Persistent(), sizeof(uint32_t), true, nullptr, nullptr, SpecialDescriptors::DebugDrawIndirectPrimitiveCountUavIndex));
	m_indirectLineArgsBuffer.reset(RenderBackend12::CreateNewShaderBuffer(L"debug_draw_line_args_buffer", FShaderBuffer::Type::Raw, FResource::AccessMode::GpuReadWrite, FResource::Allocation::Persistent(), MaxCommands * sizeof(FIndirectDrawWithRootConstants), false, nullptr, nullptr, SpecialDescriptors::DebugDrawIndirectLineArgsUavIndex));
	m_indirectLineCountsBuffer.reset(RenderBackend12::CreateNewShaderBuffer(L"debug_draw_line_counts_buffer", FShaderBuffer::Type::Raw, FResource::AccessMode::GpuReadWrite, FResource::Allocation::Persistent(), sizeof(uint32_t), true, nullptr, nullptr, SpecialDescriptors::DebugDrawIndirectLineCountUavIndex));
}

void FDebugDraw::DrawPrimitive(DebugShape::Type shapeType, Color color, Matrix transform, bool bPersistent)
{
	m_queuedCommands.push_back({ color, transform, (uint32_t)shapeType, bPersistent });
}

void FDebugDraw::Flush(const PassDesc& passDesc)
{
	static FFenceMarker flushCompleteFence;
	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_debug_draw_cmds", D3D12_COMMAND_LIST_TYPE_DIRECT);

	{
		SCOPED_COMMAND_LIST_EVENT(cmdList, "debug_draw", 0);

		if (!m_queuedCommands.empty())
		{
			const size_t numCommands = m_queuedCommands.size();
			const size_t bufferSize = numCommands * sizeof(FDebugDrawCmd);
			FResourceUploadContext uploader{ bufferSize };

			// Upload the CPU debug draw commands buffer data
			FResource* destResource = m_queuedCommandsBuffer->m_resource;
			std::vector<D3D12_SUBRESOURCE_DATA> srcData(1);
			srcData[0].pData = &m_queuedCommands[0];
			srcData[0].RowPitch = bufferSize;
			srcData[0].SlicePitch = bufferSize;
			uploader.UpdateSubresources(
				destResource,
				srcData,
				[destResource, transitionToken = destResource->GetTransitionToken()](FCommandList* cmdList)
				{
					destResource->Transition(cmdList, transitionToken, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				});

			uploader.SubmitUploads(cmdList, &flushCompleteFence);

			// Now that the CPU data is uploaded, clear the entries in m_queuedCommands
			m_queuedCommands.clear();

			// Run a compute shader to generate the indirect draw args

			SCOPED_CPU_EVENT("primitive_draw_gen", PIX_COLOR_DEFAULT);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "debug_primitive_gen", 0);

			// Transitions
			m_indirectPrimitiveArgsBuffer->m_resource->Transition(cmdList, m_indirectPrimitiveArgsBuffer->m_resource->GetTransitionToken(), 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			m_indirectPrimitiveCountsBuffer->m_resource->Transition(cmdList, m_indirectPrimitiveCountsBuffer->m_resource->GetTransitionToken(), 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			cmdList->m_d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"debug_draw_gen_rootsig",
				cmdList,
				FRootSignature::Desc{ L"debug-drawing/primitive-generation.hlsl", L"rootsig", L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({
				L"debug-drawing/primitive-generation.hlsl",
				L"cs_main",
				L"THREAD_GROUP_SIZE_X=32",
				L"cs_6_6" });

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = rootsig->m_rootsig;
			psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
			psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			D3DPipelineState_t* pso = RenderBackend12::FetchComputePipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			// Root Constants
			struct Constants
			{
				uint32_t queuedCommandsBufferIndex;
				uint32_t debugDrawCount;
			};

			std::unique_ptr<FSystemBuffer> cbuf{ RenderBackend12::CreateNewSystemBuffer(
				L"debug_drawcall_gen_cb",
				FResource::AccessMode::CpuWriteOnly,
				sizeof(Constants),
				cmdList->GetFence(FCommandList::SyncPoint::GpuFinish),
				[&](uint8_t* pDest)
				{
					auto cb = reinterpret_cast<Constants*>(pDest);
					cb->queuedCommandsBufferIndex = m_queuedCommandsBuffer->m_descriptorIndices.SRV;
					cb->debugDrawCount = (uint32_t)numCommands;
				}) };

			d3dCmdList->SetComputeRootConstantBufferView(0, cbuf->m_resource->m_d3dResource->GetGPUVirtualAddress());

			// Dispatch
			size_t threadGroupCountX = std::max<size_t>(std::ceil(MaxCommands / 32), 1);
			d3dCmdList->Dispatch(threadGroupCountX, 1, 1);

			// Transition back to expected state to avoid having to transition on the copy queue later
			m_queuedCommandsBuffer->m_resource->Transition(cmdList, m_queuedCommandsBuffer->m_resource->GetTransitionToken(), 0, D3D12_RESOURCE_STATE_COPY_DEST);
		}


		// Finally, dispatch the indirect draw commands for the debug primitives
		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "primitive_render", 0);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

			passDesc.colorTarget->m_resource->Transition(cmdList, passDesc.colorTarget->m_resource->GetTransitionToken(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
			passDesc.depthTarget->m_resource->Transition(cmdList, passDesc.depthTarget->m_resource->GetTransitionToken(), 0, D3D12_RESOURCE_STATE_DEPTH_READ);
			m_indirectPrimitiveArgsBuffer->m_resource->Transition(cmdList, m_indirectPrimitiveArgsBuffer->m_resource->GetTransitionToken(), 0, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
			m_indirectPrimitiveCountsBuffer->m_resource->Transition(cmdList, m_indirectPrimitiveCountsBuffer->m_resource->GetTransitionToken(), 0, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

			// Descriptor heaps need to be set before setting the root signature when using HLSL Dynamic Resources
			// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(L"debug_draw_rootsig", cmdList, FRootSignature::Desc{ L"debug-drawing/primitive-submission.hlsl", L"rootsig", L"rootsig_1_1" });
			d3dCmdList->SetGraphicsRootSignature(rootsig->m_rootsig);

			// Frame constant buffer
			struct FrameCbLayout
			{
				Matrix sceneRotation;
				int debugMeshAccessorsIndex;
				int debugMeshBufferViewsIndex;
				int debugPrimitivesIndex;
			};

			std::unique_ptr<FSystemBuffer> frameCb{ RenderBackend12::CreateNewSystemBuffer(
				L"frame_cb",
				FResource::AccessMode::CpuWriteOnly,
				sizeof(FrameCbLayout),
				cmdList->GetFence(FCommandList::SyncPoint::GpuFinish),
				[this, passDesc](uint8_t* pDest)
				{
					auto cbDest = reinterpret_cast<FrameCbLayout*>(pDest);
					cbDest->sceneRotation = passDesc.scene->m_rootTransform;
					cbDest->debugMeshAccessorsIndex = m_packedMeshAccessors->m_descriptorIndices.SRV;
					cbDest->debugMeshBufferViewsIndex = m_packedMeshBufferViews->m_descriptorIndices.SRV;
					cbDest->debugPrimitivesIndex = m_packedPrimitives->m_descriptorIndices.SRV;
				}) };

			d3dCmdList->SetGraphicsRootConstantBufferView(2, frameCb->m_resource->m_d3dResource->GetGPUVirtualAddress());

			// View constant buffer
			struct ViewCbLayout
			{
				Matrix viewProjTransform;
			};

			std::unique_ptr<FSystemBuffer> viewCb{ RenderBackend12::CreateNewSystemBuffer(
				L"view_cb",
				FResource::AccessMode::CpuWriteOnly,
				sizeof(ViewCbLayout),
				cmdList->GetFence(FCommandList::SyncPoint::GpuFinish),
				[passDesc](uint8_t* pDest)
				{
					auto cbDest = reinterpret_cast<ViewCbLayout*>(pDest);
					cbDest->viewProjTransform = passDesc.view->m_viewTransform * passDesc.view->m_projectionTransform;
				}) };

			d3dCmdList->SetGraphicsRootConstantBufferView(1, viewCb->m_resource->m_d3dResource->GetGPUVirtualAddress());

			D3D12_VIEWPORT viewport{ 0.f, 0.f, (float)passDesc.resX, (float)passDesc.resY, 0.f, 1.f };
			D3D12_RECT screenRect{ 0, 0, (LONG)passDesc.resX, (LONG)passDesc.resY };
			d3dCmdList->RSSetViewports(1, &viewport);
			d3dCmdList->RSSetScissorRects(1, &screenRect);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passDesc.colorTarget->m_descriptorIndices.RTVorDSVs[0]) };
			D3D12_CPU_DESCRIPTOR_HANDLE dsv = RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, passDesc.depthTarget->m_descriptorIndices.RTVorDSVs[0]);
			d3dCmdList->OMSetRenderTargets(1, rtvs, FALSE, &dsv);

			// Issue scene draws
			d3dCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			// PSO
			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.NodeMask = 1;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.pRootSignature = rootsig->m_rootsig;
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = passDesc.renderConfig.BackBufferFormat;
			psoDesc.SampleDesc.Count = 1;
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			// PSO - Shaders
			{
				D3D12_SHADER_BYTECODE& vs = psoDesc.VS;
				D3D12_SHADER_BYTECODE& ps = psoDesc.PS;

				IDxcBlob* vsBlob = RenderBackend12::CacheShader({ L"debug-drawing/primitive-submission.hlsl", L"vs_main", L"" , L"vs_6_6" });
				IDxcBlob* psBlob = RenderBackend12::CacheShader({ L"debug-drawing/primitive-submission.hlsl", L"ps_main", L"" , L"ps_6_6" });

				vs.pShaderBytecode = vsBlob->GetBufferPointer();
				vs.BytecodeLength = vsBlob->GetBufferSize();
				ps.pShaderBytecode = psBlob->GetBufferPointer();
				ps.BytecodeLength = psBlob->GetBufferSize();
			}

			// PSO - Rasterizer State
			{
				D3D12_RASTERIZER_DESC& desc = psoDesc.RasterizerState;
				desc.FillMode = D3D12_FILL_MODE_WIREFRAME;
				desc.CullMode = D3D12_CULL_MODE_NONE;
				desc.FrontCounterClockwise = TRUE;
				desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
				desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
				desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
				desc.DepthClipEnable = TRUE;
				desc.MultisampleEnable = FALSE;
				desc.AntialiasedLineEnable = TRUE;
				desc.ForcedSampleCount = 0;
				desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
			}

			// PSO - Blend State
			{
				D3D12_BLEND_DESC& desc = psoDesc.BlendState;
				desc.AlphaToCoverageEnable = FALSE;
				desc.IndependentBlendEnable = FALSE;
				desc.RenderTarget[0].BlendEnable = FALSE;
				desc.RenderTarget[0].LogicOpEnable = FALSE;
				desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			}

			// PSO - Depth Stencil State
			{
				D3D12_DEPTH_STENCIL_DESC& desc = psoDesc.DepthStencilState;
				desc.DepthEnable = TRUE;
				desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
				desc.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
				desc.StencilEnable = FALSE;
			}

			D3DPipelineState_t* pso = RenderBackend12::FetchGraphicsPipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			// Command signature
			D3DCommandSignature_t* commandSignature = FIndirectDrawWithRootConstants::GetCommandSignature(rootsig->m_rootsig);

			d3dCmdList->ExecuteIndirect(
				commandSignature,
				MaxCommands,
				m_indirectPrimitiveArgsBuffer->m_resource->m_d3dResource,
				0,
				m_indirectPrimitiveCountsBuffer->m_resource->m_d3dResource,
				0);
		}

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "line_render", 0);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

			passDesc.colorTarget->m_resource->Transition(cmdList, passDesc.colorTarget->m_resource->GetTransitionToken(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
			passDesc.depthTarget->m_resource->Transition(cmdList, passDesc.depthTarget->m_resource->GetTransitionToken(), 0, D3D12_RESOURCE_STATE_DEPTH_READ);
			m_indirectLineArgsBuffer->m_resource->Transition(cmdList, m_indirectLineArgsBuffer->m_resource->GetTransitionToken(), 0, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
			m_indirectLineCountsBuffer->m_resource->Transition(cmdList, m_indirectLineCountsBuffer->m_resource->GetTransitionToken(), 0, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

			// Descriptor heaps need to be set before setting the root signature when using HLSL Dynamic Resources
			// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(L"debug_draw_rootsig", cmdList, FRootSignature::Desc{ L"debug-drawing/line-submission.hlsl", L"rootsig", L"rootsig_1_1" });
			d3dCmdList->SetGraphicsRootSignature(rootsig->m_rootsig);

			// Frame constant buffer
			struct FrameCbLayout
			{
				Matrix sceneRotation;
			};

			std::unique_ptr<FSystemBuffer> frameCb{ RenderBackend12::CreateNewSystemBuffer(
				L"frame_cb",
				FResource::AccessMode::CpuWriteOnly,
				sizeof(FrameCbLayout),
				cmdList->GetFence(FCommandList::SyncPoint::GpuFinish),
				[this, passDesc](uint8_t* pDest)
				{
					auto cbDest = reinterpret_cast<FrameCbLayout*>(pDest);
					cbDest->sceneRotation = passDesc.scene->m_rootTransform;
				}) };

			d3dCmdList->SetGraphicsRootConstantBufferView(2, frameCb->m_resource->m_d3dResource->GetGPUVirtualAddress());

			// View constant buffer
			struct ViewCbLayout
			{
				Matrix viewProjTransform;
			};

			std::unique_ptr<FSystemBuffer> viewCb{ RenderBackend12::CreateNewSystemBuffer(
				L"view_cb",
				FResource::AccessMode::CpuWriteOnly,
				sizeof(ViewCbLayout),
				cmdList->GetFence(FCommandList::SyncPoint::GpuFinish),
				[passDesc](uint8_t* pDest)
				{
					auto cbDest = reinterpret_cast<ViewCbLayout*>(pDest);
					cbDest->viewProjTransform = passDesc.view->m_viewTransform * passDesc.view->m_projectionTransform;
				}) };

			d3dCmdList->SetGraphicsRootConstantBufferView(1, viewCb->m_resource->m_d3dResource->GetGPUVirtualAddress());

			D3D12_VIEWPORT viewport{ 0.f, 0.f, (float)passDesc.resX, (float)passDesc.resY, 0.f, 1.f };
			D3D12_RECT screenRect{ 0, 0, (LONG)passDesc.resX, (LONG)passDesc.resY };
			d3dCmdList->RSSetViewports(1, &viewport);
			d3dCmdList->RSSetScissorRects(1, &screenRect);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passDesc.colorTarget->m_descriptorIndices.RTVorDSVs[0]) };
			D3D12_CPU_DESCRIPTOR_HANDLE dsv = RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, passDesc.depthTarget->m_descriptorIndices.RTVorDSVs[0]);
			d3dCmdList->OMSetRenderTargets(1, rtvs, FALSE, &dsv);

			// Issue scene draws
			d3dCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);

			// PSO
			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.NodeMask = 1;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
			psoDesc.pRootSignature = rootsig->m_rootsig;
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = passDesc.renderConfig.BackBufferFormat;
			psoDesc.SampleDesc.Count = 1;
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			// PSO - Shaders
			{
				D3D12_SHADER_BYTECODE& vs = psoDesc.VS;
				D3D12_SHADER_BYTECODE& ps = psoDesc.PS;

				IDxcBlob* vsBlob = RenderBackend12::CacheShader({ L"debug-drawing/line-submission.hlsl", L"vs_main", L"" , L"vs_6_6" });
				IDxcBlob* psBlob = RenderBackend12::CacheShader({ L"debug-drawing/line-submission.hlsl", L"ps_main", L"" , L"ps_6_6" });

				vs.pShaderBytecode = vsBlob->GetBufferPointer();
				vs.BytecodeLength = vsBlob->GetBufferSize();
				ps.pShaderBytecode = psBlob->GetBufferPointer();
				ps.BytecodeLength = psBlob->GetBufferSize();
			}

			// PSO - Rasterizer State
			{
				D3D12_RASTERIZER_DESC& desc = psoDesc.RasterizerState;
				desc.FillMode = D3D12_FILL_MODE_WIREFRAME;
				desc.CullMode = D3D12_CULL_MODE_NONE;
				desc.FrontCounterClockwise = TRUE;
				desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
				desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
				desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
				desc.DepthClipEnable = TRUE;
				desc.MultisampleEnable = FALSE;
				desc.AntialiasedLineEnable = TRUE;
				desc.ForcedSampleCount = 0;
				desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
			}

			// PSO - Blend State
			{
				D3D12_BLEND_DESC& desc = psoDesc.BlendState;
				desc.AlphaToCoverageEnable = FALSE;
				desc.IndependentBlendEnable = FALSE;
				desc.RenderTarget[0].BlendEnable = FALSE;
				desc.RenderTarget[0].LogicOpEnable = FALSE;
				desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			}

			// PSO - Depth Stencil State
			{
				D3D12_DEPTH_STENCIL_DESC& desc = psoDesc.DepthStencilState;
				desc.DepthEnable = TRUE;
				desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
				desc.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
				desc.StencilEnable = FALSE;
			}

			D3DPipelineState_t* pso = RenderBackend12::FetchGraphicsPipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			// Command signature
			D3DCommandSignature_t* commandSignature = FIndirectDrawWithRootConstants::GetCommandSignature(rootsig->m_rootsig);

			d3dCmdList->ExecuteIndirect(
				commandSignature,
				MaxCommands,
				m_indirectLineArgsBuffer->m_resource->m_d3dResource,
				0,
				m_indirectLineCountsBuffer->m_resource->m_d3dResource,
				0);
		}

		// Clear for next frame
		const uint32_t clearValue[] = { 0, 0, 0, 0 };

		m_indirectPrimitiveCountsBuffer->m_resource->Transition(cmdList, m_indirectPrimitiveCountsBuffer->m_resource->GetTransitionToken(), 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmdList->m_d3dCmdList->ClearUnorderedAccessViewUint(
			RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_indirectPrimitiveCountsBuffer->m_descriptorIndices.UAV),
			RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_indirectPrimitiveCountsBuffer->m_descriptorIndices.NonShaderVisibleUAV, false),
			m_indirectPrimitiveCountsBuffer->m_resource->m_d3dResource,
			clearValue, 0, nullptr);

		m_indirectLineCountsBuffer->m_resource->Transition(cmdList, m_indirectLineCountsBuffer->m_resource->GetTransitionToken(), 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmdList->m_d3dCmdList->ClearUnorderedAccessViewUint(
			RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_indirectLineCountsBuffer->m_descriptorIndices.UAV),
			RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_indirectLineCountsBuffer->m_descriptorIndices.NonShaderVisibleUAV, false),
			m_indirectLineCountsBuffer->m_resource->m_d3dResource,
			clearValue, 0, nullptr);
	}

	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
	flushCompleteFence = cmdList->GetFence(FCommandList::SyncPoint::GpuFinish);
}

void Renderer::Teardown()
{
	s_envBRDF.reset(nullptr);
	s_pathtraceHistoryBuffer.reset(nullptr);
	s_taaAccumulationBuffer.reset(nullptr);
	s_renderStatsBuffer.reset(nullptr);
	s_debugDrawing.~FDebugDraw();
	s_jobSync.reset(nullptr);
}

void Renderer::Render(const FRenderState& renderState)
{
	const uint32_t resX = renderState.m_resX;
	const uint32_t resY = renderState.m_resY;
	const FConfig& config = renderState.m_config;
	const FConfig& c = config;

	RenderBackend12::WaitForSwapChain();

	SCOPED_CPU_EVENT("render", PIX_COLOR_DEFAULT);

	std::vector<concurrency::task<void>> sceneRenderJobs;

	static uint64_t frameIndex = 0;
	SCOPED_COMMAND_QUEUE_EVENT(D3D12_COMMAND_LIST_TYPE_DIRECT, PrintString("frame_%d", frameIndex).c_str(), 0);

	size_t totalPrimitives = 0;
	for (const auto& mesh : renderState.m_scene->m_sceneMeshes.m_entityList)
	{
		totalPrimitives += mesh.m_primitives.size();
	}

	// These resources need to be kept alive until all the render jobs have finished and joined
	FFenceMarker gpuFinishFence = RenderBackend12::GetCurrentFrameFence();
	const DXGI_FORMAT hdrFormat = DXGI_FORMAT_R11G11B10_FLOAT;
	const DXGI_FORMAT visBufferFormat = DXGI_FORMAT_R32_UINT;
	std::unique_ptr<FShaderSurface> hdrRasterSceneColor{ RenderBackend12::CreateNewShaderSurface(L"hdr_scene_color_raster", FShaderSurface::Type::RenderTarget | FShaderSurface::Type::UAV, FResource::Allocation::Transient(gpuFinishFence), hdrFormat, resX, resY, 1, 1, 1, 1, true, true) };
	std::unique_ptr<FShaderSurface> depthBuffer{ RenderBackend12::CreateNewShaderSurface(L"depth_buffer_raster", FShaderSurface::Type::DepthStencil, FResource::Allocation::Transient(gpuFinishFence), DXGI_FORMAT_D32_FLOAT_S8X24_UINT, resX, resY) };
	std::unique_ptr<FShaderSurface> hdrRaytraceSceneColor{ RenderBackend12::CreateNewShaderSurface(L"hdr_scene_color_rt", FShaderSurface::Type::UAV, FResource::Allocation::Transient(gpuFinishFence), DXGI_FORMAT_R16G16B16A16_FLOAT, resX, resY, 1, 1, 1, 1, true, true) };
	std::unique_ptr<FShaderSurface> visBuffer{ RenderBackend12::CreateNewShaderSurface(L"vis_buffer_raster", FShaderSurface::Type::RenderTarget, FResource::Allocation::Transient(gpuFinishFence), visBufferFormat, resX, resY) };
	std::unique_ptr<FShaderSurface> gbuffer_basecolor{ RenderBackend12::CreateNewShaderSurface(L"gbuffer_basecolor", FShaderSurface::Type::RenderTarget | FShaderSurface::Type::UAV, FResource::Allocation::Transient(gpuFinishFence), DXGI_FORMAT_R8G8B8A8_UNORM, resX, resY, 1, 1) };
	std::unique_ptr<FShaderSurface> gbuffer_normals{ RenderBackend12::CreateNewShaderSurface(L"gbuffer_normals", FShaderSurface::Type::RenderTarget | FShaderSurface::Type::UAV, FResource::Allocation::Transient(gpuFinishFence), DXGI_FORMAT_R16G16_FLOAT, resX, resY, 1, 1) };
	std::unique_ptr<FShaderSurface> gbuffer_metallicRoughnessAo{ RenderBackend12::CreateNewShaderSurface(L"gbuffer_metallic_roughness_ao", FShaderSurface::Type::RenderTarget | FShaderSurface::Type::UAV, FResource::Allocation::Transient(gpuFinishFence), DXGI_FORMAT_R8G8B8A8_UNORM, resX, resY, 1, 1) };
	std::unique_ptr<FShaderBuffer> meshHighlightIndirectArgs{ RenderBackend12::CreateNewShaderBuffer(L"mesh_highlight_indirect_args", FShaderBuffer::Type::Raw, FResource::AccessMode::GpuReadWrite, FResource::Allocation::Transient(gpuFinishFence), sizeof(FIndirectDrawWithRootConstants)) };
	std::unique_ptr<FShaderBuffer> batchArgsBuffer{ RenderBackend12::CreateNewShaderBuffer(L"batch_args_buffer", FShaderBuffer::Type::Raw, FResource::AccessMode::GpuReadWrite, FResource::Allocation::Transient(gpuFinishFence), totalPrimitives * sizeof(FIndirectDrawWithRootConstants)) };
	std::unique_ptr<FShaderBuffer> batchCountsBuffer{ RenderBackend12::CreateNewShaderBuffer(L"batch_counts_buffer", FShaderBuffer::Type::Raw, FResource::AccessMode::GpuReadWrite, FResource::Allocation::Transient(gpuFinishFence), sizeof(uint32_t), true) };
	std::unique_ptr<FShaderBuffer> culledLightCountBuffer{ RenderBackend12::CreateNewShaderBuffer(L"culled_light_count", FShaderBuffer::Type::Raw, FResource::AccessMode::GpuReadWrite, FResource::Allocation::Transient(gpuFinishFence), sizeof(uint32_t), true) };
	std::unique_ptr<FShaderBuffer> culledLightListsBuffer{ RenderBackend12::CreateNewShaderBuffer(L"culled_light_lists", FShaderBuffer::Type::Raw, FResource::AccessMode::GpuReadWrite, FResource::Allocation::Transient(gpuFinishFence), c.MaxLightsPerCluster * c.LightClusterDimX * c.LightClusterDimY * c.LightClusterDimZ * sizeof(uint32_t), true) };
	std::unique_ptr<FShaderBuffer> lightGridBuffer{ RenderBackend12::CreateNewShaderBuffer(L"light_grid", FShaderBuffer::Type::Raw, FResource::AccessMode::GpuReadWrite, FResource::Allocation::Transient(gpuFinishFence), 2 * c.LightClusterDimX * c.LightClusterDimY * c.LightClusterDimZ * sizeof(uint32_t)) }; // Each entry contains an offset into the CulledLightList buffer and the number of lights in the cluster

	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_buffers", D3D12_COMMAND_LIST_TYPE_DIRECT);

	// Light Properties
	std::unique_ptr<FShaderBuffer> packedLightPropertiesBuffer;
	if (!renderState.m_scene->m_globalLightList.empty())
	{
		const size_t bufferSize = renderState.m_scene->m_globalLightList.size() * sizeof(FLight);
		FResourceUploadContext uploader{ bufferSize };

		packedLightPropertiesBuffer.reset(RenderBackend12::CreateNewShaderBuffer(
			L"light_properties_buffer",
			FShaderBuffer::Type::Raw,
			FResource::AccessMode::GpuReadOnly,
			FResource::Allocation::Transient(gpuFinishFence),
			bufferSize,
			false,
			(const uint8_t*)renderState.m_scene->m_globalLightList.data(),
			&uploader));

		
		uploader.SubmitUploads(cmdList);
	}

	// Light Tranforms
	std::unique_ptr<FShaderBuffer> packedLightTransformsBuffer;
	const size_t sceneLightCount = renderState.m_scene->m_sceneLights.GetCount();
	if (sceneLightCount > 0)
	{
		const size_t bufferSize = sceneLightCount * sizeof(Matrix);
		FResourceUploadContext uploader{ bufferSize };

		packedLightTransformsBuffer.reset(RenderBackend12::CreateNewShaderBuffer(
			L"scene_light_transforms",
			FShaderBuffer::Type::Raw,
			FResource::AccessMode::GpuReadOnly,
			FResource::Allocation::Transient(gpuFinishFence),
			bufferSize,
			false,
			(const uint8_t*)renderState.m_scene->m_sceneLights.m_transformList.data(),
			&uploader));

		uploader.SubmitUploads(cmdList);
	}

	// Submit uploads
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

	// Scene Constants
	std::unique_ptr<FSystemBuffer> cbSceneConstants{ RenderBackend12::CreateNewSystemBuffer(
		L"scene_constants_cb",
		FResource::AccessMode::CpuWriteOnly,
		sizeof(FSceneConstants),
		gpuFinishFence, 
		[scene = renderState.m_scene, totalPrimitives, lightPropsBuf = packedLightPropertiesBuffer.get(), lightTransformsBuf = packedLightTransformsBuffer.get()](uint8_t* pDest)
		{
			const size_t lightCount = scene->m_sceneLights.GetCount();

			// Sun direction
			Vector3 L = scene->m_sunDir;
			L.Normalize();

			auto cb = reinterpret_cast<FSceneConstants*>(pDest);
			cb->m_sceneRotation = scene->m_rootTransform;
			cb->m_sunDir = L;
			cb->m_primitiveCount = totalPrimitives;
			cb->m_sceneMeshAccessorsIndex = scene->m_packedMeshAccessors->m_descriptorIndices.SRV;
			cb->m_sceneMeshBufferViewsIndex = scene->m_packedMeshBufferViews->m_descriptorIndices.SRV;
			cb->m_scenePrimitivesIndex = scene->m_packedPrimitives->m_descriptorIndices.SRV;
			cb->m_sceneMaterialBufferIndex = scene->m_packedMaterials->m_descriptorIndices.SRV;
			cb->m_lightCount = scene->m_sceneLights.GetCount();
			cb->m_packedLightIndicesBufferIndex = lightCount > 0 ? scene->m_packedLightIndices->m_descriptorIndices.SRV : -1;
			cb->m_packedLightTransformsBufferIndex = lightCount > 0 ? lightTransformsBuf->m_descriptorIndices.SRV : -1;
			cb->m_packedGlobalLightPropertiesBufferIndex = lightCount > 0 ? lightPropsBuf->m_descriptorIndices.SRV : -1;
			cb->m_sceneBvhIndex = scene->m_tlas->m_descriptorIndices.SRV;
			cb->m_envmapTextureIndex = scene->m_skylight.m_envmapTextureIndex;
			cb->m_skylightProbeIndex = scene->m_skylight.m_shTextureIndex;
			cb->m_envBrdfTextureIndex = s_envBRDF->m_srvIndex;
			cb->m_sunIndex = scene->GetDirectionalLight();
		}) };

	Vector2 pixelJitter = config.EnableTAA && config.Viewmode == (int)Viewmode::Normal ? s_pixelJitterValues[frameIndex % 16] : Vector2{ 0.f, 0.f };

	// View Constants
	std::unique_ptr<FSystemBuffer> cbViewConstants{ RenderBackend12::CreateNewSystemBuffer(
		L"view_constants_cb",
		FResource::AccessMode::CpuWriteOnly,
		sizeof(FViewConstants),
		gpuFinishFence,
		[&renderState, pixelJitter, config](uint8_t* pDest)
		{
			const FView& view = renderState.m_view;
			Matrix jitterMatrix = Matrix::CreateTranslation(pixelJitter.x, pixelJitter.y, 0.f);
			Matrix jitteredProjMatrix = view.m_projectionTransform * jitterMatrix;
			Matrix jitteredViewProjMatrix = view.m_viewTransform * jitteredProjMatrix;

			Matrix viewMatrix_ParallaxCorrected = view.m_viewTransform;
			viewMatrix_ParallaxCorrected.Translation(Vector3::Zero);

			auto cb = reinterpret_cast<FViewConstants*>(pDest);
			cb->m_viewTransform = view.m_viewTransform;
			cb->m_projTransform = jitteredProjMatrix;
			cb->m_viewProjTransform = jitteredViewProjMatrix;
			cb->m_invViewProjTransform = jitteredViewProjMatrix.Invert();
			cb->m_invViewProjTransform_ParallaxCorrected = (viewMatrix_ParallaxCorrected * jitteredProjMatrix).Invert();;
			cb->m_prevViewProjTransform = s_prevViewProjectionTransform;
			cb->m_invProjTransform = jitteredProjMatrix.Invert();
			cb->m_cullViewProjTransform = renderState.m_cullingView.m_viewTransform * jitteredProjMatrix;
			cb->m_eyePos = view.m_position;
			cb->m_exposure = config.Exposure;
			cb->m_aperture = config.Pathtracing_CameraAperture;
			cb->m_focalLength = config.Pathtracing_CameraFocalLength;
			cb->m_nearPlane = config.CameraNearPlane;
			cb->m_resX = renderState.m_resX;
			cb->m_resY = renderState.m_resY;
			cb->m_mouseX = renderState.m_mouseX;
			cb->m_mouseY = renderState.m_mouseY;
			cb->m_viewmode = config.Viewmode;
		}) };


	// Update acceleration structure. Can be used by both pathtracing and raster paths.
	RenderJob::Result updateTLASJob = RenderJob::UpdateTLAS(s_jobSync.get(), renderState.m_scene);
	sceneRenderJobs.push_back(updateTLASJob.m_task);

	if (c.PathTrace)
	{
		if (s_pathtraceCurrentSampleIndex < c.MaxSampleCount)
		{
			RenderJob::PathTracingDesc pathtraceDesc = {};
			pathtraceDesc.targetBuffer = hdrRaytraceSceneColor.get();
			pathtraceDesc.historyBuffer = s_pathtraceHistoryBuffer.get();
			pathtraceDesc.lightPropertiesBuffer = packedLightPropertiesBuffer.get();
			pathtraceDesc.lightTransformsBuffer = packedLightTransformsBuffer.get();
			pathtraceDesc.currentSampleIndex = s_pathtraceCurrentSampleIndex;
			pathtraceDesc.resX = resX;
			pathtraceDesc.resY = resY;
			pathtraceDesc.scene = renderState.m_scene;
			pathtraceDesc.view = &renderState.m_view;
			pathtraceDesc.renderConfig = c;

			RenderJob::Result pathTraceJob = RenderJob::PathTrace(s_jobSync.get(), pathtraceDesc);
			sceneRenderJobs.push_back(pathTraceJob.m_task);

			// Accumulate samples
			s_pathtraceCurrentSampleIndex++;
		}

		RenderJob::TonemapDesc tonemapDesc = {};
		tonemapDesc.source = s_pathtraceHistoryBuffer.get();
		tonemapDesc.target = RenderBackend12::GetBackBuffer();
		tonemapDesc.renderConfig = c;

		RenderJob::Result tonemapJob = RenderJob::Tonemap(s_jobSync.get(), tonemapDesc);
		sceneRenderJobs.push_back(tonemapJob.m_task);

	}
	else
	{
		// Cull Pass & Draw Call Generation
		RenderJob::BatchCullingDesc batchCullDesc = {};
		batchCullDesc.batchArgsBuffer = batchArgsBuffer.get();
		batchCullDesc.batchCountsBuffer = batchCountsBuffer.get();
		batchCullDesc.sceneConstantBuffer = cbSceneConstants.get();
		batchCullDesc.viewConstantBuffer = cbViewConstants.get();
		batchCullDesc.primitiveCount = totalPrimitives;

		RenderJob::Result batchCullJob = RenderJob::BatchCulling(s_jobSync.get(), batchCullDesc);
		sceneRenderJobs.push_back(batchCullJob.m_task);

		// Light Culling
		const size_t punctualLightCount = renderState.m_scene->GetPunctualLightCount();
		if (punctualLightCount > 0 && c.EnableDirectLighting)
		{
			RenderJob::LightCullingDesc lightCullDesc = {};
			lightCullDesc.culledLightCountBuffer = culledLightCountBuffer.get();
			lightCullDesc.culledLightListsBuffer = culledLightListsBuffer.get();
			lightCullDesc.lightGridBuffer = lightGridBuffer.get();
			lightCullDesc.lightPropertiesBuffer = packedLightPropertiesBuffer.get();
			lightCullDesc.lightTransformsBuffer = packedLightTransformsBuffer.get();
			lightCullDesc.renderConfig = c;
			lightCullDesc.scene = renderState.m_scene;
			lightCullDesc.view = &renderState.m_cullingView;
			lightCullDesc.jitter = pixelJitter;
			lightCullDesc.renderConfig = c;

			RenderJob::Result lightCullJob = RenderJob::LightCulling(s_jobSync.get(), lightCullDesc);
			sceneRenderJobs.push_back(lightCullJob.m_task);
		}

		// Visibility Pass
		RenderJob::VisibilityPassDesc visDesc = {};
		visDesc.visBufferTarget = visBuffer.get();
		visDesc.depthStencilTarget = depthBuffer.get();
		visDesc.indirectArgsBuffer = batchArgsBuffer.get();
		visDesc.indirectCountsBuffer = batchCountsBuffer.get();
		visDesc.sceneConstantBuffer = cbSceneConstants.get();
		visDesc.viewConstantBuffer = cbViewConstants.get();
		visDesc.visBufferFormat = visBufferFormat;
		visDesc.resX = resX;
		visDesc.resY = resY;
		visDesc.scenePrimitiveCount = totalPrimitives;

		RenderJob::Result visibilityJob = RenderJob::VisibilityPass(s_jobSync.get(), visDesc);
		sceneRenderJobs.push_back(visibilityJob.m_task);
		s_renderPassSync[VisibilityPass] = visibilityJob.m_syncObj;

		// GBuffer Pass + Emissive
		RenderJob::GBufferPassDesc gbufferDesc = {};
		gbufferDesc.sourceVisBuffer = visBuffer.get();
		gbufferDesc.colorTarget = hdrRasterSceneColor.get();
		gbufferDesc.gbufferTargets[0] = gbuffer_basecolor.get();
		gbufferDesc.gbufferTargets[1] = gbuffer_normals.get();
		gbufferDesc.gbufferTargets[2] = gbuffer_metallicRoughnessAo.get();
		gbufferDesc.depthStencilTarget = depthBuffer.get();
		gbufferDesc.sceneConstantBuffer = cbSceneConstants.get();
		gbufferDesc.viewConstantBuffer = cbViewConstants.get();
		gbufferDesc.resX = resX;
		gbufferDesc.resY = resY;
		gbufferDesc.scene = renderState.m_scene;

		RenderJob::Result gbufferJob = RenderJob::GBufferComputePass(s_jobSync.get(), gbufferDesc);
		sceneRenderJobs.push_back(gbufferJob.m_task);

		RenderJob::Result gbufferDecalsJob = RenderJob::GBufferDecalPass(s_jobSync.get(), gbufferDesc);
		sceneRenderJobs.push_back(gbufferDecalsJob.m_task);

		// Sky Lighting
		RenderJob::SkyLightingDesc skyLightingDesc = {};
		skyLightingDesc.colorTarget = hdrRasterSceneColor.get();
		skyLightingDesc.depthStencilTex = depthBuffer.get();
		skyLightingDesc.gbufferBaseColorTex = gbuffer_basecolor.get();
		skyLightingDesc.gbufferNormalsTex = gbuffer_normals.get();
		skyLightingDesc.gbufferMetallicRoughnessAoTex = gbuffer_metallicRoughnessAo.get();
		skyLightingDesc.renderConfig = c;
		skyLightingDesc.scene = renderState.m_scene;
		skyLightingDesc.view = &renderState.m_view;
		skyLightingDesc.jitter = pixelJitter;
		skyLightingDesc.resX = resX;
		skyLightingDesc.resY = resY;
		skyLightingDesc.envBRDFTex = s_envBRDF.get();

		RenderJob::Result skylightJob = RenderJob::SkyLighting(s_jobSync.get(), skyLightingDesc);
		sceneRenderJobs.push_back(skylightJob.m_task);

		// Direct Lighting
		int directionalLightIndex = renderState.m_scene->GetDirectionalLight();
		if (directionalLightIndex != -1 && c.EnableDirectLighting)
		{
			RenderJob::DirectLightingDesc directLightingDesc = {};
			directLightingDesc.directionalLightIndex = directionalLightIndex;
			directLightingDesc.colorTarget = hdrRasterSceneColor.get();
			directLightingDesc.depthStencilTex = depthBuffer.get();
			directLightingDesc.gbufferBaseColorTex = gbuffer_basecolor.get();
			directLightingDesc.gbufferNormalsTex = gbuffer_normals.get();
			directLightingDesc.gbufferMetallicRoughnessAoTex = gbuffer_metallicRoughnessAo.get();
			directLightingDesc.sceneConstantBuffer = cbSceneConstants.get();
			directLightingDesc.viewConstantBuffer = cbViewConstants.get();
			directLightingDesc.renderConfig = c;
			directLightingDesc.resX = resX;
			directLightingDesc.resY = resY;

			RenderJob::Result directLightingJob = RenderJob::DirectLighting(s_jobSync.get(), directLightingDesc);
			sceneRenderJobs.push_back(directLightingJob.m_task);
		}

		// Clustered Lighting
		const bool bRequiresClear = directionalLightIndex == -1;
		if (punctualLightCount > 0 && c.EnableDirectLighting)
		{
			RenderJob::ClusteredLightingDesc clusteredLightingDesc = {};
			clusteredLightingDesc.lightListsBuffer = culledLightListsBuffer.get();
			clusteredLightingDesc.lightGridBuffer = lightGridBuffer.get();
			clusteredLightingDesc.colorTarget = hdrRasterSceneColor.get();
			clusteredLightingDesc.depthStencilTex = depthBuffer.get();
			clusteredLightingDesc.gbufferBaseColorTex = gbuffer_basecolor.get();
			clusteredLightingDesc.gbufferNormalsTex = gbuffer_normals.get();
			clusteredLightingDesc.gbufferMetallicRoughnessAoTex = gbuffer_metallicRoughnessAo.get();
			clusteredLightingDesc.sceneConstantBuffer = cbSceneConstants.get();
			clusteredLightingDesc.viewConstantBuffer = cbViewConstants.get();
			clusteredLightingDesc.renderConfig = c;
			clusteredLightingDesc.resX = resX;
			clusteredLightingDesc.resY = resY;

			RenderJob::Result clusteredLightingJob = RenderJob::ClusteredLighting(s_jobSync.get(), clusteredLightingDesc, bRequiresClear);
			sceneRenderJobs.push_back(clusteredLightingJob.m_task);
		}

		if (c.EnvSkyMode == (int)EnvSkyMode::Environmentmap)
		{
			// Environmentmap pass
			RenderJob::EnvmapPassDesc envmapDesc = {};
			envmapDesc.colorTarget = hdrRasterSceneColor.get();
			envmapDesc.depthStencilTarget = depthBuffer.get();
			envmapDesc.format = hdrFormat;
			envmapDesc.resX = resX;
			envmapDesc.resY = resY;
			envmapDesc.scene = renderState.m_scene;
			envmapDesc.view = &renderState.m_view;
			envmapDesc.jitter = pixelJitter;
			envmapDesc.renderConfig = c;

			RenderJob::Result envmapJob = RenderJob::EnvironmentmapPass(s_jobSync.get(), envmapDesc);
			sceneRenderJobs.push_back(envmapJob.m_task);
		}
		else
		{
			RenderJob::DynamicSkyPassDesc skyDesc = {};
			skyDesc.colorTarget = hdrRasterSceneColor.get();
			skyDesc.depthStencilTarget = depthBuffer.get();
			skyDesc.format = hdrFormat;
			skyDesc.resX = resX;
			skyDesc.resY = resX;
			skyDesc.scene = renderState.m_scene;
			skyDesc.view = &renderState.m_view;
			skyDesc.jitter = pixelJitter;
			skyDesc.renderConfig = c;

			RenderJob::Result dynamicSkyJob = RenderJob::DynamicSkyPass(s_jobSync.get(), skyDesc);
			sceneRenderJobs.push_back(dynamicSkyJob.m_task);
		}

		if (c.Viewmode != (int)Viewmode::Normal)
		{
			// Debug Viz
			RenderJob::DebugVizDesc desc = {};
			desc.visBuffer = visBuffer.get();
			desc.gbuffers[0] = gbuffer_basecolor.get();
			desc.gbuffers[1] = gbuffer_normals.get();
			desc.gbuffers[2] = gbuffer_metallicRoughnessAo.get();
			desc.target = RenderBackend12::GetBackBuffer();
			desc.depthBuffer = depthBuffer.get();
			desc.indirectArgsBuffer = meshHighlightIndirectArgs.get();
			desc.jitter = pixelJitter;
			desc.renderConfig = c;
			desc.resX = resX;
			desc.resY = resY;
			desc.mouseX = renderState.m_mouseX;
			desc.mouseY = renderState.m_mouseY;
			desc.scene = renderState.m_scene;
			desc.view = &renderState.m_view;

			RenderJob::Result debugVizJob = RenderJob::DebugViz(s_jobSync.get(), desc);
			sceneRenderJobs.push_back(debugVizJob.m_task);

			if (c.Viewmode == (int)Viewmode::ObjectIds || c.Viewmode == (int)Viewmode::TriangleIds)
			{
				RenderJob::HighlightPassDesc desc = {};
				desc.colorTarget = RenderBackend12::GetBackBuffer();
				desc.depthStencilTarget = depthBuffer.get();
				desc.indirectArgsBuffer = meshHighlightIndirectArgs.get();
				desc.resX = resX;
				desc.resY = resY;
				desc.sceneConstantBuffer = cbSceneConstants.get();
				desc.viewConstantBuffer = cbViewConstants.get();
				desc.renderConfig = c;

				RenderJob::Result highlightJob = RenderJob::HighlightPass(s_jobSync.get(), desc);
				sceneRenderJobs.push_back(highlightJob.m_task);
			}
		}
		else
		{
			if (c.EnableTAA)
			{
				const FView& view = renderState.m_view;
				Matrix viewProjectionTransform = view.m_viewTransform * view.m_projectionTransform;

				// TAA Resolve
				RenderJob::TAAResolveDesc resolveDesc = {};
				resolveDesc.source = hdrRasterSceneColor.get();
				resolveDesc.target = s_taaAccumulationBuffer.get();
				resolveDesc.resX = resX;
				resolveDesc.resY = resY;
				resolveDesc.historyIndex = (uint32_t)frameIndex;
				resolveDesc.prevViewProjectionTransform = s_prevViewProjectionTransform;
				resolveDesc.invViewProjectionTransform = viewProjectionTransform.Invert();
				resolveDesc.depthTextureIndex = depthBuffer->m_descriptorIndices.SRV;
				resolveDesc.renderConfig = c;

				RenderJob::Result resolveJob = RenderJob::TAAResolve(s_jobSync.get(), resolveDesc);
				sceneRenderJobs.push_back(resolveJob.m_task);

				// Tonemap
				RenderJob::TonemapDesc tonemapDesc = {};
				tonemapDesc.source = s_taaAccumulationBuffer.get();
				tonemapDesc.target = RenderBackend12::GetBackBuffer();
				tonemapDesc.renderConfig = c;

				RenderJob::Result tonemapJob = RenderJob::Tonemap(s_jobSync.get(), tonemapDesc);
				sceneRenderJobs.push_back(tonemapJob.m_task);

				// Save view projection transform for next frame's reprojection
				s_prevViewProjectionTransform = viewProjectionTransform;
			}
			else
			{
				// Tonemap
				RenderJob::TonemapDesc tonemapDesc = {};
				tonemapDesc.source = hdrRasterSceneColor.get();
				tonemapDesc.target = RenderBackend12::GetBackBuffer();
				tonemapDesc.renderConfig = c;

				RenderJob::Result tonemapJob = RenderJob::Tonemap(s_jobSync.get(), tonemapDesc);
				sceneRenderJobs.push_back(tonemapJob.m_task);
			}
		}
	}
	
	// Wait for all scene render jobs to finish
	auto sceneRenderJoinTask = concurrency::when_all(std::begin(sceneRenderJobs), std::end(sceneRenderJobs));
	sceneRenderJoinTask.wait();

	// Render debug primitives
	FDebugDraw::PassDesc debugDesc = {};
	debugDesc.colorTarget = RenderBackend12::GetBackBuffer();
	debugDesc.depthTarget = depthBuffer.get();
	debugDesc.resX = resX;
	debugDesc.resY = resY;
	debugDesc.scene = renderState.m_scene;
	debugDesc.view = &renderState.m_view;
	debugDesc.renderConfig = c;
	s_debugDrawing.Flush(debugDesc);

	// Render UI
	RenderJob::UIPassDesc uiDesc = { RenderBackend12::GetBackBuffer(), c };
	ImDrawData* imguiDraws = ImGui::GetDrawData();
	if (imguiDraws && imguiDraws->CmdListsCount > 0)
	{
		RenderJob::Result uiJob = RenderJob::UI(s_jobSync.get(), uiDesc);
		uiJob.m_task.wait();
	}

	// Present the frame
	frameIndex++;
	RenderBackend12::PresentDisplay();

	// Read back render stats from the GPU
	auto renderStatsReadbackContext = std::make_shared<FResourceReadbackContext>(s_renderStatsBuffer->m_resource);
	FFenceMarker readbackCompleteCompleteMarker = renderStatsReadbackContext->StageSubresources(s_jobSync->GetCpuFence());
	auto readbackJob = concurrency::create_task([readbackCompleteCompleteMarker]()
	{
		readbackCompleteCompleteMarker.Wait();
	}).then([renderStatsReadbackContext]()
	{
		s_renderStats = *renderStatsReadbackContext->GetBufferData<FRenderStats>();
	});
}

void Renderer::ResetPathtraceAccumulation()
{
	s_pathtraceCurrentSampleIndex = 1;
}
