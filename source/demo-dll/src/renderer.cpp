#include <demo.h>
#include <backend-d3d12.h>
#include <profiling.h>
#include <common.h>
#include <renderer.h>
#include <ppltasks.h>
#include <sstream>
#include <imgui.h>
#include <dxcapi.h>
#include <random>

namespace Demo
{
	std::unique_ptr<FBindlessShaderResource> s_envBRDF;
	std::unique_ptr<FBindlessShaderResource> s_whiteNoise;
	std::unique_ptr<FBindlessUav> s_taaAccumulationBuffer;
	std::unique_ptr<FBindlessUav> s_pathtraceHistoryBuffer;
	std::vector<Vector2> s_pixelJitterValues;
	Matrix s_prevViewProjectionTransform;
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

	std::unique_ptr<FBindlessShaderResource> GenerateEnvBrdfTexture(const uint32_t width, const uint32_t height)
	{
		auto brdfUav = RenderBackend12::CreateBindlessUavTexture(L"env_brdf_uav", DXGI_FORMAT_R16G16_FLOAT, width, height, 1, 1);

		// Compute CL
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
		cmdList->SetName(L"hdr_preprocess");
		D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "integrate_env_bdrf", 0);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({
				L"image-based-lighting/split-sum-approx/brdf-integration.hlsl",
				L"rootsig",
				L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig.get());

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({
				L"image-based-lighting/split-sum-approx/brdf-integration.hlsl",
				L"cs_main",
				L"THREAD_GROUP_SIZE_X=16 THREAD_GROUP_SIZE_Y=16" ,
				L"cs_6_6" });

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = rootsig.get();
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
		auto brdfTex = RenderBackend12::CreateBindlessTexture(L"env_brdf_tex", BindlessResourceType::Texture2D, DXGI_FORMAT_R16G16_FLOAT, width, height, 1, 1, D3D12_RESOURCE_STATE_COPY_DEST);
		d3dCmdList->CopyResource(brdfTex->m_resource->m_d3dResource, brdfUav->m_resource->m_d3dResource);
		brdfTex->m_resource->Transition(cmdList, brdfTex->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

		return std::move(brdfTex);
	}

	std::unique_ptr<FBindlessShaderResource> GenerateWhiteNoiseTextures(const uint32_t width, const uint32_t height, const uint32_t depth)
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
		auto noiseTexArray = RenderBackend12::CreateBindlessTexture(L"white_noise_array", BindlessResourceType::Texture2DArray, DXGI_FORMAT_R8_UNORM, width, height, 1, depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, noiseImages.data(), &uploader);
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
		uploader.SubmitUploads(cmdList);
		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

		return std::move(noiseTexArray);
	}
}

void Demo::InitializeRenderer(const uint32_t resX, const uint32_t resY)
{

	s_envBRDF = GenerateEnvBrdfTexture(512, 512);
	s_whiteNoise = GenerateWhiteNoiseTextures(Config::g_whiteNoiseTextureSize, Config::g_whiteNoiseTextureSize, Config::g_whiteNoiseArrayCount);

	s_pathtraceHistoryBuffer = RenderBackend12::CreateBindlessUavTexture(L"hdr_history_buffer_rt", DXGI_FORMAT_R11G11B10_FLOAT, resX, resY, 1, 1);
	s_taaAccumulationBuffer = RenderBackend12::CreateBindlessUavTexture(L"taa_accumulation_buffer_raster", DXGI_FORMAT_R11G11B10_FLOAT, resX, resY, 1, 1);

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
	s_whiteNoise.reset(nullptr);
	s_pathtraceHistoryBuffer.reset(nullptr);
	s_taaAccumulationBuffer.reset(nullptr);
}

void Demo::Render(const uint32_t resX, const uint32_t resY)
{
	if (Demo::IsRenderingPaused())
		return;

	SCOPED_CPU_EVENT("render", PIX_COLOR_DEFAULT);

	std::vector<concurrency::task<void>> renderJobs;
	static RenderJob::Sync jobSync;

	static uint64_t frameIndex = 0;

	// These resources need to be kept alive until all the render jobs have finished and joined
	const DXGI_FORMAT hdrFormat = DXGI_FORMAT_R11G11B10_FLOAT;
	std::unique_ptr<FRenderTexture> hdrRasterSceneColor = RenderBackend12::CreateRenderTexture(L"hdr_scene_color_raster", hdrFormat, resX, resY, 1, 1, 1);
	std::unique_ptr<FRenderTexture> depthBuffer = RenderBackend12::CreateDepthStencilTexture(L"depth_buffer_raster", DXGI_FORMAT_D32_FLOAT, resX, resY, 1, 1);
	std::unique_ptr<FBindlessUav> hdrRaytraceSceneColor = RenderBackend12::CreateBindlessUavTexture(L"hdr_scene_color_rt", hdrFormat, resX, resY, 1, 1, true, true);

	if (Config::g_pathTrace)
	{
		int cycledArrayIndex = frameIndex % Config::g_whiteNoiseArrayCount;

		uint32_t& pathtraceHistory = Demo::GetPathtraceHistoryFrameCount();
		Vector2 pixelJitter = { Halton(pathtraceHistory, 2), Halton(pathtraceHistory, 3) };

		renderJobs.push_back(RenderJob::UpdateTLAS(jobSync, GetScene()));

		RenderJob::PathTracingDesc pathtraceDesc = {};
		pathtraceDesc.targetBuffer = hdrRaytraceSceneColor.get();
		pathtraceDesc.historyBuffer = Demo::s_pathtraceHistoryBuffer.get();
		pathtraceDesc.historyFrameCount = Demo::GetPathtraceHistoryFrameCount();
		pathtraceDesc.resX = resX;
		pathtraceDesc.resY = resY;
		pathtraceDesc.scene = GetScene();
		pathtraceDesc.view = GetView();
		pathtraceDesc.whiteNoiseArrayIndex = cycledArrayIndex;
		pathtraceDesc.whiteNoiseTextureSize = Config::g_whiteNoiseTextureSize;
		pathtraceDesc.jitter = pixelJitter;
		renderJobs.push_back(RenderJob::PathTrace(jobSync, pathtraceDesc));

		RenderJob::TonemapDesc<FBindlessUav> tonemapDesc = {};
		tonemapDesc.source = Demo::s_pathtraceHistoryBuffer.get();
		tonemapDesc.target = RenderBackend12::GetBackBuffer();
		tonemapDesc.format = Config::g_backBufferFormat;
		renderJobs.push_back(RenderJob::Tonemap(jobSync, tonemapDesc));

		// Accumulate history frames
		pathtraceHistory++;
	}
	else
	{
		Vector2 pixelJitter = Config::g_enableTAA ? s_pixelJitterValues[frameIndex % 16] : Vector2{ 0.f, 0.f };

		// Base pass
		RenderJob::BasePassDesc baseDesc = {};
		baseDesc.colorTarget = hdrRasterSceneColor.get();
		baseDesc.depthStencilTarget = depthBuffer.get();
		baseDesc.format = hdrFormat;
		baseDesc.resX = resX;
		baseDesc.resY = resY;
		baseDesc.scene = GetScene();
		baseDesc.view = GetView();
		baseDesc.jitter = pixelJitter;
		renderJobs.push_back(RenderJob::BasePass(jobSync, baseDesc));
		renderJobs.push_back(RenderJob::EnvironmentSkyPass(jobSync, baseDesc));

		
		if (Config::g_enableTAA)
		{
			const FView* view = GetView();
			Matrix viewProjectionTransform = view->m_viewTransform * view->m_projectionTransform;

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
			renderJobs.push_back(RenderJob::TAAResolve(jobSync, resolveDesc));

			// Tonemap
			RenderJob::TonemapDesc<FBindlessUav> tonemapDesc = {};
			tonemapDesc.source = Demo::s_taaAccumulationBuffer.get();
			tonemapDesc.target = RenderBackend12::GetBackBuffer();
			tonemapDesc.format = Config::g_backBufferFormat;
			renderJobs.push_back(RenderJob::Tonemap(jobSync, tonemapDesc));

			// Save view projection transform for next frame's reprojection
			s_prevViewProjectionTransform = viewProjectionTransform;
		}
		else
		{
			// Tonemap
			RenderJob::TonemapDesc<FRenderTexture> tonemapDesc = {};
			tonemapDesc.source = hdrRasterSceneColor.get();
			tonemapDesc.target = RenderBackend12::GetBackBuffer();
			tonemapDesc.format = Config::g_backBufferFormat;
			renderJobs.push_back(RenderJob::Tonemap(jobSync, tonemapDesc));
		}
	}

	frameIndex++;

	// UI
	RenderJob::UIPassDesc uiDesc = { RenderBackend12::GetBackBuffer(), Config::g_backBufferFormat };
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
