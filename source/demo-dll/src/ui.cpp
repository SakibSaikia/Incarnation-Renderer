#include <ui.h>
#include <demo.h>
#include <profiling.h>
#include <Renderer.h>
#include <backends/imgui_impl_win32.h>

namespace
{
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
}

void UI::Initialize(const HWND& windowHandle)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init(windowHandle);
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

	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
	const ImGuiViewport* viewport = ImGui::GetMainViewport();

	bool bResetPathtracelAccumulation = false;
	bool bUpdateSkylight = false;

	ImGui::SetNextWindowPos(ImVec2(0.8f * viewport->WorkSize.x, 0), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(0.2f * viewport->WorkSize.x, viewport->WorkSize.y), ImGuiCond_Always);

	ImGui::Begin("Options", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
	{
		ImGui::Checkbox("TAA", &settings->EnableTAA);
		ImGui::SameLine();
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
		EditCondition(settings->PathTrace,
			[&]()
			{
				if (ImGui::CollapsingHeader("Path Tracing"))
				{
					bResetPathtracelAccumulation |= ImGui::SliderInt("Max. Sample Count", (int*)&settings->MaxSampleCount, 1, 1024);
					bResetPathtracelAccumulation |= ImGui::SliderFloat("Camera Aperture", &settings->Pathtracing_CameraAperture, 0.f, 0.1f);
					bResetPathtracelAccumulation |= ImGui::SliderFloat("Camera Focal Length", &settings->Pathtracing_CameraFocalLength, 1.f, 15.f);
				}
			});

		// -----------------------------------------------------------------------------------------------------------------------------------------

		if (ImGui::CollapsingHeader("Scene"))
		{
			if (ImGui::BeginTabBar("ScaneTabs", ImGuiTabBarFlags_None))
			{
				if (ImGui::BeginTabItem("Environment/Sky"))
				{
					int currentSkyMode = settings->EnvSkyMode;
					ImGui::RadioButton("Environment Map", &settings->EnvSkyMode, (int)EnvSkyMode::Environmentmap);
					ImGui::SameLine();
					ImGui::RadioButton("Dynamic Sky", &settings->EnvSkyMode, (int)EnvSkyMode::DynamicSky);
					bResetPathtracelAccumulation |= (settings->EnvSkyMode != currentSkyMode);
					bUpdateSkylight |= (currentSkyMode == (int)EnvSkyMode::Environmentmap) && (settings->EnvSkyMode == (int)EnvSkyMode::DynamicSky);


					// ----------------------------------------------------------
					// Environment map
					EditCondition(settings->EnvSkyMode == (int)EnvSkyMode::Environmentmap,
						[&]()
						{
							int curHdriIndex = std::find(hdris.begin(), hdris.end(), settings->EnvironmentFilename) - hdris.begin();
							comboLabel = ws2s(hdris[curHdriIndex]);
							if (ImGui::BeginCombo("Environment map", comboLabel.c_str(), ImGuiComboFlags_None))
							{
								for (int n = 0; n < hdris.size(); n++)
								{
									const bool bSelected = (curHdriIndex == n);
									if (ImGui::Selectable(ws2s(hdris[n]).c_str(), bSelected))
									{
										curHdriIndex = n;
										settings->EnvironmentFilename = hdris[n];
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

					// ----------------------------------------------------------
					// Dynamic Sky
					EditCondition(settings->EnvSkyMode == (int)EnvSkyMode::DynamicSky,
						[&]()
						{
							if (ImGui::SliderFloat("Turbidity", &settings->Turbidity, 2.f, 10.f))
							{
								bResetPathtracelAccumulation = true;
							}
						});

					ImGui::EndTabItem();
				}

				if (ImGui::BeginTabItem("Lights"))
				{
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
	}
	ImGui::End();

	ImGui::Begin("Render Stats");
	{
		ImGui::Text("%.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

		FRenderStats stats = Renderer::GetRenderStats();
		ImGui::Text("Primitive Culling		%.2f%%", 100.f * stats.m_culledPrimitives / (float)scene->m_primitiveCount);

		const size_t numLights = scene->m_sceneLights.GetCount();
		EditCondition(numLights > 0,
			[&]()
			{
				ImGui::Text("Light Culling			%.2f%%", numLights == 0 ? 0.f : 100.f * stats.m_culledLights / (float)(numLights * settings->LightClusterDimX * settings->LightClusterDimY * settings->LightClusterDimZ));
			});
	}
	ImGui::End();

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