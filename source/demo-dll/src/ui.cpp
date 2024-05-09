#include <ui.h>
#include <imgui_internal.h>
#include <demo.h>
#include <profiling.h>
#include <Renderer.h>
#include <backends/imgui_impl_win32.h>
#include <dxcapi.h>

namespace
{
	ImVec2 operator-(ImVec2 lhs, ImVec2 rhs)
	{
		return ImVec2{ lhs.x - rhs.x, lhs.y - rhs.y };
	}

	void LoadFontTexture()
	{
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"imgui", D3D12_COMMAND_LIST_TYPE_DIRECT);
		FResourceUploadContext uploader{ 32 * 1024 * 1024 };

		uint8_t* pixels;
		int bpp, width, height;
		ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height, &bpp);

		DirectX::Image img;
		img.width = width;
		img.height = height;
		img.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		img.pixels = pixels;

		AssertIfFailed(DirectX::ComputePitch(DXGI_FORMAT_R8G8B8A8_UNORM, img.width, img.height, img.rowPitch, img.slicePitch));

		uint32_t fontSrvIndex = Demo::GetTextureCache().CacheTexture2D(&uploader, L"imgui_fonts", DXGI_FORMAT_R8G8B8A8_UNORM, img.width, img.height, &img, 1);
		ImGui::GetIO().Fonts->TexID = (ImTextureID)fontSrvIndex;
		uploader.SubmitUploads(cmdList);
		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
	}

	// Prefiltered environment radiance
	ImTextureID RenderEnvironmentRadiancePreview(const FScene* scene, const FConfig* settings, ImVec2 texSize)
	{
		FFenceMarker endOfFrameFence = RenderBackend12::GetCurrentFrameFence();
		std::unique_ptr<FShaderSurface> targetSurface{ RenderBackend12::CreateNewShaderSurface({
			.name = L"envmap_preview",
			.type = FShaderSurface::Type::UAV,
			.alloc = FResource::Allocation::Transient(endOfFrameFence),
			.format = DXGI_FORMAT_R8G8B8A8_UNORM,
			.width = (size_t)texSize.x,
			.height = (size_t)texSize.y })};

		// Render preview envmap
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"envmap_preview", D3D12_COMMAND_LIST_TYPE_DIRECT);
		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "envmap_preview", 0);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"envmap_preview_rootsig",
				cmdList,
				FRootSignature::Desc{ L"ui/envmap-preview.hlsl", L"rootsig", L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({
				L"ui/envmap-preview.hlsl",
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

			struct FConstants
			{
				uint32_t m_texSize[2];
				uint32_t m_envmapTextureIndex;
				uint32_t m_uavIndex;
				float m_skyBrightness;
				float m_exposure;
			};

			FConstants cb{};
			cb.m_texSize[0] = texSize.x;
			cb.m_texSize[1] = texSize.y;
			cb.m_envmapTextureIndex = scene->m_skylight.m_envmapTextureIndex;
			cb.m_uavIndex = targetSurface->m_descriptorIndices.UAVs[0];
			cb.m_skyBrightness = settings->SkyBrightness;
			cb.m_exposure = settings->Exposure;
			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(FConstants) / 4, &cb, 0);

			const size_t threadGroupCountX = GetDispatchSize(texSize.x, 16);
			const size_t threadGroupCountY = GetDispatchSize(texSize.y, 16);
			d3dCmdList->Dispatch(threadGroupCountX, threadGroupCountY, 1);
		}

		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

		return (ImTextureID)targetSurface->m_descriptorIndices.SRV;
	}

	// SH convolved with cosine lobe
	ImTextureID RenderEnvironmentIrradiancePreview(const FScene* scene, const FConfig* settings, ImVec2 texSize)
	{
		FFenceMarker endOfFrameFence = RenderBackend12::GetCurrentFrameFence();
		std::unique_ptr<FShaderSurface> targetSurface{ RenderBackend12::CreateNewShaderSurface({
			.name = L"sh_preview",
			.type = FShaderSurface::Type::UAV,
			.alloc = FResource::Allocation::Transient(endOfFrameFence),
			.format = DXGI_FORMAT_R8G8B8A8_UNORM,
			.width = (size_t)texSize.x,
			.height = (size_t)texSize.y })};

		// Render preview envmap
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"sh_preview", D3D12_COMMAND_LIST_TYPE_DIRECT);
		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "sh_preview", 0);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"sh_preview_rootsig",
				cmdList,
				FRootSignature::Desc{ L"ui/sh-preview.hlsl", L"rootsig", L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({
				L"ui/sh-preview.hlsl",
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

			struct FConstants
			{
				uint32_t m_texSize[2];
				uint32_t m_shTextureIndex;
				uint32_t m_uavIndex;
				float m_skyBrightness;
				float m_exposure;
			};

			FConstants cb{};
			cb.m_texSize[0] = texSize.x;
			cb.m_texSize[1] = texSize.y;
			cb.m_shTextureIndex = scene->m_skylight.m_shTextureIndex;
			cb.m_uavIndex = targetSurface->m_descriptorIndices.UAVs[0];
			cb.m_skyBrightness = settings->SkyBrightness;
			cb.m_exposure = settings->Exposure;
			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(FConstants) / 4, &cb, 0);

			const size_t threadGroupCountX = GetDispatchSize(texSize.x, 16);
			const size_t threadGroupCountY = GetDispatchSize(texSize.y, 16);
			d3dCmdList->Dispatch(threadGroupCountX, threadGroupCountY, 1);
		}

		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

		return (ImTextureID)targetSurface->m_descriptorIndices.SRV;
	}
}

namespace ImGuiExt
{
	void TextCentered(const char* text)
	{
		auto windowWidth = ImGui::GetWindowSize().x;
		auto textWidth = ImGui::CalcTextSize(text).x;

		ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
		ImGui::Text(text);
	}

	void EditCondition(bool predicate, std::function<void()> code)
	{
		if (!predicate)
		{
			ImGui::BeginDisabled();
		}

		code();

		if (!predicate)
		{
			ImGui::EndDisabled();
		}
	}

	void ImageZoom(ImTextureID texture, ImVec2 size)
	{
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImGui::Image(texture, size);
		if (ImGui::IsItemHovered())
		{
			ImGuiIO& io = ImGui::GetIO();

			ImGui::BeginTooltip();
			float region_sz = 32.0f;
			float region_x = io.MousePos.x - pos.x - region_sz * 0.5f;
			float region_y = io.MousePos.y - pos.y - region_sz * 0.5f;
			float zoom = 4.0f;

			if (region_x < 0.0f)
			{
				region_x = 0.0f;
			}
			else if (region_x > size.x - region_sz)
			{
				region_x = size.x - region_sz;
			}

			if (region_y < 0.0f)
			{
				region_y = 0.0f;
			}
			else if (region_y > size.y - region_sz)
			{
				region_y = size.y - region_sz;
			}

			ImVec2 uv0 = ImVec2((region_x) / size.x, (region_y) / size.y);
			ImVec2 uv1 = ImVec2((region_x + region_sz) / size.x, (region_y + region_sz) / size.y);
			ImGui::Image(texture, ImVec2(region_sz * zoom, region_sz * zoom), uv0, uv1);
			ImGui::EndTooltip();
		}
	}
}

void UI::Initialize(const HWND& windowHandle)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init(windowHandle);
	LoadFontTexture();
}

void UI::Teardown()
{
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}

bool UI::HasFocus()
{
	ImGuiIO& io = ImGui::GetIO();
	return io.WantCaptureMouse;
}

void UI::Update(Demo::App* demoApp, const float deltaTime)
{
	if (Renderer::Status::IsPaused())
		return;

	FConfig* settings = &demoApp->m_config;
	FScene* scene = &demoApp->m_scene;
	FView* view = &demoApp->m_view;
	const std::vector<std::wstring>& models = demoApp->m_modelList;
	const std::vector<std::wstring>& hdris = demoApp->m_hdriList;;

	SCOPED_CPU_EVENT("ui_update", PIX_COLOR_DEFAULT);
	SCOPED_COMMAND_QUEUE_EVENT(D3D12_COMMAND_LIST_TYPE_DIRECT, "ui", 0);

	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
	const ImGuiViewport* viewport = ImGui::GetMainViewport();

	bool bResetPathtracelAccumulation = false;
	bool bUpdateSkylight = false;

	// Show options menu once the scene has finished loading
	if (FScene::s_loadProgress == 1.f)
	{
		const ImVec2 optionsWindowSize = { 0.2f * viewport->WorkSize.x, viewport->WorkSize.y };
		ImGui::SetNextWindowPos(viewport->WorkSize - optionsWindowSize, ImGuiCond_Always);
		ImGui::SetNextWindowSize(optionsWindowSize, ImGuiCond_Always);

		ImGui::Begin("Options", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
		ImGui::Checkbox("Pathtracing", &settings->PathTrace);

		// --------------------------------------------------------------------------------------------------------------------------------------------
		// Model
		static int curModelIndex = std::find(demoApp->m_modelList.begin(), demoApp->m_modelList.end(), settings->ModelFilename) - demoApp->m_modelList.begin();
		std::string comboLabel = ws2s(demoApp->m_modelList[curModelIndex]);
		if (ImGui::BeginCombo("Model", comboLabel.c_str(), ImGuiComboFlags_None))
		{
			for (int n = 0; n < models.size(); n++)
			{
				const bool bSelected = (curModelIndex == n);
				if (ImGui::Selectable(ws2s(models[n]).c_str(), bSelected))
				{
					curModelIndex = n;
					settings->ModelFilename = models[n];
				}

				if (bSelected)
				{
					ImGui::SetItemDefaultFocus();
				}
			}

			ImGui::EndCombo();
		}

		// --------------------------------------------------------------------------------------------------------------------------------------------
		// Pathtracing Options
		ImGuiExt::EditCondition(settings->PathTrace,
			[&]()
			{
				if (ImGui::CollapsingHeader("Path Tracing"))
				{
					bResetPathtracelAccumulation |= ImGui::SliderInt("Max. Sample Count", (int*)&settings->MaxSampleCount, 1, 1024);
					bResetPathtracelAccumulation |= ImGui::SliderFloat("Camera Aperture", &settings->Pathtracing_CameraAperture, 0.f, 0.1f);
					bResetPathtracelAccumulation |= ImGui::SliderFloat("Camera Focal Length", &settings->Pathtracing_CameraFocalLength, 1.f, 15.f);
				}
			});

		// --------------------------------------------------------------------------------------------------------------------------------------------
		// Raster Options
		ImGuiExt::EditCondition(!settings->PathTrace,
			[&]()
			{
				if (ImGui::CollapsingHeader("Raster"))
				{
					ImGui::Checkbox("TAA", &settings->EnableTAA);
					ImGui::SameLine();
					ImGui::Checkbox("HBAO", &settings->EnableHBAO);
					ImGui::SameLine();
					ImGuiExt::EditCondition(settings->EnableHBAO, [&]() { ImGui::Checkbox("Bent Normals", &settings->UseBentNormals); });
					ImGui::SameLine();
					ImGui::Checkbox("Meshlets", &settings->UseMeshlets);
					ImGui::Checkbox("Forward Lighting", &settings->ForwardLighting);
					ImGui::SameLine();
					ImGui::Checkbox("Frustum Culling", &settings->FrustumCulling);
				}
			});

		// -----------------------------------------------------------------------------------------------------------------------------------------

		if (ImGui::CollapsingHeader("Scene"))
		{
			if (ImGui::BeginTabBar("SceneTabs", ImGuiTabBarFlags_None))
			{
				if (ImGui::BeginTabItem("Geo"))
				{
					int meshCount = scene->m_sceneMeshes.GetCount();
					if (ImGui::Button("Hide All"))
					{
						for (int i = 0; i < meshCount; ++i)
						{
							scene->m_sceneMeshes.m_visibleList[i] = 0;
							bResetPathtracelAccumulation |= true;
						}
					}

					ImGui::SameLine();

					if (ImGui::Button("Show All"))
					{
						for (int i = 0; i < meshCount; ++i)
						{
							scene->m_sceneMeshes.m_visibleList[i] = 1;
							bResetPathtracelAccumulation |= true;
						}
					}

					for (int i = 0; i < meshCount; ++i)
					{
						bool bVisible = scene->m_sceneMeshes.m_visibleList[i];
						bResetPathtracelAccumulation |= ImGui::Selectable(scene->m_sceneMeshes.m_entityNames[i].c_str(), &bVisible);
						scene->m_sceneMeshes.m_visibleList[i] = bVisible;
					}
					ImGui::EndTabItem();
				}

				if (ImGui::BeginTabItem("Environment/Sky"))
				{
					int currentSkyMode = settings->EnvSkyMode;
					ImGui::RadioButton("HDRI", &settings->EnvSkyMode, (int)EnvSkyMode::HDRI);
					ImGui::SameLine();
					ImGui::RadioButton("Dynamic Sky", &settings->EnvSkyMode, (int)EnvSkyMode::DynamicSky);
					bResetPathtracelAccumulation |= (settings->EnvSkyMode != currentSkyMode);
					bUpdateSkylight |= (currentSkyMode == (int)EnvSkyMode::HDRI) && (settings->EnvSkyMode == (int)EnvSkyMode::DynamicSky);

					if (bUpdateSkylight)
					{
						// Clear the envmap name so that it will be reloaded when the envsky mode changes
						scene->m_hdriFilename = {};
					}


					// ----------------------------------------------------------
					// HDRI
					ImGuiExt::EditCondition(settings->EnvSkyMode == (int)EnvSkyMode::HDRI,
						[&]()
						{
							int curHdriIndex = std::find(hdris.begin(), hdris.end(), settings->HDRIFilename) - hdris.begin();
							comboLabel = ws2s(hdris[curHdriIndex]);
							if (ImGui::BeginCombo("HDRI Filename", comboLabel.c_str(), ImGuiComboFlags_None))
							{
								for (int n = 0; n < hdris.size(); n++)
								{
									const bool bSelected = (curHdriIndex == n);
									if (ImGui::Selectable(ws2s(hdris[n]).c_str(), bSelected))
									{
										curHdriIndex = n;
										settings->HDRIFilename = hdris[n];
										bResetPathtracelAccumulation = true;
									}

									if (bSelected)
									{
										ImGui::SetItemDefaultFocus();
									}
								}

								ImGui::EndCombo();
							}
						});

					if (ImGui::SliderFloat("Sky Brightness", &settings->SkyBrightness, 1000.f, 25000.f))
					{
						bResetPathtracelAccumulation = true;
					}

					// ----------------------------------------------------------
					// Dynamic Sky
					ImGuiExt::EditCondition(settings->EnvSkyMode == (int)EnvSkyMode::DynamicSky,
						[&]()
						{
							if (ImGui::SliderFloat("Turbidity", &settings->Turbidity, 2.f, 10.f))
							{
								bResetPathtracelAccumulation = true;
							}
						});

					ImVec2 previewTexSize;
					previewTexSize.x = 0.95f * optionsWindowSize.x;
					previewTexSize.y = 0.5f * previewTexSize.x;
					ImTextureID previewEnvmapTex = RenderEnvironmentRadiancePreview(scene, settings, previewTexSize);
					ImTextureID previewShTex = RenderEnvironmentIrradiancePreview(scene, settings, previewTexSize);
					ImGuiExt::ImageZoom(previewEnvmapTex, previewTexSize);
					ImGuiExt::TextCentered("Radiance");
					ImGuiExt::ImageZoom(previewShTex, previewTexSize);
					ImGuiExt::TextCentered("Irradiance");

					ImGui::EndTabItem();
				}

				if (ImGui::BeginTabItem("Lights"))
				{
					ImGui::Checkbox("Enable Skylight", &settings->EnableSkyLighting);

					int lightCount = scene->m_sceneLights.GetCount();
					if (lightCount > 0)
					{
						for (int i = 0; i < lightCount; ++i)
						{
							// Add indent
							ImGui::TreePush("Lights");

							int lightIndex = scene->m_sceneLights.m_entityList[i];
							const std::string& lightName = scene->m_sceneLights.m_entityNames[i];

							FLight& light = scene->m_globalLightList[lightIndex];
							if (ImGui::CollapsingHeader(lightName.c_str()))
							{
								switch (light.m_type)
								{
								case Light::Directional:
									ImGui::LabelText("Type", "Directional Light");
									break;
								case Light::Point:
									ImGui::LabelText("Type", "Point Light");
									break;
								case Light::Spot:
									ImGui::LabelText("Type", "Spot Light");
									break;
								}

								float color[4] = { light.m_color.x, light.m_color.y, light.m_color.z, 1.f };
								if (ImGui::ColorEdit4("Color", (float*)&color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoAlpha))
								{
									light.m_color = Vector3(color);
									bResetPathtracelAccumulation = true;
								}

								if (light.m_type != Light::Directional)
								{
									bResetPathtracelAccumulation |= ImGui::SliderFloat("Intensity (cd)", &light.m_intensity, 0.f, 10000.f);
									bResetPathtracelAccumulation |= ImGui::SliderFloat("Range", &light.m_range, 0.f, 500.f);
								}
								else
								{
									bResetPathtracelAccumulation |= ImGui::SliderFloat("Intensity (lux)", &light.m_intensity, 0.f, 10000.f);
								}

								if (light.m_type == Light::Spot)
								{
									bResetPathtracelAccumulation |= ImGui::SliderFloat("Inner Cone Angle (rad)", &light.m_spotAngles.x, 0.f, 3.14159f);
									bResetPathtracelAccumulation |= ImGui::SliderFloat("Outer Cone Angle (rad)", &light.m_spotAngles.y, 0.f, 3.14159f);
								}
							}

							// Remove indent
							ImGui::TreePop();
						}
					}
					ImGui::EndTabItem();
				}

				ImGui::EndTabBar();
			}
		}

		// --------------------------------------------------------------------------------------------------------------------------------------------

		if (ImGui::CollapsingHeader("Camera"))
		{
			ImGui::SliderFloat("Speed", &settings->CameraSpeed, 1.0f, 20.0f);

			static float fovDeg = DirectX::XMConvertToDegrees(settings->Fov);
			ImGui::SliderFloat("FOV", &fovDeg, 0.0f, 140.0f);
			settings->Fov = DirectX::XMConvertToRadians(fovDeg);

			ImGui::SliderFloat("Exposure", &settings->Exposure, 1.0f, 20.0f);

			if (ImGui::Button("Reset"))
			{
				view->Reset(scene);
			}
		}

		// --------------------------------------------------------------------------------------------------------------------------------------------

		if (ImGui::CollapsingHeader("Time of Day"))
		{
			bool bDirty = false;
			bDirty |= ImGui::Checkbox("Enable", &settings->ToD_Enable);
			bDirty |= ImGui::SliderFloat("Time (hours)", &settings->ToD_DecimalHours, 0.f, 24.f);
			bDirty |= ImGui::SliderInt("Julian Date", &settings->ToD_JulianDate, 1, 365);
			bDirty |= ImGui::SliderFloat("Latitude", &settings->ToD_Latitude, -90.f, 90.f);

			if (bDirty)
			{
				bResetPathtracelAccumulation = true;
				scene->UpdateSunDirection();

				if (settings->EnvSkyMode == (int)EnvSkyMode::DynamicSky)
				{
					bUpdateSkylight = true;
				}
			}
		}

		// --------------------------------------------------------------------------------------------------------------------------------------------

		if (ImGui::CollapsingHeader("Debug"))
		{
			ImGui::Checkbox("Freeze Culling", &settings->FreezeCulling);

			int currentViewMode = settings->Viewmode;
			if (ImGui::TreeNode("View Modes"))
			{
				ImGui::RadioButton("Normal", &settings->Viewmode, (int)Viewmode::Normal);
				ImGui::RadioButton("Nan Check", &settings->Viewmode, (int)Viewmode::NanCheck);
				ImGui::RadioButton("Lighting Only", &settings->Viewmode, (int)Viewmode::LightingOnly);
				ImGui::RadioButton("Roughness", &settings->Viewmode, (int)Viewmode::Roughness);
				ImGui::RadioButton("Metallic", &settings->Viewmode, (int)Viewmode::Metallic);
				ImGui::RadioButton("Base Color", &settings->Viewmode, (int)Viewmode::BaseColor);
				ImGui::RadioButton("Normalmap", &settings->Viewmode, (int)Viewmode::Normalmap);
				ImGui::RadioButton("Emissive", &settings->Viewmode, (int)Viewmode::Emissive);
				ImGui::RadioButton("Reflections", &settings->Viewmode, (int)Viewmode::Reflections);
				ImGui::RadioButton("Ambient Occlusion", &settings->Viewmode, (int)Viewmode::AmbientOcclusion);
				ImGui::RadioButton("Bent Normals", &settings->Viewmode, (int)Viewmode::BentNormals);
				ImGui::RadioButton("Object IDs", &settings->Viewmode, (int)Viewmode::ObjectIds);
				ImGui::RadioButton("Triangle IDs", &settings->Viewmode, (int)Viewmode::TriangleIds);
				ImGui::RadioButton("Light Cluster Slices", &settings->Viewmode, (int)Viewmode::LightClusterSlices);
				ImGui::TreePop();
			}

			bResetPathtracelAccumulation |= (settings->Viewmode != currentViewMode);

			if (ImGui::TreeNode("Light Components"))
			{
				ImGui::Checkbox("Direct Lighting", &settings->EnableDirectLighting);
				ImGui::Checkbox("Diffuse IBL", &settings->EnableDiffuseIBL);
				ImGui::Checkbox("Specular IBL", &settings->EnableSpecularIBL);
				ImGui::TreePop();
			}

			if (ImGui::TreeNode("Debug Rendering"))
			{
				ImGui::Checkbox("Light Bounds", &settings->ShowLightBounds);
				ImGui::TreePop();
			}
		}
		ImGui::End();
	}

	// Show render stats once the scene has finished loading
	if (FScene::s_loadProgress == 1.f)
	{
		ImGui::Begin("Render Stats");
		ImGui::Text("%.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

		FRenderStats stats = Renderer::GetRenderStats();
		ImGui::Text("Primitive Culling		%.2f%%", 100.f * stats.m_culledPrimitives / (float)scene->m_primitiveCount);

		const size_t numLights = scene->m_sceneLights.GetCount();
		ImGuiExt::EditCondition(numLights > 0,
			[&]()
			{
				ImGui::Text("Light Culling			%.2f%%", numLights == 0 ? 0.f : 100.f * stats.m_culledLights / (float)(numLights * settings->LightClusterDimX * settings->LightClusterDimY * settings->LightClusterDimZ));
			});
		ImGui::End();
	}

	// Show progress bar when scene is being loaded
	if (FScene::s_loadProgress != 1.f)
	{
		ImGui::SetNextWindowPos(ImVec2(0.2f * viewport->WorkSize.x, 0.8f * viewport->WorkSize.y), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(0.6f * viewport->WorkSize.x, 20), ImGuiCond_Always);

		ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
		ImGui::Begin("Load Progress", nullptr, flags);
		ImGui::ProgressBar(FScene::s_loadProgress, ImVec2(-1.f, 0.0f));
		ImGui::End();
	}

	ImGui::PopStyleVar();

	ImGui::EndFrame();
	ImGui::Render();

	if (bResetPathtracelAccumulation)
	{
		Renderer::ResetPathtraceAccumulation();
	}

	if (bUpdateSkylight)
	{
		scene->UpdateDynamicSky(true);
	}
}