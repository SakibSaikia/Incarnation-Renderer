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

// Render Jobs
#include "render-jobs/job-sync.h"
#include "render-jobs/base-pass.inl"
#include "render-jobs/environment-sky.inl"
#include "render-jobs/msaa-resolve.inl"
#include "render-jobs/ui-pass.inl"
#include "render-jobs/present.inl"
#include "render-jobs/path-tracing.inl"
#include "render-jobs/tonemap.inl"
#include "render-jobs/update-tlas.inl"

void Demo::Render(const uint32_t resX, const uint32_t resY)
{
	if (Demo::IsRenderingPaused())
		return;

	SCOPED_CPU_EVENT("render", PIX_COLOR_DEFAULT);

	std::vector<concurrency::task<void>> renderJobs;
	static RenderJob::Sync jobSync;

	bool bCapturing = false;
	if (Demo::GetPathtraceHistoryFrameCount() == 1)
	{
		RenderBackend12::BeginCapture();
		bCapturing = true;
	}

	// These resources need to be kept alive until all the render jobs have finished and joined
	const uint32_t sampleCount = 4;
	const DXGI_FORMAT hdrFormat = DXGI_FORMAT_R11G11B10_FLOAT;
	std::unique_ptr<FRenderTexture> hdrRasterSceneColor = RenderBackend12::CreateRenderTexture(L"hdr_scene_color_raster_msaa", hdrFormat, resX, resY, 1, 1, sampleCount);
	std::unique_ptr<FRenderTexture> depthBuffer = RenderBackend12::CreateDepthStencilTexture(L"depth_buffer_raster", DXGI_FORMAT_D32_FLOAT, resX, resY, 1, sampleCount);
	std::unique_ptr<FRenderTexture> hdrRasterSceneColorResolve = RenderBackend12::CreateRenderTexture(L"hdr_scene_color_raster", hdrFormat, resX, resY, 1, 1, 1);
	std::unique_ptr<FBindlessUav> hdrRaytraceSceneColor = RenderBackend12::CreateBindlessUavTexture(L"hdr_scene_color_rt", hdrFormat, resX, resY, 1, 1);

	if (Config::g_pathTrace)
	{
		static int cycledArrayIndex = 0;
		cycledArrayIndex = (cycledArrayIndex + 1) % Config::g_whiteNoiseArrayCount;

		uint32_t& pathtraceHistory = Demo::GetPathtraceHistoryFrameCount();
		Vector2 pixelJitter = { Halton(pathtraceHistory, 2), Halton(pathtraceHistory, 3) };

		renderJobs.push_back(RenderJob::UpdateTLAS(jobSync, GetScene()));

		RenderJob::PathTracingDesc pathtraceDesc = {};
		pathtraceDesc.targetBuffer = hdrRaytraceSceneColor.get();
		pathtraceDesc.historyBuffer = Demo::GetPathtraceHistoryBuffer();
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
		tonemapDesc.source = Demo::GetPathtraceHistoryBuffer();
		tonemapDesc.target = RenderBackend12::GetBackBuffer();
		tonemapDesc.format = Config::g_backBufferFormat;
		renderJobs.push_back(RenderJob::Tonemap(jobSync, tonemapDesc));

		// Accumulate history frames
		pathtraceHistory++;
	}
	else
	{
		// Base pass
		RenderJob::BasePassDesc baseDesc = {};
		baseDesc.colorTarget = hdrRasterSceneColor.get();
		baseDesc.depthStencilTarget = depthBuffer.get();
		baseDesc.format = hdrFormat;
		baseDesc.resX = resX;
		baseDesc.resY = resY;
		baseDesc.sampleCount = sampleCount;
		baseDesc.scene = GetScene();
		baseDesc.view = GetView();
		renderJobs.push_back(RenderJob::BasePass(jobSync, baseDesc));
		renderJobs.push_back(RenderJob::EnvironmentSkyPass(jobSync, baseDesc));

		// NOTE: The following is not technically correct. Tonemapping should be performed 
		// before or during the resolve so that we are filtering the correct signal response
		// i.e the color and not the radiance. Refer the following for more information:
		// https://therealmjp.github.io/posts/msaa-overview/#fnref:3

		// MSAA Resolve
		RenderJob::MSAAResolveDesc resolveDesc = {};
		resolveDesc.colorSource = hdrRasterSceneColor.get();
		resolveDesc.colorTarget = hdrRasterSceneColorResolve.get();
		resolveDesc.format = hdrFormat;
		resolveDesc.resX = resX;
		resolveDesc.resY = resY;
		renderJobs.push_back(RenderJob::MSAAResolve(jobSync, resolveDesc));

		// Tonemap
		RenderJob::TonemapDesc<FRenderTexture> tonemapDesc = {};
		tonemapDesc.source = hdrRasterSceneColorResolve.get();
		tonemapDesc.target = RenderBackend12::GetBackBuffer();
		tonemapDesc.format = Config::g_backBufferFormat;
		renderJobs.push_back(RenderJob::Tonemap(jobSync, tonemapDesc));
	}

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

	if (bCapturing)
	{
		RenderBackend12::EndCapture();
	}

	RenderBackend12::PresentDisplay();
}

std::unique_ptr<FBindlessShaderResource> Demo::GenerateEnvBrdfTexture(const uint32_t width, const uint32_t height)
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

std::unique_ptr<FBindlessShaderResource> Demo::GenerateWhiteNoiseTextures(const uint32_t width, const uint32_t height, const uint32_t depth)
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