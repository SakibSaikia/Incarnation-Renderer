#include <demo.h>
#include <backend-d3d12.h>
#include <profiling.h>
#include <common.h>
#include <renderer.h>
#include <ppltasks.h>
#include <sstream>
#include <imgui.h>
#include <dxcapi.h>

// Render Jobs
#include "render-jobs/job-sync.h"
#include "render-jobs/base-pass.inl"
#include "render-jobs/environment-sky.inl"
#include "render-jobs/post-process.inl"
#include "render-jobs/ui-pass.inl"
#include "render-jobs/present.inl"
#include "render-jobs/path-tracing.inl"
#include "render-jobs/path-tracing-resolve.inl"

void Demo::Render(const uint32_t resX, const uint32_t resY)
{
	if (Demo::IsRenderingPaused())
		return;

	SCOPED_CPU_EVENT("render", PIX_COLOR_DEFAULT);

	std::vector<concurrency::task<void>> renderJobs;
	static RenderJob::Sync jobSync;

	// These resources need to be kept alive until all the render jobs have finished and joined
	const uint32_t sampleCount = 4;
	std::unique_ptr<FRenderTexture> colorBuffer = RenderBackend12::CreateRenderTexture(L"scene_color", Config::g_backBufferFormat, resX, resY, 1, 1, sampleCount);
	std::unique_ptr<FRenderTexture> depthBuffer = RenderBackend12::CreateDepthStencilTexture(L"depth_buffer", DXGI_FORMAT_D32_FLOAT, resX, resY, 1, sampleCount);
	std::unique_ptr<FBindlessUav> raytraceOutput = RenderBackend12::CreateBindlessUavTexture(L"raytrace_output", DXGI_FORMAT_R8G8B8A8_UNORM, resX, resY, 1, 1);

	if (Config::g_pathTrace)
	{
		RenderJob::PathTracingDesc pathtraceDesc = {};
		pathtraceDesc.target = raytraceOutput.get();
		pathtraceDesc.resX = resX;
		pathtraceDesc.resY = resY;
		pathtraceDesc.scene = GetScene();
		pathtraceDesc.view = GetView();
		renderJobs.push_back(RenderJob::PathTrace(jobSync, pathtraceDesc));

		RenderJob::PathtraceResolvePassDesc resolveDesc = {};
		resolveDesc.uavSource = raytraceOutput.get();
		resolveDesc.colorTarget = RenderBackend12::GetBackBuffer();
		renderJobs.push_back(RenderJob::PathtraceResolve(jobSync, resolveDesc));
	}
	else
	{
		// Base pass
		RenderJob::BasePassDesc baseDesc = {};
		baseDesc.colorTarget = colorBuffer.get();
		baseDesc.depthStencilTarget = depthBuffer.get();
		baseDesc.resX = resX;
		baseDesc.resY = resY;
		baseDesc.sampleCount = sampleCount;
		baseDesc.scene = GetScene();
		baseDesc.view = GetView();
		renderJobs.push_back(RenderJob::BasePass(jobSync, baseDesc));
		renderJobs.push_back(RenderJob::EnvironmentSkyPass(jobSync, baseDesc));

		// Post Process
		RenderJob::PostprocessPassDesc postDesc = {};
		postDesc.colorSource = colorBuffer.get();
		postDesc.colorTarget = RenderBackend12::GetBackBuffer();
		postDesc.resX = resX;
		postDesc.resY = resY;
		postDesc.view = GetView();
		renderJobs.push_back(RenderJob::Postprocess(jobSync, postDesc));
	}

	// UI
	RenderJob::UIPassDesc uiDesc = { RenderBackend12::GetBackBuffer() };
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