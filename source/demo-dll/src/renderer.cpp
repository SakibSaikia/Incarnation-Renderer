#include <demo.h>
#include <backend-d3d12.h>
#include <profiling.h>
#include <renderer.h>
#include <ppltasks.h>
#include <sstream>
#include <imgui.h>
#include <dxcapi.h>
#include <random>

namespace Demo
{
	std::unique_ptr<FTexture> s_envBRDF;
	std::unique_ptr<FShaderSurface> s_taaAccumulationBuffer;
	std::unique_ptr<FShaderSurface> s_pathtraceHistoryBuffer;
	std::vector<Vector2> s_pixelJitterValues;
	Matrix s_prevViewProjectionTransform;
	uint32_t s_pathtraceCurrentSampleIndex = 1;
}

// Render Jobs
#include "render-jobs/job-sync.h"
#include "render-jobs/base-pass.inl"
#include "render-jobs/environment-sky.inl"
#include "render-jobs/msaa-resolve.inl"
#include "render-jobs/taa-resolve.inl"
#include "render-jobs/ui-pass.inl"
#include "render-jobs/present.inl"
#include "render-jobs/path-tracing.inl"
#include "render-jobs/tonemap.inl"
#include "render-jobs/update-tlas.inl"
#include "render-jobs/visibility-pass.inl"
#include "render-jobs/gbuffer-pass.inl"
#include "render-jobs/debug-visualization.inl"
#include "render-jobs/highlight-pass.inl"
#include "render-jobs/batch-culling.inl"

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

	std::unique_ptr<FTexture> GenerateEnvBrdfTexture(const uint32_t width, const uint32_t height)
	{
		auto brdfUav = RenderBackend12::CreateSurface(L"env_brdf_uav", SurfaceType::UAV, DXGI_FORMAT_R16G16_FLOAT, width, height);

		// Compute CL
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"hdr_preprocess", D3D12_COMMAND_LIST_TYPE_DIRECT);
		D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "integrate_env_bdrf", 0);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"brdf_integration_rootsig",
				cmdList,
				FRootsigDesc{ L"image-based-lighting/split-sum-approx/brdf-integration.hlsl", L"rootsig", L"rootsig_1_1" });

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
			} rootConstants = { width, height, brdfUav->m_uavIndices[0], 1024 };

			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);

			// Dispatch
			size_t threadGroupCount = std::max<size_t>(std::ceil(width / 16), 1);
			d3dCmdList->Dispatch(threadGroupCount, threadGroupCount, 1);
		}

		// Copy from UAV to destination texture
		brdfUav->m_resource->Transition(cmdList, brdfUav->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE);
		auto brdfTex = RenderBackend12::CreateTexture(L"env_brdf_tex", TextureType::Tex2D, DXGI_FORMAT_R16G16_FLOAT, width, height, 1, 1, D3D12_RESOURCE_STATE_COPY_DEST);
		d3dCmdList->CopyResource(brdfTex->m_resource->m_d3dResource, brdfUav->m_resource->m_d3dResource);
		brdfTex->m_resource->Transition(cmdList, brdfTex->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

		return std::move(brdfTex);
	}

	std::unique_ptr<FTexture> GenerateWhiteNoiseTextures(const uint32_t width, const uint32_t height, const uint32_t depth)
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
		auto noiseTexArray = RenderBackend12::CreateTexture(L"white_noise_array", TextureType::Tex2DArray, DXGI_FORMAT_R8_UNORM, width, height, 1, depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, noiseImages.data(), &uploader);
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_noise_texture", D3D12_COMMAND_LIST_TYPE_DIRECT);
		uploader.SubmitUploads(cmdList);
		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

		return std::move(noiseTexArray);
	}
}

void Demo::InitializeRenderer(const uint32_t resX, const uint32_t resY)
{

	s_envBRDF = GenerateEnvBrdfTexture(512, 512);

	s_pathtraceHistoryBuffer = RenderBackend12::CreateSurface(L"hdr_history_buffer_rt", SurfaceType::UAV, DXGI_FORMAT_R11G11B10_FLOAT, resX, resY, 1, 1);
	s_taaAccumulationBuffer = RenderBackend12::CreateSurface(L"taa_accumulation_buffer_raster", SurfaceType::UAV, DXGI_FORMAT_R11G11B10_FLOAT, resX, resY, 1, 1);

	// Generate Pixel Jitter Values
	for (int sampleIdx = 0; sampleIdx < 16; ++sampleIdx)
	{
		Vector2 pixelJitter = { Halton(sampleIdx, 2), Halton(sampleIdx, 3) };
		pixelJitter = 2.f * (pixelJitter - Vector2(0.5, 0.5)) / Vector2(resX, resY);
		s_pixelJitterValues.push_back(pixelJitter);
	}
}

void Demo::TeardownRenderer()
{
	s_envBRDF.reset(nullptr);
	s_pathtraceHistoryBuffer.reset(nullptr);
	s_taaAccumulationBuffer.reset(nullptr);
}

void Demo::Render(const uint32_t resX, const uint32_t resY)
{
	RenderBackend12::WaitForSwapChain();

	// Create a immutable copy of the render state for render jobs to use
	const FRenderState renderState = Demo::GetRenderState();
	const FConfig& c = renderState.m_config;

	if (renderState.m_suspendRendering)
		return;

	SCOPED_CPU_EVENT("render", PIX_COLOR_DEFAULT);

	std::vector<concurrency::task<void>> renderJobs;
	static RenderJob::Sync jobSync;

	static uint64_t frameIndex = 0;
	size_t totalPrimitives = 0;
	for (const auto& mesh : renderState.m_scene->m_sceneMeshes.m_entityList)
	{
		totalPrimitives += mesh.m_primitives.size();
	}

	// These resources need to be kept alive until all the render jobs have finished and joined
	const DXGI_FORMAT hdrFormat = DXGI_FORMAT_R11G11B10_FLOAT;
	const DXGI_FORMAT visBufferFormat = DXGI_FORMAT_R32_UINT;
	std::unique_ptr<FShaderSurface> hdrRasterSceneColor = RenderBackend12::CreateSurface(L"hdr_scene_color_raster", SurfaceType::RenderTarget, hdrFormat, resX, resY);
	std::unique_ptr<FShaderSurface> depthBuffer = RenderBackend12::CreateSurface(L"depth_buffer_raster", SurfaceType::DepthStencil, DXGI_FORMAT_D32_FLOAT, resX, resY);
	std::unique_ptr<FShaderSurface> hdrRaytraceSceneColor = RenderBackend12::CreateSurface(L"hdr_scene_color_rt", SurfaceType::UAV, DXGI_FORMAT_R16G16B16A16_FLOAT, resX, resY, 1, 1, 1, 1, true, true);
	std::unique_ptr<FShaderSurface> visBuffer = RenderBackend12::CreateSurface(L"vis_buffer_raster", SurfaceType::RenderTarget, visBufferFormat, resX, resY);
	std::unique_ptr<FShaderSurface> gbuffer_basecolor = RenderBackend12::CreateSurface(L"gbuffer_basecolor", SurfaceType::RenderTarget | SurfaceType::UAV, DXGI_FORMAT_R8G8B8A8_UNORM, resX, resY, 1, 1);
	std::unique_ptr<FShaderSurface> gbuffer_normals = RenderBackend12::CreateSurface(L"gbuffer_normals", SurfaceType::RenderTarget | SurfaceType::UAV, DXGI_FORMAT_R16G16_FLOAT, resX, resY, 1, 1);
	std::unique_ptr<FShaderSurface> gbuffer_metallicRoughnessAo = RenderBackend12::CreateSurface(L"gbuffer_metallic_roughness_ao", SurfaceType::RenderTarget | SurfaceType::UAV, DXGI_FORMAT_R8G8B8A8_UNORM, resX, resY, 1, 1);
	std::unique_ptr<FShaderBuffer> meshHighlightIndirectArgs = RenderBackend12::CreateBuffer(L"mesh_highlight_indirect_args", BufferType::Raw, ResourceAccessMode::GpuReadWrite, ResourceAllocationType::Pooled, sizeof(FDrawWithRootConstants));
	std::unique_ptr<FShaderBuffer> batchArgsBuffer = RenderBackend12::CreateBuffer(L"batch_args_buffer", BufferType::Raw, ResourceAccessMode::GpuReadWrite, ResourceAllocationType::Pooled, totalPrimitives * sizeof(FDrawWithRootConstants));
	std::unique_ptr<FShaderBuffer> batchCountsBuffer = RenderBackend12::CreateBuffer(L"batch_counts_buffer", BufferType::Raw, ResourceAccessMode::GpuReadWrite, ResourceAllocationType::Pooled, sizeof(uint32_t), true);

	// Update acceleration structure. Can be used by both pathtracing and raster paths.
	renderJobs.push_back(RenderJob::UpdateTLAS(jobSync, renderState.m_scene));

	if (c.PathTrace)
	{
		if (s_pathtraceCurrentSampleIndex < c.MaxSampleCount)
		{
			RenderJob::PathTracingDesc pathtraceDesc = {};
			pathtraceDesc.targetBuffer = hdrRaytraceSceneColor.get();
			pathtraceDesc.historyBuffer = Demo::s_pathtraceHistoryBuffer.get();
			pathtraceDesc.currentSampleIndex = s_pathtraceCurrentSampleIndex;
			pathtraceDesc.resX = resX;
			pathtraceDesc.resY = resY;
			pathtraceDesc.scene = renderState.m_scene;
			pathtraceDesc.view = &renderState.m_view;
			pathtraceDesc.renderConfig = c;
			renderJobs.push_back(RenderJob::PathTrace(jobSync, pathtraceDesc));

			// Accumulate samples
			s_pathtraceCurrentSampleIndex++;
		}

		RenderJob::TonemapDesc tonemapDesc = {};
		tonemapDesc.source = Demo::s_pathtraceHistoryBuffer.get();
		tonemapDesc.target = RenderBackend12::GetBackBuffer();
		tonemapDesc.renderConfig = c;
		renderJobs.push_back(RenderJob::Tonemap(jobSync, tonemapDesc));

	}
	else
	{
		Vector2 pixelJitter = c.EnableTAA && c.Viewmode == (int)Viewmode::Normal ? s_pixelJitterValues[frameIndex % 16] : Vector2{ 0.f, 0.f };

		// Cull Pass & Draw Call Generation
		RenderJob::BatchCullingDesc cullDesc = {};
		cullDesc.batchArgsBuffer = batchArgsBuffer.get();
		cullDesc.batchCountsBuffer = batchCountsBuffer.get();
		cullDesc.scene = renderState.m_scene;
		cullDesc.view = &renderState.m_cullingView;
		cullDesc.primitiveCount = totalPrimitives;
		cullDesc.jitter = pixelJitter;
		renderJobs.push_back(RenderJob::BatchCulling(jobSync, cullDesc));

		// Visibility Pass
		RenderJob::VisibilityPassDesc visDesc = {};
		visDesc.visBufferTarget = visBuffer.get();
		visDesc.depthStencilTarget = depthBuffer.get();
		visDesc.indirectArgsBuffer = batchArgsBuffer.get();
		visDesc.indirectCountsBuffer = batchCountsBuffer.get();
		visDesc.visBufferFormat = visBufferFormat;
		visDesc.resX = resX;
		visDesc.resY = resY;
		visDesc.scene = renderState.m_scene;
		visDesc.view = &renderState.m_view;
		visDesc.scenePrimitiveCount = totalPrimitives;
		visDesc.jitter = pixelJitter;
		visDesc.renderConfig = c;
		renderJobs.push_back(RenderJob::VisibilityPass(jobSync, visDesc));

		// GBuffer Pass
		RenderJob::GBufferPassDesc gbufferDesc = {};
		gbufferDesc.sourceVisBuffer = visBuffer.get();
		gbufferDesc.gbufferTargets[0] = gbuffer_basecolor.get();
		gbufferDesc.gbufferTargets[1] = gbuffer_normals.get();
		gbufferDesc.gbufferTargets[2] = gbuffer_metallicRoughnessAo.get();
		gbufferDesc.depthStencilTarget = depthBuffer.get();
		gbufferDesc.resX = resX;
		gbufferDesc.resY = resY;
		gbufferDesc.scene = renderState.m_scene;
		gbufferDesc.view = &renderState.m_view;
		gbufferDesc.jitter = pixelJitter;
		gbufferDesc.renderConfig = c;
		renderJobs.push_back(RenderJob::GBufferComputePass(jobSync, gbufferDesc));
		renderJobs.push_back(RenderJob::GBufferDecalPass(jobSync, gbufferDesc));

		// Base pass
		RenderJob::BasePassDesc baseDesc = {};
		baseDesc.colorTarget = hdrRasterSceneColor.get();
		baseDesc.depthStencilTarget = depthBuffer.get();
		baseDesc.format = hdrFormat;
		baseDesc.resX = resX;
		baseDesc.resY = resY;
		baseDesc.scene = renderState.m_scene;
		baseDesc.view = &renderState.m_view;
		baseDesc.jitter = pixelJitter;
		baseDesc.renderConfig = c;
		renderJobs.push_back(RenderJob::BasePass(jobSync, baseDesc));
		renderJobs.push_back(RenderJob::EnvironmentSkyPass(jobSync, baseDesc));

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
			renderJobs.push_back(RenderJob::DebugViz(jobSync, desc));

			if (c.Viewmode == (int)Viewmode::ObjectIds || c.Viewmode == (int)Viewmode::TriangleIds)
			{
				RenderJob::HighlightPassDesc desc = {};
				desc.colorTarget = RenderBackend12::GetBackBuffer();
				desc.depthStencilTarget = depthBuffer.get();
				desc.indirectArgsBuffer = meshHighlightIndirectArgs.get();
				desc.resX = resX;
				desc.resY = resY;
				desc.scene = renderState.m_scene;
				desc.view = &renderState.m_view;
				desc.renderConfig = c;
				renderJobs.push_back(RenderJob::HighlightPass(jobSync, desc));
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
				resolveDesc.target = Demo::s_taaAccumulationBuffer.get();
				resolveDesc.resX = resX;
				resolveDesc.resY = resY;
				resolveDesc.historyIndex = (uint32_t)frameIndex;
				resolveDesc.prevViewProjectionTransform = s_prevViewProjectionTransform;
				resolveDesc.invViewProjectionTransform = viewProjectionTransform.Invert();
				resolveDesc.depthTextureIndex = depthBuffer->m_srvIndex;
				resolveDesc.renderConfig = c;
				renderJobs.push_back(RenderJob::TAAResolve(jobSync, resolveDesc));

				// Tonemap
				RenderJob::TonemapDesc tonemapDesc = {};
				tonemapDesc.source = Demo::s_taaAccumulationBuffer.get();
				tonemapDesc.target = RenderBackend12::GetBackBuffer();
				tonemapDesc.renderConfig = c;
				renderJobs.push_back(RenderJob::Tonemap(jobSync, tonemapDesc));

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
				renderJobs.push_back(RenderJob::Tonemap(jobSync, tonemapDesc));
			}
		}
	}

	frameIndex++;

	// UI
	RenderJob::UIPassDesc uiDesc = { RenderBackend12::GetBackBuffer(), c };
	ImDrawData* imguiDraws = ImGui::GetDrawData();
	if (imguiDraws && imguiDraws->CmdListsCount > 0)
	{
		renderJobs.push_back(RenderJob::UI(jobSync, uiDesc));
	}

	// Present
	RenderJob::PresentDesc presentDesc = { RenderBackend12::GetBackBuffer() };
	renderJobs.push_back(RenderJob::Present(jobSync, presentDesc));
	
	// Wait for all render jobs to finish
	auto joinTask = concurrency::when_all(std::begin(renderJobs), std::end(renderJobs));
	joinTask.wait();

	RenderBackend12::PresentDisplay();
}

void Demo::ResetPathtraceAccumulation()
{
	s_pathtraceCurrentSampleIndex = 1;
}
