#include <demo-interface.h>
#include <demo.h>
#include <profiling.h>
#include <backend-d3d12.h>
#include <shadercompiler.h>
#include <renderer.h>
#include <ui.h>
#include <mesh-utils.h>
#include <gpu-shared-types.h>
#include <concurrent_unordered_map.h>
#include <ppltasks.h>
#include <ppl.h>

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Demo
//-----------------------------------------------------------------------------------------------------------------------------------------------

namespace Demo
{
	App s_demoApp;
	FTextureCache s_textureCache;
	FSamplerCache s_samplerCache;
}

bool Demo::Initialize(const HWND& windowHandle, const uint32_t resX, const uint32_t resY)
{
	return s_demoApp.Initialize(windowHandle, resX, resY);
}

void Demo::Tick(float deltaTime)
{
	s_demoApp.Tick(deltaTime);
}

void Demo::HeartbeatThread()
{
	while (true)
	{
		if (!Renderer::Status::IsPaused())
		{
			RenderBackend12::RecompileModifiedShaders(&Renderer::ResetPathtraceAccumulation);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}
}

void Demo::Teardown(HWND& windowHandle)
{
	s_textureCache.Clear();
	s_samplerCache.Clear();
	s_demoApp.Teardown(windowHandle);
}

void Demo::OnMouseMove(WPARAM buttonState, int x, int y)
{
	if (!UI::HasFocus())
	{
		s_demoApp.OnMouseMove(buttonState, x, y);
	}
}

void Demo::Render(const uint32_t resX, const uint32_t resY)
{
	s_demoApp.Render(resX, resY);
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT Demo::WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
}

const FConfig& Demo::GetConfig()
{
	return s_demoApp.GetConfig();
}

float Demo::GetAspectRatio()
{
	return s_demoApp.m_aspectRatio;
}

FTextureCache& Demo::GetTextureCache()
{
	return s_textureCache;
}

FSamplerCache& Demo::GetSamplerCache()
{
	return s_samplerCache;
}

Matrix Demo::Utils::GetReverseZInfinitePerspectiveFovLH(float fov, float r, float n)
{
	return Matrix{
		1.f / (r * tan(fov / 2.f)),	0.f,						0.f,	0.f,
		0.f,							1.f / tan(fov / 2.f),	0.f,	0.f,
		0.f,							0.f,						0.f,	1.f,
		0.f,							0.f,						n,		0.f
	};
}

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														View
//-----------------------------------------------------------------------------------------------------------------------------------------------

void FView::Tick(const float deltaTime, const FController* controller)
{
	SCOPED_CPU_EVENT("tick_view", PIX_COLOR_DEFAULT);

	bool updateView = false;


	// Walk
	if (controller->MoveForward())
	{
		m_position += Demo::GetConfig().CameraSpeed * deltaTime * m_look;
		updateView = true;
	}
	else if (controller->MoveBack())
	{
		m_position -= Demo::GetConfig().CameraSpeed * deltaTime * m_look;
		updateView = true;
	}

	// Strafe
	if (controller->StrafeLeft())
	{
		m_position -= Demo::GetConfig().CameraSpeed * deltaTime * m_right;
		updateView = true;
	}
	else if (controller->StrafeRight())
	{
		m_position += Demo::GetConfig().CameraSpeed * deltaTime * m_right;
		updateView = true;
	}

	// Pitch
	float pitch = controller->Pitch();
	if (pitch != 0.f)
	{
		Matrix rotationMatrix = Matrix::CreateFromAxisAngle(m_right, pitch);
		m_up = Vector3::TransformNormal(m_up, rotationMatrix);
		m_look = Vector3::TransformNormal(m_look, rotationMatrix);
		updateView = true;
	}

	// Yaw-ish (Rotate about world y-axis)
	float yaw = controller->Yaw();
	if (yaw != 0.f)
	{
		Matrix rotationMatrix = Matrix::CreateRotationY(yaw);
		m_right = Vector3::TransformNormal(m_right, rotationMatrix);
		m_up = Vector3::TransformNormal(m_up, rotationMatrix);
		m_look = Vector3::TransformNormal(m_look, rotationMatrix);
		updateView = true;
	}

	if (updateView)
	{
		UpdateViewTransform();
	}

	if (m_fov != Demo::GetConfig().Fov)
	{
		m_fov = Demo::GetConfig().Fov;
		m_projectionTransform = Demo::Utils::GetReverseZInfinitePerspectiveFovLH(m_fov, Demo::GetAspectRatio(), 1.f);
	}
}

void FView::Reset(const FScene* scene)
{
	if (scene && scene->m_cameras.size() > 0)
	{
		// Use provided camera
		m_position = scene->m_cameras[0].m_viewTransform.Translation();
		m_right = scene->m_cameras[0].m_viewTransform.Right();
		m_up = scene->m_cameras[0].m_viewTransform.Up();
		m_look = scene->m_cameras[0].m_viewTransform.Forward();
		UpdateViewTransform();

		m_projectionTransform = scene->m_cameras[0].m_projectionTransform;
	}
	else
	{
		// Default camera
		m_position = { 0.f, 0.f, -15.f };
		m_right = { 1.f, 0.f, 0.f };
		m_up = { 0.f, 1.f, 0.f };
		m_look = { 0.f, 0.f, 1.f };
		UpdateViewTransform();

		m_fov = Demo::GetConfig().Fov;
		m_projectionTransform = Demo::Utils::GetReverseZInfinitePerspectiveFovLH(m_fov, Demo::GetAspectRatio(), 1.f);
	}
}

void FView::UpdateViewTransform()
{
	m_look.Normalize();
	m_up = m_look.Cross(m_right);
	m_up.Normalize();
	m_right = m_up.Cross(m_look);

	Vector3 translation = Vector3(m_position.Dot(m_right), m_position.Dot(m_up), m_position.Dot(m_look));

	// v = inv(T) * transpose(R)
	m_viewTransform(0, 0) = m_right.x;
	m_viewTransform(1, 0) = m_right.y;
	m_viewTransform(2, 0) = m_right.z;
	m_viewTransform(3, 0) = -translation.x;

	m_viewTransform(0, 1) = m_up.x;
	m_viewTransform(1, 1) = m_up.y;
	m_viewTransform(2, 1) = m_up.z;
	m_viewTransform(3, 1) = -translation.y;

	m_viewTransform(0, 2) = m_look.x;
	m_viewTransform(1, 2) = m_look.y;
	m_viewTransform(2, 2) = m_look.z;
	m_viewTransform(3, 2) = -translation.z;

	m_viewTransform(0, 3) = 0.0f;
	m_viewTransform(1, 3) = 0.0f;
	m_viewTransform(2, 3) = 0.0f;
	m_viewTransform(3, 3) = 1.0f;

	Renderer::ResetPathtraceAccumulation();
}

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Texture Cache
//-----------------------------------------------------------------------------------------------------------------------------------------------

// The returned index is offset to the beginning of the descriptor table range
uint32_t FTextureCache::CacheTexture2D(
	FResourceUploadContext* uploadContext, 
	const std::wstring& name,
	const DXGI_FORMAT format,
	const int width,
	const int height,
	const DirectX::Image* images,
	const size_t imageCount)
{
	auto search = m_cachedTextures.find(name);
	if (search != m_cachedTextures.cend())
	{
		return search->second->m_srvIndex;
	}
	else
	{
		m_cachedTextures[name].reset(RenderBackend12::CreateNewTexture(name, FTexture::Type::Tex2D, FResource::Allocation::Persistent(), format, width, height, imageCount, 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, images, uploadContext));
		return m_cachedTextures[name]->m_srvIndex;
	}
}

uint32_t FTextureCache::CacheEmptyTexture2D(
	const std::wstring& name,
	const DXGI_FORMAT format,
	const int width,
	const int height,
	const size_t mipCount)
{
	m_cachedTextures[name].reset(RenderBackend12::CreateNewTexture(name, FTexture::Type::Tex2D, FResource::Allocation::Persistent(), format, width, height, mipCount, 1, D3D12_RESOURCE_STATE_COPY_DEST));
	return m_cachedTextures[name]->m_srvIndex;
}

FLightProbe FTextureCache::CacheHDRI(const std::wstring& name)
{
	SCOPED_CPU_EVENT("cache_hdri", PIX_COLOR_DEFAULT);

	const std::wstring envmapTextureName = name + L".envmap";
	const std::wstring shTextureName = name + L".shtex";

	auto search0 = m_cachedTextures.find(envmapTextureName);
	auto search1 = m_cachedTextures.find(shTextureName);
	if (search0 != m_cachedTextures.cend() && search1 != m_cachedTextures.cend())
	{
		return FLightProbe{
			(int)search0->second->m_srvIndex,
			(int)search1->second->m_srvIndex,
		};
	}
	else
	{
		// Read HDR sphere map from file
		DirectX::TexMetadata metadata;
		DirectX::ScratchImage scratch;
		AssertIfFailed(DirectX::LoadFromHDRFile(GetFilepathW(name).c_str(), &metadata, scratch));

		// Calculate mips upto 4x4 for block compression
		size_t width = metadata.width, height = metadata.height;
		size_t numMips = RenderUtils12::CalcMipCount(metadata.width, metadata.height, true);

		// Generate mips
		DirectX::ScratchImage mipchain = {};
		AssertIfFailed(DirectX::GenerateMipMaps(*scratch.GetImage(0,0,0), DirectX::TEX_FILTER_LINEAR, numMips, mipchain));

		// Compute CL
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"hdr_preprocess", D3D12_COMMAND_LIST_TYPE_DIRECT);
		FFenceMarker gpuFinishFence = cmdList->GetFence(FCommandList::SyncPoint::GpuFinish);
		FScopedGpuCapture pixCapture{ cmdList };

		// Create the equirectangular source texture
		FResourceUploadContext uploadContext{ mipchain.GetPixelsSize() };
		std::unique_ptr<FTexture> srcHdrTex{ RenderBackend12::CreateNewTexture(
			name, FTexture::Type::Tex2D, FResource::Allocation::Transient(gpuFinishFence), metadata.format, metadata.width, metadata.height, mipchain.GetImageCount(), 1,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, mipchain.GetImages(), &uploadContext) };

		D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
		SCOPED_COMMAND_QUEUE_EVENT(cmdList->m_type, "hdr_preprocess", 0);
		uploadContext.SubmitUploads(cmdList);

		// ---------------------------------------------------------------------------------------------------------
		// Generate environment cubemap
		// ---------------------------------------------------------------------------------------------------------
		const size_t cubemapSize = metadata.height;
		std::unique_ptr<FShaderSurface> texCubeUav{ RenderBackend12::CreateNewShaderSurface(L"src_cubemap", FShaderSurface::Type::UAV, FResource::Allocation::Transient(gpuFinishFence), metadata.format, cubemapSize, cubemapSize, numMips, 1, 6) };
		Renderer::ConvertLatlong2Cubemap(cmdList, srcHdrTex->m_srvIndex, texCubeUav->m_descriptorIndices.UAVs, cubemapSize, numMips);

		// ---------------------------------------------------------------------------------------------------------
		// Prefilter Environment map
		// ---------------------------------------------------------------------------------------------------------
		const size_t filteredEnvmapSize = cubemapSize >> 1;
		const int filteredEnvmapMips = numMips - 1;
		std::unique_ptr<FShaderSurface> texFilteredEnvmapUav{ RenderBackend12::CreateNewShaderSurface(L"filtered_envmap", FShaderSurface::Type::UAV, FResource::Allocation::Transient(gpuFinishFence), metadata.format, filteredEnvmapSize, filteredEnvmapSize, filteredEnvmapMips, 1, 6) };
		texCubeUav->m_resource->Transition(cmdList, texCubeUav->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		Renderer::PrefilterCubemap(cmdList, texCubeUav->m_descriptorIndices.SRV, texFilteredEnvmapUav->m_descriptorIndices.UAVs, filteredEnvmapSize, 0, filteredEnvmapMips);


		// Copy from UAV to destination cubemap texture
		texFilteredEnvmapUav->m_resource->Transition(cmdList, texFilteredEnvmapUav->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE);
		std::unique_ptr<FTexture> filteredEnvmapTex{ RenderBackend12::CreateNewTexture(envmapTextureName, FTexture::Type::TexCube, FResource::Allocation::Persistent(), metadata.format, filteredEnvmapSize, filteredEnvmapSize, filteredEnvmapMips, 6, D3D12_RESOURCE_STATE_COPY_DEST) };
		d3dCmdList->CopyResource(filteredEnvmapTex->m_resource->m_d3dResource, texFilteredEnvmapUav->m_resource->m_d3dResource);
		filteredEnvmapTex->m_resource->Transition(cmdList, filteredEnvmapTex->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		m_cachedTextures[envmapTextureName] = std::move(filteredEnvmapTex);

		// ---------------------------------------------------------------------------------------------------------
		// Project radiance to SH basis
		// ---------------------------------------------------------------------------------------------------------
		constexpr int numCoefficients = 9; 
		constexpr uint32_t baseMipIndex = 0;
		const uint32_t baseMipWidth = metadata.width >> baseMipIndex;
		const uint32_t baseMipHeight = metadata.height >> baseMipIndex;
		const size_t shMips = RenderUtils12::CalcMipCount(baseMipWidth, baseMipHeight, false);
		std::unique_ptr<FShaderSurface> shTexureUav{ RenderBackend12::CreateNewShaderSurface(L"ShProj", FShaderSurface::Type::UAV, FResource::Allocation::Transient(gpuFinishFence), metadata.format, baseMipWidth, baseMipHeight, shMips, 1, numCoefficients) };

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "SH_projection", 0);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"sh_projection_rootsig",
				cmdList,
				FRootSignature::Desc{ L"image-based-lighting/spherical-harmonics/projection.hlsl", L"rootsig", L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({ 
				L"image-based-lighting/spherical-harmonics/projection.hlsl",
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
				uint32_t inputHdriIndex;
				uint32_t outputUavIndex;
				uint32_t hdriWidth;
				uint32_t hdriHeight;
				uint32_t srcMip;
			} rootConstants = { srcHdrTex->m_srvIndex, shTexureUav->m_descriptorIndices.UAVs[0], baseMipWidth, baseMipHeight, baseMipIndex };

			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);

			size_t threadGroupCountX = std::max<size_t>(std::ceil(baseMipWidth / 16), 1);
			size_t threadGroupCountY = std::max<size_t>(std::ceil(baseMipHeight / 16), 1);
			d3dCmdList->Dispatch(threadGroupCountX, threadGroupCountY, 1);
		}

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "SH_integration", 0);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"sh_integration_rootsig",
				cmdList,
				FRootSignature::Desc{ L"image-based-lighting/spherical-harmonics/parallel-reduction.hlsl", L"rootsig", L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

			uint32_t threadGroupSizeX = 8;
			uint32_t threadGroupSizeY = 8;
			uint32_t threadGroupSizeZ = 1;

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({ 
				L"image-based-lighting/spherical-harmonics/parallel-reduction.hlsl", 
				L"cs_main", 
				PrintString(L"THREAD_GROUP_SIZE_X=%u THREAD_GROUP_SIZE_Y=%u THREAD_GROUP_SIZE_Z=%u", threadGroupSizeX, threadGroupSizeY, threadGroupSizeZ),
				L"cs_6_6" });

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = rootsig->m_rootsig;
			psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
			psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			D3DPipelineState_t* pso = RenderBackend12::FetchComputePipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			// Reduction - Each thread will read 4 neighboring texel values and write the result out to the next mip
			for(uint32_t srcMip = 0; srcMip < shMips -1; ++srcMip)
			{
				uint32_t destMip = srcMip + 1;
				uint32_t srcMipWidth = std::max<uint32_t>(baseMipWidth >> srcMip, 1), srcMipHeight = std::max<uint32_t>(baseMipHeight >> srcMip, 1);
				uint32_t destMipWidth = std::max<uint32_t>(baseMipWidth >> destMip, 1), destMipHeight = std::max<uint32_t>(baseMipHeight >> destMip, 1);
				float uvScale[2] = {
					srcMipWidth / (float)destMipWidth,
					srcMipHeight / (float)destMipHeight
				};

				struct CbLayout
				{
					uint32_t srcUavIndex;
					uint32_t destUavIndex;
					uint32_t srcMipIndex;
					uint32_t destMipIndex;
					float uvScale[2];
				};
				
				CbLayout cb{};
				cb.srcUavIndex = shTexureUav->m_descriptorIndices.UAVs[srcMip];
				cb.destUavIndex = shTexureUav->m_descriptorIndices.UAVs[destMip];
				cb.srcMipIndex = srcMip;
				cb.destMipIndex = destMip;
				cb.uvScale[0] = uvScale[0];
				cb.uvScale[1] = uvScale[1];

				SCOPED_COMMAND_LIST_EVENT(cmdList, PrintString("Parallel Reduction (%dx%d)", destMipWidth, destMipHeight).c_str(), 0);

				shTexureUav->m_resource->UavBarrier(cmdList);
				d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(CbLayout) / 4, &cb, 0);

				// Reduce by 2 x 2 on each iteration
				size_t threadGroupCountX = std::max<size_t>(std::ceil(destMipWidth / threadGroupSizeX), 1);
				size_t threadGroupCountY = std::max<size_t>(std::ceil(destMipHeight / threadGroupSizeY), 1);
				d3dCmdList->Dispatch(threadGroupCountX, threadGroupCountY, numCoefficients);
			}
		}

		std::unique_ptr<FShaderSurface> shExportTexureUav{ RenderBackend12::CreateNewShaderSurface(L"ShExport", FShaderSurface::Type::UAV, FResource::Allocation::Transient(gpuFinishFence), metadata.format, numCoefficients, 1) };

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "SH_export", 0);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"sh_export_rootsig",
				cmdList,
				FRootSignature::Desc { L"image-based-lighting/spherical-harmonics/export.hlsl", L"rootsig", L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({ 
				L"image-based-lighting/spherical-harmonics/export.hlsl", 
				L"cs_main", 
				L"",
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
				uint32_t srcUavIndex;
				uint32_t destUavIndex;
			} rootConstants = {
					shTexureUav->m_descriptorIndices.UAVs[shMips - 1],
					shExportTexureUav->m_descriptorIndices.UAVs[0]
			};

			uint32_t mipUavIndex = shTexureUav->m_descriptorIndices.UAVs[shMips - 1];
			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);
			d3dCmdList->Dispatch(1, 1, 1);
		}

		// Copy from UAV to destination texture
		shExportTexureUav->m_resource->Transition(cmdList, shExportTexureUav->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE);
		std::unique_ptr<FTexture> shTex{ RenderBackend12::CreateNewTexture(shTextureName, FTexture::Type::Tex2D, FResource::Allocation::Persistent(), metadata.format, numCoefficients, 1, 1, 1, D3D12_RESOURCE_STATE_COPY_DEST) };
		d3dCmdList->CopyResource(shTex->m_resource->m_d3dResource, shExportTexureUav->m_resource->m_d3dResource);
		shTex->m_resource->Transition(cmdList, shTex->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		m_cachedTextures[shTextureName] = std::move(shTex);

		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

		return FLightProbe{
			(int)m_cachedTextures[envmapTextureName]->m_srvIndex,
			(int)m_cachedTextures[shTextureName]->m_srvIndex,
		};
	}
}

void FTextureCache::Clear()
{
	m_cachedTextures.clear();
}

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Sampler Cache
//-----------------------------------------------------------------------------------------------------------------------------------------------

uint32_t FSamplerCache::CacheSampler(const tinygltf::Sampler& s)
{
	auto search = m_cachedSamplers.find(s);
	if (search != m_cachedSamplers.cend())
	{
		return search->second;
	}
	else
	{
		auto AddressModeConversion = [](int wrapMode) -> D3D12_TEXTURE_ADDRESS_MODE
		{
			switch (wrapMode)
			{
			case TINYGLTF_TEXTURE_WRAP_REPEAT: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
			default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			}
		};

		// Just go off min filter to reduce number of combinations
		auto FilterModeConversion = [](int filterMode)
		{
			switch (filterMode)
			{
			case TINYGLTF_TEXTURE_FILTER_NEAREST: return D3D12_FILTER_MIN_MAG_MIP_POINT;
			case TINYGLTF_TEXTURE_FILTER_LINEAR: return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST: return D3D12_FILTER_MIN_MAG_MIP_POINT;
			case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST: return D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
			case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR: return D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
			case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR: return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			default: return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			}
		};

		const D3D12_FILTER filter = FilterModeConversion(s.minFilter);
		const D3D12_TEXTURE_ADDRESS_MODE addressU = AddressModeConversion(s.wrapS);
		const D3D12_TEXTURE_ADDRESS_MODE addressV = AddressModeConversion(s.wrapT);
		const D3D12_TEXTURE_ADDRESS_MODE addressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

		m_cachedSamplers[s] = RenderBackend12::CreateSampler(filter, addressU, addressV, addressW);
		return m_cachedSamplers[s];
	}
}

void FSamplerCache::Clear()
{
	m_cachedSamplers.clear();
}
