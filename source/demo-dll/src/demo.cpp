#include <demo.h>
#include <profiling.h>
#include <backend-d3d12.h>
#include <shadercompiler.h>
#include <renderer.h>
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <sstream>
#include <mesh-utils.h>
#include <gpu-shared-types.h>
#include <concurrent_unordered_map.h>
#include <ppltasks.h>
#include <ppl.h>

namespace
{
	Matrix GetReverseZInfinitePerspectiveFovLH(float fov, float r, float n)
	{
		return Matrix{
			1.f / (r * tan(fov / 2.f)),	0.f,					0.f,	0.f,
			0.f,						1.f / tan(fov / 2.f),	0.f,	0.f,
			0.f,						0.f,					0.f,	1.f,
			0.f,						0.f,					n,		0.f
		};
	}
}

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Texture Cache
//-----------------------------------------------------------------------------------------------------------------------------------------------

struct FTextureCache
{
	uint32_t CacheTexture2D(
		FResourceUploadContext* uploadContext,
		const std::wstring& name,
		const DXGI_FORMAT format,
		const int width,
		const int height,
		const DirectX::Image* images,
		const size_t imageCount);

	uint32_t CacheEmptyTexture2D(
		const std::wstring& name,
		const DXGI_FORMAT format,
		const int width,
		const int height,
		const size_t mipCount);

	FLightProbe CacheHDRI(const std::wstring& name);

	void Clear();

	concurrency::concurrent_unordered_map<std::wstring, std::unique_ptr<FTexture>> m_cachedTextures;
};

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Sampler Cache
//-----------------------------------------------------------------------------------------------------------------------------------------------

template<>
struct std::hash<tinygltf::Sampler>
{
	std::size_t operator()(const tinygltf::Sampler& key) const
	{
		uint64_t seed1{}, seed2{};
		spookyhash_context context;
		spookyhash_context_init(&context, seed1, seed2);
		spookyhash_update(&context, &key.minFilter, sizeof(key.minFilter));
		spookyhash_update(&context, &key.magFilter, sizeof(key.magFilter));
		spookyhash_update(&context, &key.wrapS, sizeof(key.wrapS));
		spookyhash_update(&context, &key.wrapT, sizeof(key.wrapT));
		spookyhash_update(&context, &key.wrapR, sizeof(key.wrapR));
		spookyhash_final(&context, &seed1, &seed2);

		return seed1 ^ (seed2 << 1);
	}
};

struct FSamplerCache
{
	uint32_t CacheSampler(const tinygltf::Sampler& s);
	void Clear();

	concurrency::concurrent_unordered_map<tinygltf::Sampler, uint32_t> m_cachedSamplers;
};

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Controller
//-----------------------------------------------------------------------------------------------------------------------------------------------

struct FController
{
	void MouseMove(const WPARAM buttonState, const POINT position)
	{
		m_mouseButtonState = buttonState;
		m_mouseCurrentPosition = position;
	}

	bool KeyPress(int key) const
	{
		return (GetAsyncKeyState(key) & 0x8000) != 0;
	}

	bool MouseLeftButtonPressed() const
	{
		return m_mouseButtonState == MK_LBUTTON;
	}

	bool MouseRightButtonPressed() const
	{
		return m_mouseButtonState == MK_RBUTTON;
	}

	bool MoveForward() const
	{
		return MouseLeftButtonPressed() && KeyPress('W');
	}

	bool MoveBack() const
	{
		return MouseLeftButtonPressed() && KeyPress('S');
	}

	bool StrafeLeft() const
	{
		return MouseLeftButtonPressed() && KeyPress('A');
	}

	bool StrafeRight() const
	{
		return MouseLeftButtonPressed() && KeyPress('D');
	}

	float Pitch() const
	{
		return MouseLeftButtonPressed() ? DirectX::XMConvertToRadians((float)m_mouseMovement.y) : 0.f;
	}

	float Yaw() const
	{
		return MouseLeftButtonPressed() ? DirectX::XMConvertToRadians((float)m_mouseMovement.x) : 0.f;
	}

	float RotateSceneX() const
	{
		return MouseRightButtonPressed() ? DirectX::XMConvertToRadians((float)m_mouseMovement.x) : 0.f;
	}

	float RotateSceneY() const
	{
		return MouseRightButtonPressed() ? DirectX::XMConvertToRadians((float)m_mouseMovement.y) : 0.f;
	}

	void Tick(const float deltaTime)
	{
		m_mouseMovement = { m_mouseCurrentPosition.x - m_mouseLastPosition.x, m_mouseCurrentPosition.y - m_mouseLastPosition.y };
		m_mouseLastPosition = m_mouseCurrentPosition;
	}

	WPARAM m_mouseButtonState = {};
	POINT m_mouseCurrentPosition = {0,0};
	POINT m_mouseLastPosition = {0,0};
	POINT m_mouseMovement = { 0,0 };
};

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Demo
//-----------------------------------------------------------------------------------------------------------------------------------------------

namespace Demo
{
	FConfig s_globalConfig;
	FScene s_scene;
	FView s_view;
	FView s_cullingView;
	FController s_controller;
	FTextureCache s_textureCache;
	FSamplerCache s_samplerCache;
	FDebugDraw s_debugDraw;
	float s_aspectRatio;
	bool s_suspendRendering = true;

	std::vector<std::wstring> s_modelList;
	std::vector<std::wstring> s_hdriList;

	FRenderState GetRenderState()
	{
		FRenderState s;
		s.m_config = s_globalConfig;
		s.m_suspendRendering = s_suspendRendering;
		s.m_scene = &s_scene;
		s.m_view = s_view;
		s.m_cullingView = s_cullingView;
		s.m_mouseX = s_controller.m_mouseCurrentPosition.x;
		s.m_mouseY = s_controller.m_mouseCurrentPosition.y;
		return s;
	}

	bool IsRenderingSuspended()
	{
		return s_suspendRendering;
	}

	FScene* GetScene()
	{
		return &s_scene;
	}

	const FView* GetView()
	{
		return &s_view;
	}

	FDebugDraw* GetDebugRenderer()
	{
		return &s_debugDraw;
	}

	void UpdateUI(float deltaTime);
}

class ScopedPauseRendering
{
public:
	ScopedPauseRendering()
	{
		Demo::s_suspendRendering = true;
		RenderBackend12::FlushGPU();
	}

	~ScopedPauseRendering()
	{
		RenderBackend12::FlushGPU();
		Demo::s_suspendRendering = false;
	}
};

#define SCOPED_PAUSE_RENDERING ScopedPauseRendering temp

bool Demo::Initialize(const HWND& windowHandle, const uint32_t resX, const uint32_t resY)
{
	s_aspectRatio = resX / (float)resY;

	bool ok = RenderBackend12::Initialize(windowHandle, resX, resY, s_globalConfig);
	ok = ok && ShaderCompiler::Initialize();

	InitializeRenderer(resX, resY);
	s_debugDraw.Initialize();

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init(windowHandle);

	// List of models
	for (auto& entry : std::filesystem::recursive_directory_iterator(CONTENT_DIR))
	{
		if (entry.is_regular_file() && 
			entry.path().extension().string() == ".gltf" &&
			!entry.path().parent_path().string().ends_with(".model-cache"))
		{
			s_modelList.push_back(entry.path().filename().wstring());
		}
	}

	// List of HDRIs
	for (auto& entry : std::filesystem::recursive_directory_iterator(CONTENT_DIR))
	{
		if (entry.is_regular_file() && entry.path().extension().string() == ".hdr")
		{
			s_hdriList.push_back(entry.path().filename().wstring());
		}
	}

	return ok;
}

void Demo::Tick(float deltaTime)
{
	SCOPED_CPU_EVENT("tick_demo", PIX_COLOR_INDEX(1));

	// Scene rotation
	static float rotX = 0.f;
	static float rotY = 0.f;

	// Reload scene model if required
	if (s_scene.m_modelFilename.empty() ||
		s_scene.m_modelFilename != s_globalConfig.ModelFilename)
	{
		// Async loading of model. Create a temp scene on the stack 
		// and replace the main scene once loading has finished.
		// --> A shared pointer is used to keep the temp scene alive and pass to the continuation task. 
		// --> The modelFilename is updated immediately to prevent subsequent reloads before the async reloading has finished.
		// TODO: support cancellation if a different model load is triggered
		std::shared_ptr<FScene> newScene = std::make_shared<FScene>();
		s_scene.m_modelFilename = s_globalConfig.ModelFilename;
		concurrency::task<void> loadSceneTask = concurrency::create_task([newScene]()
		{
			newScene->ReloadModel(s_globalConfig.ModelFilename);
			newScene->ReloadEnvironment(s_globalConfig.EnvironmentFilename);
		}).then([newScene]()
		{
			SCOPED_PAUSE_RENDERING;
			s_scene = std::move(*newScene);
			s_view.Reset(&s_scene);
			rotX = 0.f;
			rotY = 0.f;
		});

		// Block when loading for the first time so that we have a scene to render.
		// Subsequent loads will load in the background while displaying the previous scene.
		static bool bInitialLoad = true;
		if (bInitialLoad)
		{
			loadSceneTask.wait();
			bInitialLoad = false;
		}
	}

	// Reload scene environment if required
	if (s_scene.m_environmentFilename.empty() ||
		s_scene.m_environmentFilename != s_globalConfig.EnvironmentFilename)
	{
		SCOPED_PAUSE_RENDERING;
		s_scene.ReloadEnvironment(s_globalConfig.EnvironmentFilename);
	}

	// Tick components
	s_controller.Tick(deltaTime);
	s_view.Tick(deltaTime, &s_controller);
	if (!s_globalConfig.FreezeCulling)
	{
		s_cullingView = s_view;
	}

	// Handle scene rotation
	{
		// Mouse rotation but as applied in view space
		Matrix rotation = Matrix::Identity;

		float newRotX = s_controller.RotateSceneX();
		float newRotY = s_controller.RotateSceneY();
		if (newRotX != 0.f || newRotY != 0.f)
		{
			Demo::ResetPathtraceAccumulation();
		}

		rotX -= newRotX;
		if (rotX != 0.f)
		{
			rotation *= Matrix::CreateFromAxisAngle(s_view.m_up, rotX);
		}

		rotY -= newRotY;
		if (rotY != 0.f)
		{
			rotation *= Matrix::CreateFromAxisAngle(s_view.m_right, rotY);
		}

		// Rotate to view space, apply view space rotation and then rotate back to world space
		s_scene.m_rootTransform = rotation;
	}

	UpdateUI(deltaTime);
}

void Demo::HeartbeatThread()
{
	while (true)
	{
		if (!s_suspendRendering)
		{
			RenderBackend12::RecompileModifiedShaders(&Demo::ResetPathtraceAccumulation);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}
}

void Demo::Teardown(HWND& windowHandle)
{
	s_suspendRendering = true;
	RenderBackend12::FlushGPU();

	s_scene.Clear();
	s_textureCache.Clear();
	s_samplerCache.Clear();
	s_debugDraw.~FDebugDraw();

	Demo::TeardownRenderer();

	if (windowHandle)
	{
		RenderBackend12::Teardown();
		ShaderCompiler::Teardown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
	}
}

void Demo::OnMouseMove(WPARAM buttonState, int x, int y)
{
	ImGuiIO& io = ImGui::GetIO();
	if (!io.WantCaptureMouse)
	{
		s_controller.MouseMove(buttonState, POINT{ x, y });
	}
}

void Demo::UpdateUI(float deltaTime)
{
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

	uint32_t fontSrvIndex = s_textureCache.CacheTexture2D(&uploader, L"imgui_fonts", DXGI_FORMAT_R8G8B8A8_UNORM, img.width, img.height, &img, 1);
	ImGui::GetIO().Fonts->TexID = (ImTextureID)fontSrvIndex;
	uploader.SubmitUploads(cmdList);
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

	bool bResetPathtracelAccumulation = false;

	ImGui::Begin("Menu");
	{
		ImGui::Checkbox("TAA", &s_globalConfig.EnableTAA);
		ImGui::SameLine();
		ImGui::Checkbox("Pathtracing", &s_globalConfig.PathTrace);

		// Model
		static int curModelIndex = std::find(s_modelList.begin(), s_modelList.end(), s_globalConfig.ModelFilename) - s_modelList.begin();
		std::string comboLabel = ws2s(s_modelList[curModelIndex]);
		if (ImGui::BeginCombo("Model", comboLabel.c_str(), ImGuiComboFlags_None))
		{
			for (int n = 0; n < s_modelList.size(); n++)
			{
				const bool bSelected = (curModelIndex == n);
				if (ImGui::Selectable(ws2s(s_modelList[n]).c_str(), bSelected))
				{
					curModelIndex = n;
					s_globalConfig.ModelFilename = s_modelList[n];
				}

				if (bSelected)
				{
					ImGui::SetItemDefaultFocus();
				}
			}

			ImGui::EndCombo();
		}

		// --------------------------------------------------------------------------------------------------------------------------------------------

		if (!s_globalConfig.PathTrace)
			ImGui::BeginDisabled();

		if (ImGui::CollapsingHeader("Path Tracing"))
		{
			bResetPathtracelAccumulation |= ImGui::SliderInt("Max. Sample Count", (int*) &s_globalConfig.MaxSampleCount, 1, 1024);
			bResetPathtracelAccumulation |= ImGui::SliderFloat("Camera Aperture", &s_globalConfig.Pathtracing_CameraAperture, 0.f, 0.1f);
			bResetPathtracelAccumulation |= ImGui::SliderFloat("Camera Focal Length", &s_globalConfig.Pathtracing_CameraFocalLength, 1.f, 15.f);
		}

		if (!s_globalConfig.PathTrace)
			ImGui::EndDisabled();

		// -----------------------------------------------------------------------------------------------------------------------------------------

		if (ImGui::CollapsingHeader("Scene"))
		{
			if (ImGui::BeginTabBar("ScaneTabs", ImGuiTabBarFlags_None))
			{
				if (ImGui::BeginTabItem("Environment/Sky"))
				{
					ImGui::RadioButton("Environment Map", &s_globalConfig.EnvSkyMode, (int)EnvSkyMode::Environmentmap);
					ImGui::SameLine();
					ImGui::RadioButton("Dynamic Sky", &s_globalConfig.EnvSkyMode, (int)EnvSkyMode::DynamicSky);


					// ----------------------------------------------------------
					// Environment map
					if (s_globalConfig.EnvSkyMode != (int)EnvSkyMode::Environmentmap)
						ImGui::BeginDisabled();
						
					static int curHdriIndex = std::find(s_hdriList.begin(), s_hdriList.end(), s_globalConfig.EnvironmentFilename) - s_hdriList.begin();
					comboLabel = ws2s(s_hdriList[curHdriIndex]);
					if (ImGui::BeginCombo("Environment map", comboLabel.c_str(), ImGuiComboFlags_None))
					{
						for (int n = 0; n < s_hdriList.size(); n++)
						{
							const bool bSelected = (curHdriIndex == n);
							if (ImGui::Selectable(ws2s(s_hdriList[n]).c_str(), bSelected))
							{
								curHdriIndex = n;
								s_globalConfig.EnvironmentFilename = s_hdriList[n];
								bResetPathtracelAccumulation = true;
							}

							if (bSelected)
							{
								ImGui::SetItemDefaultFocus();
							}
						}

						ImGui::EndCombo();
					}

					if (s_globalConfig.EnvSkyMode != (int)EnvSkyMode::Environmentmap)
						ImGui::EndDisabled();

					// ----------------------------------------------------------
					// Dynamic Sky
					if (s_globalConfig.EnvSkyMode != (int)EnvSkyMode::DynamicSky)
						ImGui::BeginDisabled();

					ImGui::SliderFloat("Turbidity", &s_globalConfig.Turbidity, 2.f, 6.f);

					if (s_globalConfig.EnvSkyMode != (int)EnvSkyMode::DynamicSky)
						ImGui::EndDisabled();

					ImGui::EndTabItem();
				}

				if (ImGui::BeginTabItem("Lights"))
				{
					int lightCount = GetScene()->m_sceneLights.GetCount();
					if (lightCount > 0)
					{
						for (int i = 0; i < lightCount; ++i)
						{
							// Add indent
							ImGui::TreePush();

							int lightIndex = GetScene()->m_sceneLights.m_entityList[i];
							const std::string& lightName = GetScene()->m_sceneLights.m_entityNames[i];

							FLight& light = GetScene()->m_globalLightList[lightIndex];
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

								static float color[4] = { light.m_color.x, light.m_color.y, light.m_color.z, 1.f };
								ImGui::ColorEdit4("Color", (float*)&color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoAlpha);

								if (light.m_type != Light::Directional)
								{
									ImGui::SliderFloat("Intensity (cd)", &light.m_intensity, 0.f, 10000.f);
									ImGui::SliderFloat("Range", &light.m_range, 0.f, 500.f);
								}
								else
								{
									ImGui::SliderFloat("Intensity (lux)", &light.m_intensity, 0.f, 10000.f);
								}

								if (light.m_type == Light::Spot)
								{
									ImGui::SliderFloat("Inner Cone Angle (rad)", &light.m_spotAngles.x, 0.f, 3.14159f);
									ImGui::SliderFloat("Outer Cone Angle (rad)", &light.m_spotAngles.y, 0.f, 3.14159f);
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
			ImGui::SliderFloat("Speed", &s_globalConfig.CameraSpeed, 1.0f, 20.0f);

			static float fovDeg = DirectX::XMConvertToDegrees(s_globalConfig.Fov);
			ImGui::SliderFloat("FOV", &fovDeg, 0.0f, 140.0f);
			s_globalConfig.Fov = DirectX::XMConvertToRadians(fovDeg);

			ImGui::SliderFloat("Exposure", &s_globalConfig.Exposure, 1.0f, 20.0f);

			if (ImGui::Button("Reset"))
			{
				s_view.Reset(&Demo::s_scene);
			}
		}		

		// --------------------------------------------------------------------------------------------------------------------------------------------

		if (ImGui::CollapsingHeader("Debug"))
		{
			ImGui::Checkbox("Freeze Culling", &s_globalConfig.FreezeCulling);

			int currentViewMode = s_globalConfig.Viewmode;
			if (ImGui::TreeNode("View Modes"))
			{
				ImGui::RadioButton("Normal", &s_globalConfig.Viewmode, (int)Viewmode::Normal);
				ImGui::RadioButton("Nan Check", &s_globalConfig.Viewmode, (int)Viewmode::NanCheck);
				ImGui::RadioButton("Lighting Only", &s_globalConfig.Viewmode, (int)Viewmode::LightingOnly);
				ImGui::RadioButton("Roughness", &s_globalConfig.Viewmode, (int)Viewmode::Roughness);
				ImGui::RadioButton("Metallic", &s_globalConfig.Viewmode, (int)Viewmode::Metallic);
				ImGui::RadioButton("Base Color", &s_globalConfig.Viewmode, (int)Viewmode::BaseColor);
				ImGui::RadioButton("Normalmap", &s_globalConfig.Viewmode, (int)Viewmode::Normalmap);
				ImGui::RadioButton("Emissive", &s_globalConfig.Viewmode, (int)Viewmode::Emissive);
				ImGui::RadioButton("Reflections", &s_globalConfig.Viewmode, (int)Viewmode::Reflections);
				ImGui::RadioButton("Object IDs", &s_globalConfig.Viewmode, (int)Viewmode::ObjectIds);
				ImGui::RadioButton("Triangle IDs", &s_globalConfig.Viewmode, (int)Viewmode::TriangleIds);
				ImGui::RadioButton("Light Cluster Slices", &s_globalConfig.Viewmode, (int)Viewmode::LightClusterSlices);
				ImGui::TreePop();
			}

			bResetPathtracelAccumulation |= (s_globalConfig.Viewmode != currentViewMode);

			if (ImGui::TreeNode("Light Components"))
			{
				ImGui::Checkbox("Direct Lighting", &s_globalConfig.EnableDirectLighting);
				ImGui::Checkbox("Diffuse IBL", &s_globalConfig.EnableDiffuseIBL);
				ImGui::Checkbox("Specular IBL", &s_globalConfig.EnableSpecularIBL);
				ImGui::TreePop();
			}

			if (ImGui::TreeNode("Debug Rendering"))
			{
				ImGui::Checkbox("Light Bounds", &s_globalConfig.ShowLightBounds);
				ImGui::TreePop();
			}
		}
	}
	ImGui::End();

	ImGui::Begin("Render Stats");
	{
		ImGui::Text("%.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

		FRenderStatsBuffer stats = Demo::GetRenderStats();
		ImGui::Text("Primitive Culling		%.2f%%", 100.f * stats.m_culledPrimitives / (float) GetScene()->m_primitiveCount);

		const size_t numLights = GetScene()->m_sceneLights.GetCount();
		if (numLights == 0)
			ImGui::BeginDisabled();

		ImGui::Text("Light Culling			%.2f%%", numLights == 0 ? 0.f : 100.f * stats.m_culledLights / (float) (numLights * Demo::s_globalConfig.LightClusterDimX * Demo::s_globalConfig.LightClusterDimY * Demo::s_globalConfig.LightClusterDimZ));

		if (numLights == 0)
			ImGui::EndDisabled();
	}
	ImGui::End();

	ImGui::PopStyleVar();

	ImGui::EndFrame();
	ImGui::Render();

	if (bResetPathtracelAccumulation)
	{
		Demo::ResetPathtraceAccumulation();
	}
}

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Scene
//-----------------------------------------------------------------------------------------------------------------------------------------------

bool LoadImageCallback(
	tinygltf::Image* image,
	const int image_idx,
	std::string* err,
	std::string* warn,
	int req_width,
	int req_height,
	const unsigned char* bytes,
	int size,
	void* user_data)
{
	const char* pathStr = (const char*)user_data;
	std::filesystem::path dirPath{ pathStr };
	std::filesystem::path srcFilename{ image->uri };
	std::filesystem::path destFilename = dirPath / srcFilename.stem();
	destFilename += std::filesystem::path{ ".dds" };

	if (Demo::s_globalConfig.UseContentCache && std::filesystem::exists(destFilename))
	{
		// Skip image data initialization. We will load compressed file from the cache instead
		image->image.clear();
		image->uri = destFilename.string();
		return true;
	}
	else
	{
		// Initialize image data using built-in loader
		bool ok = tinygltf::LoadImageData(image, image_idx, err, warn, req_width, req_height, bytes, size, user_data);
		if (ok)
		{
			DebugAssert(image->pixel_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE);
			size_t bpp = (image->bits * 4) / 8;
			size_t rowPitch = bpp * image->width;

			// If the image has less than 4 components, pad in the remaining channels.
			if (image->component < 4)
			{
				std::vector<uint8_t> originalImageData{ image->image };
				size_t originalBpp = (image->bits * image->component) / 8;
				size_t originalRowPitch = originalBpp * image->width;

				image->image.resize(rowPitch * image->height);
				for (int row = 0; row < image->height; ++row)
				{
					for (int col = 0; col < image->width; ++col)
					{
						const size_t srcPixelOffset = row * originalRowPitch + col * originalBpp;
						const size_t destPixelOffset = row * rowPitch + col * bpp;
						image->image[destPixelOffset + 0] = originalImageData[srcPixelOffset + 0];
						image->image[destPixelOffset + 1] = image->component > 1 ? originalImageData[srcPixelOffset + 1] : 0;
						image->image[destPixelOffset + 2] = image->component > 2 ? originalImageData[srcPixelOffset + 2] : 0;
						image->image[destPixelOffset + 3] = image->component > 3 ? originalImageData[srcPixelOffset + 3] : 255;
					}
				}

				image->component = 4;
			}
		}

		return ok;
	}
}

std::string GetContentCachePath(const std::string filename)
{
	std::filesystem::path filepath { filename };
	std::filesystem::path dir{ ".content-cache" };
	std::filesystem::path dirPath = filepath.parent_path() / dir;

	if (!std::filesystem::exists(dirPath))
	{
		bool ok = std::filesystem::create_directory(dirPath);
		DebugAssert(ok, "Failed to create cache dir");
	}

	return dirPath.string();
}

void FScene::ReloadModel(const std::wstring& filename)
{
	SCOPED_CPU_EVENT("reload_model", PIX_COLOR_DEFAULT);

	std::string modelFilepath = GetFilepathA(ws2s(filename));

	tinygltf::TinyGLTF loader;
	m_textureCachePath = GetContentCachePath(modelFilepath);
	const char* path = m_textureCachePath.c_str();
	loader.SetImageLoader(&LoadImageCallback, (void*)path);

	// Load from model cache if a cached version exists
	m_modelCachePath = GetContentCachePath(modelFilepath);
	std::filesystem::path cachedFilepath = std::filesystem::path{ m_modelCachePath } / std::filesystem::path{ ws2s(filename) };
	if (Demo::s_globalConfig.UseContentCache && std::filesystem::exists(cachedFilepath))
	{
		modelFilepath = cachedFilepath.string();
	}

	// Load GLTF
	tinygltf::Model model;
	{
		SCOPED_CPU_EVENT("tiny_gltf_load", PIX_COLOR_DEFAULT);

		std::string errors, warnings;
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
	}

	m_modelFilename = filename;

	// Clear previous scene
	Clear();

	// Load assets
	bool requiresResave = MeshUtils::FixupMeshes(model);
	LoadMeshBuffers(model);
	LoadMeshBufferViews(model);
	LoadMeshAccessors(model);
	LoadMaterials(model);
	LoadLights(model);

	//m_sceneMeshes.Reserve(model.meshes.size());
	
	// GlTF uses a right handed coordinate. Use the following root transform to convert it to LH.
	Matrix RH2LH = Matrix
	{
		Vector3{1.f, 0.f , 0.f},
		Vector3{0.f, 1.f , 0.f},
		Vector3{0.f, 0.f, -1.f}
	};

	// Parse GLTF and initialize scene
	// See https://github.com/KhronosGroup/glTF-Tutorials/blob/master/gltfTutorial/gltfTutorial_003_MinimalGltfFile.md
	for (tinygltf::Scene& scene : model.scenes)
	{
		for (const int nodeIndex : scene.nodes)
		{
			LoadNode(nodeIndex, model, RH2LH);
		}
	}

	// If the model required fixup during load, resave a cached copy so that 
	// subsequent loads are faster.
	/*if (requiresResave && Config::g_useContentCache)
	{
		ok = loader.WriteGltfSceneToFile(&model, cachedFilepath.string(), false, false, true, false);
		DebugAssert(ok, "Failed to save cached glTF model");
	}*/

	// Scene bounds
	std::vector<DirectX::BoundingBox> meshWorldBounds(m_sceneMeshes.m_objectSpaceBoundsList.size());
	for (int i = 0; i < meshWorldBounds.size(); ++i)
	{
		m_sceneMeshes.m_objectSpaceBoundsList[i].Transform(meshWorldBounds[i], m_sceneMeshes.m_transformList[i]);
	}

	m_sceneBounds = meshWorldBounds[0];
	for (const auto& bb : meshWorldBounds)
	{
		DirectX::BoundingBox::CreateMerged(m_sceneBounds, m_sceneBounds, bb);
	}

	// Primitive counts
	m_primitiveCount = 0;
	for (const FMesh& mesh : m_sceneMeshes.m_entityList)
	{
		m_primitiveCount += mesh.m_primitives.size();
	}

	CreateAccelerationStructures(model);
	CreateGpuPrimitiveBuffers();
	CreateGpuLightBuffers();

	// Wait for all loading jobs to finish
	auto joinTask = concurrency::when_all(std::begin(m_loadingJobs), std::end(m_loadingJobs));
	joinTask.wait();
	m_loadingJobs.clear();
}

void FScene::ReloadEnvironment(const std::wstring& filename)
{
	m_environmentSky = Demo::s_textureCache.CacheHDRI(filename);
	m_environmentFilename = filename;
}

void FScene::LoadNode(int nodeIndex, tinygltf::Model& model, const Matrix& parentTransform)
{
	const tinygltf::Node& node = model.nodes[nodeIndex];
	
	// Transform (GLTF uses column-major storage)
	Matrix nodeTransform = Matrix::Identity;
	if (!node.matrix.empty())
	{
		const auto& m = node.matrix;
		nodeTransform = Matrix { 
			(float)m[0], (float)m[1], (float)m[2], (float)m[3],
			(float)m[4], (float)m[5], (float)m[6], (float)m[7],
			(float)m[8], (float)m[9], (float)m[10],(float)m[11],
			(float)m[12], (float)m[13], (float)m[14],(float)m[15]
		};
	}
	else if (!node.translation.empty() || !node.rotation.empty() || !node.scale.empty())
	{
		Matrix translation = !node.translation.empty() ? Matrix::CreateTranslation((float)node.translation[0], (float)node.translation[1], (float)node.translation[2]) : Matrix::Identity;
		Matrix rotation = !node.rotation.empty() ? Matrix::CreateFromQuaternion(Quaternion{ (float)node.rotation[0], (float)node.rotation[1], (float)node.rotation[2], (float)node.rotation[3] }) : Matrix::Identity;
		Matrix scale = !node.scale.empty() ? Matrix::CreateScale(node.scale[0], node.scale[1], node.scale[2]) : Matrix::Identity;

		nodeTransform = scale * rotation * translation;
	}

	if (node.camera != -1)
	{
		LoadCamera(node.camera, model, nodeTransform * parentTransform);
	}

	if (node.mesh != -1)
	{
		LoadMesh(node.mesh, model, nodeTransform * parentTransform);
	}

	auto lightIt = node.extensions.find("KHR_lights_punctual");
	if (lightIt != node.extensions.cend())
	{
		int lightIndex = lightIt->second.Get("light").GetNumberAsInt();
		m_sceneLights.m_entityList.push_back(lightIndex);
		m_sceneLights.m_entityNames.push_back(model.lights[lightIndex].name);
		m_sceneLights.m_transformList.push_back(nodeTransform * parentTransform);
	}

	for (const int childIndex : node.children)
	{
		LoadNode(childIndex, model, nodeTransform * parentTransform);
	}
}

void FScene::LoadMesh(int meshIndex, const tinygltf::Model& model, const Matrix& parentTransform)
{
	const tinygltf::Mesh& mesh = model.meshes[meshIndex];
	FSceneMeshEntities* sceneCollection = mesh.name.starts_with("decal") ? &m_sceneMeshDecals : &m_sceneMeshes;

	SCOPED_CPU_EVENT("load_mesh", PIX_COLOR_DEFAULT);

	auto CalcBounds = [&model](int positionAccessorIndex) -> DirectX::BoundingBox
	{
		const tinygltf::Accessor& accessor = model.accessors[positionAccessorIndex];
		const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];

		size_t dataSize = tinygltf::GetComponentSizeInBytes(accessor.componentType) * tinygltf::GetNumComponentsInType(accessor.type);
		size_t dataStride = accessor.ByteStride(bufferView);

		const uint8_t* pData = &model.buffers[bufferView.buffer].data[bufferView.byteOffset + accessor.byteOffset];

		DirectX::BoundingBox bb = {};
		DirectX::BoundingBox::CreateFromPoints(bb, accessor.count, (DirectX::XMFLOAT3*)pData, dataStride);
		return bb;
	};

	FMesh newMesh = {};
	newMesh.m_primitives.resize(mesh.primitives.size());

	DirectX::BoundingBox meshBounds = {};

	// Each primitive is a separate render mesh with its own vertex and index buffers
	for (int primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
	{
		const tinygltf::Primitive& primitive = mesh.primitives[primitiveIndex];
		FMeshPrimitive& outPrimitive = newMesh.m_primitives[primitiveIndex];

		// Index data
		const tinygltf::Accessor& indexAccessor = model.accessors[primitive.indices];
		outPrimitive.m_indexCount = indexAccessor.count;
		outPrimitive.m_indexAccessor = primitive.indices;

		// Position data
		auto posIt = primitive.attributes.find("POSITION");
		DebugAssert(posIt != primitive.attributes.cend());
		outPrimitive.m_positionAccessor = posIt->second;

		// UV data
		auto uvIt = primitive.attributes.find("TEXCOORD_0");
		outPrimitive.m_uvAccessor = (uvIt != primitive.attributes.cend() ? uvIt->second : -1);

		// Normal data
		auto normalIt = primitive.attributes.find("NORMAL");
		outPrimitive.m_normalAccessor = (normalIt != primitive.attributes.cend() ? normalIt->second : -1);

		// Tangent data
		auto tangentIt = primitive.attributes.find("TANGENT");
		outPrimitive.m_tangentAccessor = (tangentIt != primitive.attributes.cend() ? tangentIt->second : -1);

		// Topology
		switch (primitive.mode)
		{
		case TINYGLTF_MODE_POINTS:
			outPrimitive.m_topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
			break;
		case TINYGLTF_MODE_LINE:
			outPrimitive.m_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
			break;
		case TINYGLTF_MODE_LINE_STRIP:
			outPrimitive.m_topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
			break;
		case TINYGLTF_MODE_TRIANGLES:
			outPrimitive.m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			break;
		case TINYGLTF_MODE_TRIANGLE_STRIP:
			outPrimitive.m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
			break;
		default:
			DebugAssert(false);
		}

		// Material
		outPrimitive.m_materialIndex = primitive.material;

		// Bounds
		DirectX::BoundingBox primitiveBounds = CalcBounds(posIt->second);
		DirectX::BoundingBox::CreateMerged(meshBounds, meshBounds, primitiveBounds);
		DirectX::BoundingSphere::CreateFromBoundingBox(outPrimitive.m_boundingSphere, primitiveBounds);
	}

	sceneCollection->m_entityList.push_back(newMesh);
	sceneCollection->m_transformList.push_back(parentTransform);
	sceneCollection->m_entityNames.push_back(mesh.name);
	sceneCollection->m_objectSpaceBoundsList.push_back(meshBounds);
}

void FModelLoader::LoadMeshBuffers(const tinygltf::Model& model)
{
	SCOPED_CPU_EVENT("load_mesh_buffers", PIX_COLOR_DEFAULT);

	size_t uploadSize = 0;
	for (const tinygltf::Buffer& buffer : model.buffers)
	{
		uploadSize += buffer.data.size();
	}

	FResourceUploadContext uploader{ uploadSize };

	m_meshBuffers.resize(model.buffers.size());
	for (int bufferIndex = 0; bufferIndex < model.buffers.size(); ++bufferIndex)
	{
		std::wstringstream s;
		s << L"scene_mesh_buffer_" << bufferIndex;

		m_meshBuffers[bufferIndex] = RenderBackend12::CreateBuffer(
			s.str(),
			BufferType::Raw,
			ResourceAccessMode::GpuReadOnly,
			ResourceAllocationType::Committed,
			model.buffers[bufferIndex].data.size(),
			false,
			model.buffers[bufferIndex].data.data(),
			&uploader);
	}

	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_mesh_buffers", D3D12_COMMAND_LIST_TYPE_DIRECT);
	uploader.SubmitUploads(cmdList);
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
}

void FModelLoader::LoadMeshBufferViews(const tinygltf::Model& model)
{
	SCOPED_CPU_EVENT("load_mesh_bufferviews", PIX_COLOR_DEFAULT);

	// CPU Copy
	std::vector<FMeshBufferView> views(model.bufferViews.size());
	concurrency::parallel_for(0, (int)model.bufferViews.size(), [&](int viewIndex)
	{
		const int meshBufferIndex = model.bufferViews[viewIndex].buffer;
		views[viewIndex].m_bufferSrvIndex = m_meshBuffers[meshBufferIndex]->m_srvIndex;
		views[viewIndex].m_byteLength = model.bufferViews[viewIndex].byteLength;
		views[viewIndex].m_byteOffset = model.bufferViews[viewIndex].byteOffset;
	});

	const size_t bufferSize = views.size() * sizeof(FMeshBufferView);
	FResourceUploadContext uploader{ bufferSize };

	m_packedMeshBufferViews = RenderBackend12::CreateBuffer(
		L"scene_mesh_buffer_views",
		BufferType::Raw,
		ResourceAccessMode::GpuReadOnly,
		ResourceAllocationType::Committed,
		bufferSize,
		false,
		(const uint8_t*) views.data(),
		&uploader);

	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_mesh_buffer_views", D3D12_COMMAND_LIST_TYPE_DIRECT);
	uploader.SubmitUploads(cmdList);
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
}

void FModelLoader::LoadMeshAccessors(const tinygltf::Model& model)
{
	SCOPED_CPU_EVENT("load_mesh_accessors", PIX_COLOR_DEFAULT);

	// CPU Copy
	std::vector<FMeshAccessor> accessors(model.accessors.size());
	concurrency::parallel_for(0, (int)model.accessors.size(), [&](int i)
	{
		const int bufferViewIndex = model.accessors[i].bufferView;
		accessors[i].m_bufferViewIndex = bufferViewIndex;
		accessors[i].m_byteOffset = (uint32_t)model.accessors[i].byteOffset;
		accessors[i].m_byteStride = model.accessors[i].ByteStride(model.bufferViews[bufferViewIndex]);
	});

	const size_t bufferSize = accessors.size() * sizeof(FMeshAccessor);
	FResourceUploadContext uploader{ bufferSize };

	m_packedMeshAccessors = RenderBackend12::CreateBuffer(
		L"scene_mesh_accessors",
		BufferType::Raw,
		ResourceAccessMode::GpuReadOnly,
		ResourceAllocationType::Committed,
		bufferSize,
		false,
		(const uint8_t*) accessors.data(),
		&uploader);

	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_mesh_accessors", D3D12_COMMAND_LIST_TYPE_DIRECT);
	uploader.SubmitUploads(cmdList);
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
}

void FScene::CreateGpuPrimitiveBuffers()
{
	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_primitives", D3D12_COMMAND_LIST_TYPE_DIRECT);

	// Packed buffer that contains an array of FGpuPrimitive(s)
	{
		std::vector<FGpuPrimitive> primitives;
		for (int meshIndex = 0; meshIndex < m_sceneMeshes.GetCount(); ++meshIndex)
		{
			const FMesh& mesh = m_sceneMeshes.m_entityList[meshIndex];
			for (int primitiveIndex = 0; primitiveIndex < mesh.m_primitives.size(); ++primitiveIndex)
			{
				const FMeshPrimitive& primitive = mesh.m_primitives[primitiveIndex];
				const DirectX::BoundingSphere& bounds = primitive.m_boundingSphere;
				FGpuPrimitive newPrimitive = {};
				newPrimitive.m_localToWorld = m_sceneMeshes.m_transformList[meshIndex];
				newPrimitive.m_boundingSphere = Vector4(bounds.Center.x, bounds.Center.y, bounds.Center.z, bounds.Radius);
				newPrimitive.m_indexAccessor = primitive.m_indexAccessor;
				newPrimitive.m_positionAccessor = primitive.m_positionAccessor;
				newPrimitive.m_uvAccessor = primitive.m_uvAccessor;
				newPrimitive.m_normalAccessor = primitive.m_normalAccessor;
				newPrimitive.m_tangentAccessor = primitive.m_tangentAccessor;
				newPrimitive.m_materialIndex = primitive.m_materialIndex;
				newPrimitive.m_indexCount = primitive.m_indexCount;
				newPrimitive.m_indicesPerTriangle = 3;
				primitives.push_back(newPrimitive);
			}
		}

		const size_t bufferSize = primitives.size() * sizeof(FGpuPrimitive);
		FResourceUploadContext uploader{ bufferSize };

		m_packedPrimitives = RenderBackend12::CreateBuffer(
			L"scene_primitives",
			BufferType::Raw,
			ResourceAccessMode::GpuReadOnly,
			ResourceAllocationType::Committed,
			bufferSize,
			false,
			(const uint8_t*)primitives.data(),
			&uploader);

		uploader.SubmitUploads(cmdList);
	}

	// Buffer that contains primitive count for each mesh. This is used to calculate an offset to read from the above buffer
	{
		std::vector<uint32_t> primitiveCounts;
		for (const auto& mesh : m_sceneMeshes.m_entityList)
		{
			primitiveCounts.push_back(mesh.m_primitives.size());
		}

		const size_t bufferSize = primitiveCounts.size() * sizeof(uint32_t);
		FResourceUploadContext uploader{ bufferSize };

		m_packedPrimitiveCounts = RenderBackend12::CreateBuffer(
			L"scene_primitive_counts",
			BufferType::Raw,
			ResourceAccessMode::GpuReadOnly,
			ResourceAllocationType::Committed,
			bufferSize,
			false,
			(const uint8_t*)primitiveCounts.data(),
			&uploader);

		uploader.SubmitUploads(cmdList);
	}

	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
}

void FScene::CreateGpuLightBuffers()
{
	const size_t numLights = m_sceneLights.GetCount();
	if (numLights > 0)
	{
		const size_t indexBufferSize = numLights * sizeof(int);
		const size_t transformsBufferSize = numLights * sizeof(Matrix);
		FResourceUploadContext uploader{ indexBufferSize + transformsBufferSize };

		m_packedLightIndices = RenderBackend12::CreateBuffer(
			L"scene_light_indices",
			BufferType::Raw,
			ResourceAccessMode::GpuReadOnly,
			ResourceAllocationType::Committed,
			indexBufferSize,
			false,
			(const uint8_t*)m_sceneLights.m_entityList.data(),
			&uploader);

		m_packedLightTransforms = RenderBackend12::CreateBuffer(
			L"scene_light_transforms",
			BufferType::Raw,
			ResourceAccessMode::GpuReadOnly,
			ResourceAllocationType::Committed,
			transformsBufferSize,
			false,
			(const uint8_t*)m_sceneLights.m_transformList.data(),
			&uploader);

		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_lights", D3D12_COMMAND_LIST_TYPE_DIRECT);
		uploader.SubmitUploads(cmdList);
		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
	}
}

void FScene::CreateAccelerationStructures(const tinygltf::Model& model)
{
	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"create_acceleration_structure", D3D12_COMMAND_LIST_TYPE_DIRECT);

	std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
	for (int meshIndex = 0; meshIndex < m_sceneMeshes.GetCount(); ++meshIndex)
	{
		const FMesh& mesh = m_sceneMeshes.m_entityList[meshIndex];
		const std::string& meshName = m_sceneMeshes.m_entityNames[meshIndex];
		auto search = m_blasList.find(meshName);
		if (search == m_blasList.cend())
		{
			std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> primitiveDescs;
			primitiveDescs.reserve(mesh.m_primitives.size());

			// Create D3D12_RAYTRACING_GEOMETRY_DESC for each primitive in the mesh
			for (int primitiveIndex = 0; primitiveIndex < mesh.m_primitives.size(); ++primitiveIndex)
			{
				const FMeshPrimitive& primitive = mesh.m_primitives[primitiveIndex];

				tinygltf::Accessor posAccessor = model.accessors[primitive.m_positionAccessor];
				tinygltf::Accessor indexAccessor = model.accessors[primitive.m_indexAccessor];
				tinygltf::BufferView posView = model.bufferViews[posAccessor.bufferView];
				tinygltf::BufferView indexView = model.bufferViews[indexAccessor.bufferView];

				DXGI_FORMAT vertexFormat;
				switch (posAccessor.type)
				{
				case TINYGLTF_TYPE_VEC2:
					vertexFormat = DXGI_FORMAT_R32G32_FLOAT;
					break;
				case TINYGLTF_TYPE_VEC3:
					vertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
					break;
				case TINYGLTF_TYPE_VEC4:
					vertexFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
					break;
				}

				DXGI_FORMAT indexFormat;
				switch (indexAccessor.componentType)
				{
				case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
					indexFormat = DXGI_FORMAT_R8_UINT;
					break;
				case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
					indexFormat = DXGI_FORMAT_R16_UINT;
					break;
				case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
					indexFormat = DXGI_FORMAT_R32_UINT;
					break;
				}

				D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
				geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
				geometry.Triangles.VertexBuffer.StartAddress = m_meshBuffers[posView.buffer]->m_resource->m_d3dResource->GetGPUVirtualAddress() + posAccessor.byteOffset + posView.byteOffset;
				geometry.Triangles.VertexBuffer.StrideInBytes = posAccessor.ByteStride(posView);
				geometry.Triangles.VertexCount = posAccessor.count;
				geometry.Triangles.VertexFormat = vertexFormat;
				geometry.Triangles.IndexBuffer = m_meshBuffers[indexView.buffer]->m_resource->m_d3dResource->GetGPUVirtualAddress() + indexAccessor.byteOffset + indexView.byteOffset;
				geometry.Triangles.IndexFormat = indexFormat;
				geometry.Triangles.IndexCount = indexAccessor.count;
				geometry.Triangles.Transform3x4 = 0;
				geometry.Flags = m_materialList[primitive.m_materialIndex].m_alphaMode == AlphaMode::Opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
				primitiveDescs.push_back(geometry);
			}

			// Build BLAS - One per mesh entity
			{
				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputsDesc = {};
				blasInputsDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
				blasInputsDesc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
				blasInputsDesc.pGeometryDescs = primitiveDescs.data();
				blasInputsDesc.NumDescs = primitiveDescs.size();
				blasInputsDesc.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasPreBuildInfo = {};
				RenderBackend12::GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputsDesc, &blasPreBuildInfo);

				auto blasScratch = RenderBackend12::CreateBuffer(
					L"blas_scratch",
					BufferType::AccelerationStructure,
					ResourceAccessMode::GpuWriteOnly,
					ResourceAllocationType::Pooled,
					GetAlignedSize(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, blasPreBuildInfo.ScratchDataSizeInBytes));

				std::wstringstream s;
				s << s2ws(meshName) << L"_blas";
				m_blasList[meshName] = RenderBackend12::CreateBuffer(
					s.str(),
					BufferType::AccelerationStructure,
					ResourceAccessMode::GpuReadWrite,
					ResourceAllocationType::Committed,
					GetAlignedSize(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, blasPreBuildInfo.ResultDataMaxSizeInBytes));

				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
				buildDesc.Inputs = blasInputsDesc;
				buildDesc.ScratchAccelerationStructureData = blasScratch->m_resource->m_d3dResource->GetGPUVirtualAddress();
				buildDesc.DestAccelerationStructureData = m_blasList[meshName]->m_resource->m_d3dResource->GetGPUVirtualAddress();
				cmdList->m_d3dCmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
				m_blasList[meshName]->m_resource->UavBarrier(cmdList);
			}
		}

		// Mesh is considered double sided if any of the primitives are
		bool bDoubleSided = false;
		for (const auto& primitive : mesh.m_primitives)
		{
			bDoubleSided |= m_materialList[primitive.m_materialIndex].m_doubleSided;
		}

		// Create D3D12_RAYTRACING_INSTANCE_DESC for each mesh
		D3D12_RAYTRACING_INSTANCE_DESC instance = {};
		instance.InstanceID = 0;
		instance.InstanceContributionToHitGroupIndex = 0;
		instance.InstanceMask = 1;
		instance.Flags = bDoubleSided ? D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE : D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
		instance.AccelerationStructure = m_blasList[meshName]->m_resource->m_d3dResource->GetGPUVirtualAddress();

		// Transpose and convert to 3x4 matrix
		const Matrix& localToWorld = m_sceneMeshes.m_transformList[meshIndex];
		decltype(instance.Transform)& dest = instance.Transform;
		dest[0][0] = localToWorld._11;	dest[1][0] = localToWorld._12;	dest[2][0] = localToWorld._13;
		dest[0][1] = localToWorld._21;	dest[1][1] = localToWorld._22;	dest[2][1] = localToWorld._23;
		dest[0][2] = localToWorld._31;	dest[1][2] = localToWorld._32;	dest[2][2] = localToWorld._33;
		dest[0][3] = localToWorld._41;	dest[1][3] = localToWorld._42;	dest[2][3] = localToWorld._43;

		instanceDescs.push_back(instance);
	}

	// Build TLAS
	{
		const size_t instanceDescBufferSize = instanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
		auto instanceDescBuffer = RenderBackend12::CreateUploadBuffer(
			L"instance_descs_buffer",
			instanceDescBufferSize,
			cmdList,
			[pData = instanceDescs.data(), instanceDescBufferSize](uint8_t* pDest)
			{
				memcpy(pDest, pData, instanceDescBufferSize);
			});

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputsDesc = {};
		tlasInputsDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		tlasInputsDesc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		tlasInputsDesc.InstanceDescs = instanceDescBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress();
		tlasInputsDesc.NumDescs = instanceDescs.size();
		tlasInputsDesc.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPreBuildInfo = {};
		RenderBackend12::GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputsDesc, &tlasPreBuildInfo);

		auto tlasScratch = RenderBackend12::CreateBuffer(
			L"tlas_scratch",
			BufferType::AccelerationStructure,
			ResourceAccessMode::GpuWriteOnly,
			ResourceAllocationType::Pooled,
			GetAlignedSize(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, tlasPreBuildInfo.ScratchDataSizeInBytes));

		m_tlas = RenderBackend12::CreateBuffer(
			L"tlas_buffer",
			BufferType::AccelerationStructure,
			ResourceAccessMode::GpuReadWrite,
			ResourceAllocationType::Committed,
			GetAlignedSize(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, tlasPreBuildInfo.ResultDataMaxSizeInBytes));

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
		buildDesc.Inputs = tlasInputsDesc;
		buildDesc.ScratchAccelerationStructureData = tlasScratch->m_resource->m_d3dResource->GetGPUVirtualAddress();
		buildDesc.DestAccelerationStructureData = m_tlas->m_resource->m_d3dResource->GetGPUVirtualAddress();
		cmdList->m_d3dCmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
		m_tlas->m_resource->UavBarrier(cmdList);
	}

	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
}

void FScene::LoadMaterials(const tinygltf::Model& model)
{
	SCOPED_CPU_EVENT("load_materials", PIX_COLOR_DEFAULT);

	// Load material and initialize CPU-side copy
	m_materialList.resize(model.materials.size());
	
	//concurrency::parallel_for(0, (int)model.materials.size(), [&](int i)
	for(int i = 0; i < model.materials.size(); ++i)
	{
		m_materialList[i] = LoadMaterial(model, i);
	}//);


	const size_t bufferSize = m_materialList.size() * sizeof(FMaterial);
	FResourceUploadContext uploader{ bufferSize };

	m_packedMaterials = RenderBackend12::CreateBuffer(
		L"scene_materials",
		BufferType::Raw,
		ResourceAccessMode::GpuReadOnly,
		ResourceAllocationType::Committed,
		bufferSize,
		false,
		(const uint8_t*)m_materialList.data(),
		&uploader);

	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_materials", D3D12_COMMAND_LIST_TYPE_DIRECT);
	uploader.SubmitUploads(cmdList);
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
}

FMaterial FScene::LoadMaterial(const tinygltf::Model& model, const int materialIndex)
{
	SCOPED_CPU_EVENT("load_material", PIX_COLOR_DEFAULT);

	tinygltf::Material material = model.materials[materialIndex];

	// The occlusion texture is sometimes packed with the metal/roughness texture. This is currently not supported since the filtered normal roughness texture point to the same location as the cached AO texture
	DebugAssert(material.occlusionTexture.index == -1 || (material.occlusionTexture.index != material.pbrMetallicRoughness.metallicRoughnessTexture.index), "Not supported");

	FMaterial mat = {};
	mat.m_emissiveFactor = Vector3{ (float)material.emissiveFactor[0], (float)material.emissiveFactor[1], (float)material.emissiveFactor[2] };
	mat.m_baseColorFactor = Vector3{ (float)material.pbrMetallicRoughness.baseColorFactor[0], (float)material.pbrMetallicRoughness.baseColorFactor[1], (float)material.pbrMetallicRoughness.baseColorFactor[2] };
	mat.m_metallicFactor = (float)material.pbrMetallicRoughness.metallicFactor;
	mat.m_roughnessFactor = (float)material.pbrMetallicRoughness.roughnessFactor;
	mat.m_aoStrength = (float)material.occlusionTexture.strength;
	mat.m_emissiveTextureIndex = material.emissiveTexture.index != -1 ? LoadTexture(model.images[model.textures[material.emissiveTexture.index].source], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_BC3_UNORM_SRGB) : -1;
	mat.m_baseColorTextureIndex = material.pbrMetallicRoughness.baseColorTexture.index != -1 ? LoadTexture(model.images[model.textures[material.pbrMetallicRoughness.baseColorTexture.index].source], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_BC3_UNORM_SRGB) : -1;
	mat.m_aoTextureIndex = material.occlusionTexture.index != -1 ? LoadTexture(model.images[model.textures[material.occlusionTexture.index].source], DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC4_UNORM) : -1;
	mat.m_emissiveSamplerIndex = material.emissiveTexture.index != -1 ? Demo::s_samplerCache.CacheSampler(model.samplers[model.textures[material.emissiveTexture.index].sampler]) : -1;
	mat.m_baseColorSamplerIndex = material.pbrMetallicRoughness.baseColorTexture.index != -1 ? Demo::s_samplerCache.CacheSampler(model.samplers[model.textures[material.pbrMetallicRoughness.baseColorTexture.index].sampler]) : -1;
	mat.m_metallicRoughnessSamplerIndex = material.pbrMetallicRoughness.metallicRoughnessTexture.index != -1 ? Demo::s_samplerCache.CacheSampler(model.samplers[model.textures[material.pbrMetallicRoughness.metallicRoughnessTexture.index].sampler]) : -1;
	mat.m_normalSamplerIndex = material.normalTexture.index != -1 ? Demo::s_samplerCache.CacheSampler(model.samplers[model.textures[material.normalTexture.index].sampler]) : -1;
	mat.m_aoSamplerIndex = material.occlusionTexture.index != -1 ? Demo::s_samplerCache.CacheSampler(model.samplers[model.textures[material.occlusionTexture.index].sampler]) : -1;

	// VMF filtering of Normal-Roughness maps
	if (material.normalTexture.index != -1 && material.pbrMetallicRoughness.metallicRoughnessTexture.index != -1)
	{
		// If a normalmap and roughness map are specified, prefilter together to reduce specular aliasing
		const tinygltf::Image& normalmapImage = model.images[model.textures[material.normalTexture.index].source];
		const tinygltf::Image& metallicRoughnessImage = model.images[model.textures[material.pbrMetallicRoughness.metallicRoughnessTexture.index].source];

		// Skip pre-filtering if a cached version is available, which means that they are already pre-filtered
		if (normalmapImage.image.empty() && metallicRoughnessImage.image.empty())
		{
			mat.m_metallicRoughnessTextureIndex = LoadTexture(metallicRoughnessImage);
			mat.m_normalTextureIndex = LoadTexture(normalmapImage);
		}
		else
		{
			std::tie(mat.m_normalTextureIndex, mat.m_metallicRoughnessTextureIndex) = PrefilterNormalRoughnessTextures(model.images[model.textures[material.normalTexture.index].source], model.images[model.textures[material.pbrMetallicRoughness.metallicRoughnessTexture.index].source]);
		}
	}
	else
	{
		// Otherwise filter individually
		mat.m_metallicRoughnessTextureIndex = material.pbrMetallicRoughness.metallicRoughnessTexture.index != -1 ? LoadTexture(model.images[model.textures[material.pbrMetallicRoughness.metallicRoughnessTexture.index].source], DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_BC5_UNORM) : -1; // Note that this uses a swizzled format to extract the G and B channels for metal/roughness
		mat.m_normalTextureIndex = material.normalTexture.index != -1 ? LoadTexture(model.images[model.textures[material.normalTexture.index].source], DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC5_SNORM) : -1;
	}

	// ## TRANSMISSION ##
	mat.m_transmissionFactor = 0.f;
	mat.m_transmissionTextureIndex = -1;
	mat.m_transmissionSamplerIndex = -1;
	auto transmissionIt = material.extensions.find("KHR_materials_transmission");
	if (transmissionIt != material.extensions.cend())
	{
		mat.m_transmissionFactor = transmissionIt->second.Has("transmissionFactor") ? transmissionIt->second.Get("transmissionFactor").GetNumberAsDouble() : 0.f;

		if (transmissionIt->second.Has("transmissionTexture"))
		{
			int texId = transmissionIt->second.Get("transmissionTexture").Get("index").GetNumberAsInt();
			mat.m_transmissionTextureIndex = LoadTexture(model.images[model.textures[texId].source], DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC4_UNORM);
			mat.m_transmissionSamplerIndex = Demo::s_samplerCache.CacheSampler(model.samplers[model.textures[texId].sampler]);
		}
	}

	// ## CLEARCOAT ##
	mat.m_clearcoatFactor = 0.f;
	mat.m_clearcoatRoughnessFactor = 0.f;
	mat.m_clearcoatTextureIndex = -1;
	mat.m_clearcoatRoughnessTextureIndex = -1;
	mat.m_clearcoatNormalTextureIndex = -1;
	mat.m_clearcoatSamplerIndex = -1;
	mat.m_clearcoatRoughnessSamplerIndex = -1;
	mat.m_clearcoatNormalSamplerIndex = -1;
	auto clearcoatIt = material.extensions.find("KHR_materials_clearcoat");
	if (clearcoatIt != material.extensions.cend())
	{
		mat.m_clearcoatFactor = clearcoatIt->second.Has("clearcoatFactor") ? clearcoatIt->second.Get("clearcoatFactor").GetNumberAsDouble() : 0.f;
		mat.m_clearcoatRoughnessFactor = clearcoatIt->second.Has("clearcoatRoughnessFactor") ? clearcoatIt->second.Get("clearcoatRoughnessFactor").GetNumberAsDouble() : 0.f;

		if (clearcoatIt->second.Has("clearcoatTexture"))
		{
			int texId = clearcoatIt->second.Get("clearcoatTexture").Get("index").GetNumberAsInt();
			mat.m_clearcoatTextureIndex = LoadTexture(model.images[model.textures[texId].source], DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC4_UNORM);
			mat.m_clearcoatSamplerIndex = Demo::s_samplerCache.CacheSampler(model.samplers[model.textures[texId].sampler]);
		}

		int clearcoatRoughnessTexId = clearcoatIt->second.Has("clearcoatRoughnessTexture") ? clearcoatIt->second.Get("clearcoatRoughnessTexture").Get("index").GetNumberAsInt() : -1;
		int clearcoatNormalTexId = clearcoatIt->second.Has("clearcoatNormalTexture") ? clearcoatIt->second.Get("clearcoatNormalTexture").Get("index").GetNumberAsInt() : -1;

		// VMF filtering of Clearcoat Normal-Roughness maps
		if (clearcoatNormalTexId != -1 && clearcoatRoughnessTexId != -1)
		{
			// If a normalmap and roughness map are specified, prefilter together to reduce specular aliasing
			const tinygltf::Image& normalmapImage = model.images[model.textures[clearcoatNormalTexId].source];
			const tinygltf::Image& roughnessImage = model.images[model.textures[clearcoatRoughnessTexId].source];

			// Skip pre-filtering if a cached version is available, which means that they are already pre-filtered
			if (normalmapImage.image.empty() && roughnessImage.image.empty())
			{
				mat.m_clearcoatRoughnessTextureIndex = LoadTexture(roughnessImage);
				mat.m_clearcoatNormalTextureIndex = LoadTexture(normalmapImage);
			}
			else
			{
				std::tie(mat.m_clearcoatNormalTextureIndex, mat.m_clearcoatRoughnessTextureIndex) = PrefilterNormalRoughnessTextures(model.images[model.textures[clearcoatNormalTexId].source], model.images[model.textures[clearcoatRoughnessTexId].source]);
			}
		}
		else
		{
			// Otherwise filter individually
			mat.m_clearcoatRoughnessTextureIndex = clearcoatRoughnessTexId != -1 ? LoadTexture(model.images[model.textures[clearcoatRoughnessTexId].source], DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_BC5_UNORM) : -1;
			mat.m_clearcoatNormalTextureIndex = clearcoatNormalTexId != -1 ? LoadTexture(model.images[model.textures[clearcoatNormalTexId].source], DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC5_SNORM) : -1;
		}

		mat.m_clearcoatRoughnessSamplerIndex = clearcoatRoughnessTexId != -1 ? Demo::s_samplerCache.CacheSampler(model.samplers[model.textures[clearcoatRoughnessTexId].sampler]) : -1;
		mat.m_clearcoatNormalSamplerIndex = clearcoatNormalTexId != -1 ? Demo::s_samplerCache.CacheSampler(model.samplers[model.textures[clearcoatNormalTexId].sampler]) : -1;
	}

	// ## BLEND MODE ##
	mat.m_alphaMode = AlphaMode::Opaque;
	if (material.alphaMode == "MASK") 
		mat.m_alphaMode = AlphaMode::Masked;
	else if (material.alphaMode == "BLEND") 
		mat.m_alphaMode = AlphaMode::Blend;

	// ## DOUBLE SIDED ##
	mat.m_doubleSided = material.doubleSided;

	return mat;
}

int FScene::LoadTexture(const tinygltf::Image& image, const DXGI_FORMAT srcFormat, const DXGI_FORMAT compressedFormat)
{
	DebugAssert(!image.uri.empty(), "Embedded image data is not yet supported.");

	if (image.image.empty())
	{
		// Compressed image was found in texture cache
		const std::wstring cachedFilepath = s2ws(image.uri);
		DebugAssert(std::filesystem::exists(cachedFilepath), "File not found in texture cache");

		// Load from cache
		DirectX::TexMetadata metadata;
		DirectX::ScratchImage scratch;
		AssertIfFailed(DirectX::LoadFromDDSFile(cachedFilepath.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, scratch));

		// Upload
		std::wstring name = std::filesystem::path{ cachedFilepath }.filename().wstring();
		FResourceUploadContext uploader{ RenderBackend12::GetResourceSize(scratch) };
		uint32_t bindlessIndex = Demo::s_textureCache.CacheTexture2D(
			&uploader,
			name,
			metadata.format,
			metadata.width,
			metadata.height,
			scratch.GetImages(),
			scratch.GetImageCount());

		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_texture", D3D12_COMMAND_LIST_TYPE_DIRECT);
		uploader.SubmitUploads(cmdList);
		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
		return bindlessIndex;
	}
	else
	{
		DebugAssert(image.pixel_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE && image.component == 4, "Source Images are always 4 channel 8bpp");

		// Source image
		size_t bpp = (image.bits * image.component) / 8;
		DirectX::Image srcImage = {};
		srcImage.width = image.width;
		srcImage.height = image.height;
		srcImage.format = srcFormat;
		srcImage.rowPitch = bpp * image.width;
		srcImage.slicePitch = srcImage.rowPitch * image.height;
		srcImage.pixels = (uint8_t*)image.image.data();

		// Calculate mips upto 4x4 for block compression
		int numMips = 0;
		size_t width = image.width, height = image.height;
		while (width >= 4 && height >= 4)
		{
			numMips++;
			width = width >> 1;
			height = height >> 1;
		}

		// Generate mips
		DirectX::ScratchImage mipchain = {};
		if (SUCCEEDED((DirectX::GenerateMipMaps(srcImage, DirectX::TEX_FILTER_LINEAR, numMips, mipchain))))
		{
			// Block compression
			DirectX::ScratchImage compressedScratch;
			AssertIfFailed(DirectX::Compress(mipchain.GetImages(), numMips, mipchain.GetMetadata(), compressedFormat, DirectX::TEX_COMPRESS_PARALLEL, DirectX::TEX_THRESHOLD_DEFAULT, compressedScratch));

			// Save to disk
			if (Demo::s_globalConfig.UseContentCache)
			{
				std::filesystem::path dirPath{ m_textureCachePath };
				std::filesystem::path srcFilename{ image.uri };
				std::filesystem::path destFilename = dirPath / srcFilename.stem();
				destFilename += std::filesystem::path{ ".dds" };
				DirectX::TexMetadata compressedMetadata = compressedScratch.GetMetadata();
				AssertIfFailed(DirectX::SaveToDDSFile(compressedScratch.GetImages(), compressedScratch.GetImageCount(), compressedMetadata, DirectX::DDS_FLAGS_NONE, destFilename.wstring().c_str()));
			}

			std::wstring name{ image.uri.begin(), image.uri.end() };
			FResourceUploadContext uploader{ RenderBackend12::GetResourceSize(compressedScratch) };
			uint32_t bindlessIndex = Demo::s_textureCache.CacheTexture2D(
				&uploader,
				name,
				compressedFormat,
				srcImage.width,
				srcImage.height,
				compressedScratch.GetImages(),
				compressedScratch.GetImageCount());

			FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_texture", D3D12_COMMAND_LIST_TYPE_DIRECT);
			uploader.SubmitUploads(cmdList);
			RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
			return bindlessIndex;
		}
		else
		{
			// No mips and no compression. Don't save to cache and load directly
			DirectX::ScratchImage scratch;
			scratch.InitializeFromImage(srcImage);
			std::wstring name{ image.uri.begin(), image.uri.end() };
			FResourceUploadContext uploader{ RenderBackend12::GetResourceSize(scratch) };
			uint32_t bindlessIndex = Demo::s_textureCache.CacheTexture2D(
				&uploader,
				name,
				compressedFormat,
				srcImage.width,
				srcImage.height,
				&srcImage,
				1);

			FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_texture", D3D12_COMMAND_LIST_TYPE_DIRECT);
			uploader.SubmitUploads(cmdList);
			RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
			return bindlessIndex;
		}
	}
}

std::pair<int, int> FScene::PrefilterNormalRoughnessTextures(const tinygltf::Image& normalmap, const tinygltf::Image& metallicRoughnessmap)
{
	// Output compression format to use
	const DXGI_FORMAT normalmapCompressionFormat = DXGI_FORMAT_BC5_SNORM;
	const DXGI_FORMAT metalRoughnessCompressionFormat = DXGI_FORMAT_BC5_UNORM;

	// Source normal image data
	DebugAssert(normalmap.pixel_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE && normalmap.component == 4, "Source Images are always 4 channel 8bpp");
	size_t bpp = (normalmap.bits * normalmap.component) / 8;
	DirectX::Image normalmapImage = {};
	normalmapImage.width = normalmap.width;
	normalmapImage.height = normalmap.height;
	normalmapImage.format = DXGI_FORMAT_R8G8B8A8_UNORM;
	normalmapImage.rowPitch = bpp * normalmap.width;
	normalmapImage.slicePitch = normalmapImage.rowPitch * normalmap.height;
	normalmapImage.pixels = (uint8_t*)normalmap.image.data();

	DirectX::ScratchImage normalScratch;
	normalScratch.InitializeFromImage(normalmapImage);

	// Source metallic roughness image data
	DebugAssert(metallicRoughnessmap.pixel_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE && metallicRoughnessmap.component == 4, "Source Images are always 4 channel 8bpp");
	bpp = (metallicRoughnessmap.bits * metallicRoughnessmap.component) / 8;
	DirectX::Image metallicRoughnessImage = {};
	metallicRoughnessImage.width = metallicRoughnessmap.width;
	metallicRoughnessImage.height = metallicRoughnessmap.height;
	metallicRoughnessImage.format = DXGI_FORMAT_R8G8B8A8_UNORM;
	metallicRoughnessImage.rowPitch = bpp * metallicRoughnessmap.width;
	metallicRoughnessImage.slicePitch = metallicRoughnessImage.rowPitch * metallicRoughnessmap.height;
	metallicRoughnessImage.pixels = (uint8_t*)metallicRoughnessmap.image.data();

	DirectX::ScratchImage metallicRoughnessScratch;
	metallicRoughnessScratch.InitializeFromImage(metallicRoughnessImage);

	// Create source textures
	const size_t uploadSize = RenderBackend12::GetResourceSize(normalScratch) + RenderBackend12::GetResourceSize(metallicRoughnessScratch);
	FResourceUploadContext uploader{ uploadSize };
	auto srcNormalmap = RenderBackend12::CreateTexture(L"src_normalmap", TextureType::Tex2D, normalmapImage.format, normalmapImage.width, normalmapImage.height, 1, 1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &normalmapImage, &uploader);
	auto srcMetallicRoughnessmap = RenderBackend12::CreateTexture(L"src_metallic_roughness", TextureType::Tex2D, metallicRoughnessImage.format, metallicRoughnessImage.width, metallicRoughnessImage.height, 1, 1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &metallicRoughnessImage, &uploader);

	// Create UAVs for prefiltering
	size_t normalmapMipCount = RenderUtils12::CalcMipCount(normalmapImage.width, normalmapImage.height, true);
	size_t metallicRoughnessMipCount = RenderUtils12::CalcMipCount(metallicRoughnessImage.width, metallicRoughnessImage.height, true);
	auto normalmapFilterUav = RenderBackend12::CreateSurface(L"dest_normalmap", SurfaceType::UAV, normalmapImage.format, normalmapImage.width, normalmapImage.height, normalmapMipCount);
	auto metallicRoughnessFilterUav = RenderBackend12::CreateSurface(L"dest_metallicRoughnessmap", SurfaceType::UAV, metallicRoughnessImage.format, metallicRoughnessImage.width, metallicRoughnessImage.height, metallicRoughnessMipCount);

	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"prefilter_normal_roughness", D3D12_COMMAND_LIST_TYPE_DIRECT);

	D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
	SCOPED_COMMAND_QUEUE_EVENT(cmdList->m_type, "prefilter_normal_roughness", 0);
	uploader.SubmitUploads(cmdList);

	{
		SCOPED_COMMAND_LIST_EVENT(cmdList, "prefilter_normal_roughness", 0);

		// Descriptor Heaps
		D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
		d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

		// Root Signature
		std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
			L"prefilter_normal_roughness_rootsig",
			cmdList,
			FRootsigDesc{ L"content-pipeline/prefilter-normal-roughness.hlsl", L"rootsig", L"rootsig_1_1" });

		d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

		// PSO
		IDxcBlob* csBlob = RenderBackend12::CacheShader({ 
			L"content-pipeline/prefilter-normal-roughness.hlsl",
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

		DebugAssert(normalmapImage.width == metallicRoughnessImage.width && normalmapImage.height == metallicRoughnessImage.height, "Assuming texture dimensions are same for now");

		// Prefilter
		size_t numMips = std::min<size_t>(normalmapMipCount, metallicRoughnessMipCount);
		size_t mipWidth = std::min<size_t>(normalmapImage.width, metallicRoughnessImage.width);
		size_t mipHeight = std::min<size_t>(normalmapImage.height, metallicRoughnessImage.height);
		for (uint32_t mipIndex = 0; mipIndex < numMips; ++mipIndex)
		{
			struct
			{
				uint32_t mipIndex;
				uint32_t textureWidth;
				uint32_t textureHeight;
				uint32_t normalMapTextureIndex;
				uint32_t metallicRoughnessTextureIndex;
				uint32_t normalmapUavIndex;
				uint32_t metallicRoughnessUavIndex;
			} rootConstants = { 
				mipIndex, (uint32_t)normalmapImage.width, (uint32_t)normalmapImage.height, srcNormalmap->m_srvIndex, srcMetallicRoughnessmap->m_srvIndex, normalmapFilterUav->m_uavIndices[mipIndex], metallicRoughnessFilterUav->m_uavIndices[mipIndex]
			};

			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);

			// Dispatch
			size_t threadGroupCountX = std::max<size_t>(std::ceil(mipWidth / 16), 1);
			size_t threadGroupCountY = std::max<size_t>(std::ceil(mipHeight / 16), 1);
			d3dCmdList->Dispatch(threadGroupCountX, threadGroupCountY, 1);

			mipWidth = mipWidth >> 1;
			mipHeight = mipHeight >> 1;
		}
	}

	// Transition to COMMON because we will be doing a GPU readback after filtering is done on the GPU
	normalmapFilterUav->m_resource->Transition(cmdList, normalmapFilterUav->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON);
	metallicRoughnessFilterUav->m_resource->Transition(cmdList, metallicRoughnessFilterUav->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON);

	// Execute CL
	FFenceMarker fenceMarker = RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

	// Initialize destination textures where the filtered results will be copied
	int normalmapSrvIndex = (int) Demo::s_textureCache.CacheEmptyTexture2D(s2ws(normalmap.uri), normalmapCompressionFormat, normalmap.width, normalmap.height, normalmapMipCount);
	int metalRoughnessSrvIndex = (int) Demo::s_textureCache.CacheEmptyTexture2D(s2ws(metallicRoughnessmap.uri), metalRoughnessCompressionFormat, metallicRoughnessmap.width, metallicRoughnessmap.height, metallicRoughnessMipCount);

	// Copy back normal texture and compress
	auto normalmapReadbackContext = std::make_shared<FResourceReadbackContext>(normalmapFilterUav->m_resource);
	FFenceMarker normalmapStageCompleteMarker = normalmapReadbackContext->StageSubresources(fenceMarker);
	auto normalmapProcessingJob = concurrency::create_task([normalmapStageCompleteMarker, this]()
	{
		normalmapStageCompleteMarker.BlockingWait();
	}).then([
		normalmapReadbackContext, 
		width = normalmap.width, 
		height = normalmap.height,
		filename = normalmap.uri, 
		mipCount = normalmapMipCount, 
		compressionFmt = normalmapCompressionFormat, 
		bpp = (normalmap.bits * normalmap.component) / 8,
		this]
		()
		{
			ProcessReadbackTexture(normalmapReadbackContext.get(), filename, width, height, mipCount, compressionFmt, bpp);
		});

	// Copy back metallic-roughness texture and compress
	auto metallicRoughnessReadbackContext = std::make_shared<FResourceReadbackContext>(metallicRoughnessFilterUav->m_resource);
	FFenceMarker metallicRoughnessStageCompleteMarker = metallicRoughnessReadbackContext->StageSubresources(fenceMarker);
	auto metallicRoughnessProcessingJob = concurrency::create_task([metallicRoughnessStageCompleteMarker, this]()
	{
		metallicRoughnessStageCompleteMarker.BlockingWait();
	}).then([
		metallicRoughnessReadbackContext,
		width = metallicRoughnessmap.width,
		height = metallicRoughnessmap.height,
		filename = metallicRoughnessmap.uri,
		mipCount = metallicRoughnessMipCount,
		compressionFmt = metalRoughnessCompressionFormat,
		bpp = (metallicRoughnessmap.bits * metallicRoughnessmap.component) / 8,
		this]
		()
		{
			ProcessReadbackTexture(metallicRoughnessReadbackContext.get(), filename, width, height, mipCount, compressionFmt, bpp);
		});

	m_loadingJobs.push_back(normalmapProcessingJob);
	m_loadingJobs.push_back(metallicRoughnessProcessingJob);

	return std::make_pair(normalmapSrvIndex, metalRoughnessSrvIndex);
}

void FScene::ProcessReadbackTexture(FResourceReadbackContext* context, const std::string& filename, const int width, const int height, const size_t mipCount, const DXGI_FORMAT fmt, const int bpp)
{
	std::vector<DirectX::Image> mipchain(mipCount);

	for (int i = 0; i < mipchain.size(); ++i)
	{
		D3D12_SUBRESOURCE_DATA data = context->GetTextureData(i);
		DirectX::Image& mip = mipchain[i];
		mip.width = width >> i;
		mip.height = height >> i;
		mip.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		mip.rowPitch = data.RowPitch;
		mip.slicePitch = data.SlicePitch;
		mip.pixels = (uint8_t*)data.pData;
	}

	// Block compression
	DirectX::ScratchImage compressedScratch;
	DirectX::TexMetadata metadata = {
		.width = (size_t)width,
		.height = (size_t)height,
		.depth = 1,
		.arraySize = 1,
		.mipLevels = mipchain.size(),
		.format = DXGI_FORMAT_R8G8B8A8_UNORM,
		.dimension = DirectX::TEX_DIMENSION_TEXTURE2D };
	AssertIfFailed(DirectX::Compress(mipchain.data(), mipchain.size(), metadata, fmt, DirectX::TEX_COMPRESS_PARALLEL, DirectX::TEX_THRESHOLD_DEFAULT, compressedScratch));

	// Save to disk
	if (Demo::s_globalConfig.UseContentCache)
	{
		std::filesystem::path dirPath{ m_textureCachePath };
		std::filesystem::path srcFilename{ filename };
		std::filesystem::path destFilename = dirPath / srcFilename.stem();
		destFilename += std::filesystem::path{ ".dds" };
		DirectX::TexMetadata compressedMetadata = compressedScratch.GetMetadata();
		AssertIfFailed(DirectX::SaveToDDSFile(compressedScratch.GetImages(), compressedScratch.GetImageCount(), compressedMetadata, DirectX::DDS_FLAGS_NONE, destFilename.wstring().c_str()));
	}

	// Upload texture data
	FResourceUploadContext uploader{ RenderBackend12::GetResourceSize(compressedScratch) };
	const DirectX::Image* images = compressedScratch.GetImages();

	std::vector<D3D12_SUBRESOURCE_DATA> srcData(compressedScratch.GetImageCount());
	for (int mipIndex = 0; mipIndex < srcData.size(); ++mipIndex)
	{
		srcData[mipIndex].pData = images[mipIndex].pixels;
		srcData[mipIndex].RowPitch = images[mipIndex].rowPitch;
		srcData[mipIndex].SlicePitch = images[mipIndex].slicePitch;
	}

	FResource* texResource = Demo::s_textureCache.m_cachedTextures[s2ws(filename)]->m_resource;

	uploader.UpdateSubresources(
		texResource,
		srcData,
		[texResource](FCommandList* cmdList)
		{
			texResource->Transition(cmdList, texResource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		});

	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_texture", D3D12_COMMAND_LIST_TYPE_DIRECT);
	uploader.SubmitUploads(cmdList);
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
}

void FScene::LoadCamera(int cameraIndex, const tinygltf::Model& model, const Matrix& transform)
{
	FCamera newCamera = {};
	newCamera.m_viewTransform = transform;
	
	if (model.cameras[cameraIndex].type == "perspective")
	{
		std::stringstream s;
		s << "perspective_cam_" << m_cameras.size();
		newCamera.m_name = s.str();

		const tinygltf::PerspectiveCamera& cam = model.cameras[cameraIndex].perspective;
		newCamera.m_projectionTransform = GetReverseZInfinitePerspectiveFovLH(cam.yfov, cam.aspectRatio, Demo::s_globalConfig.CameraNearPlane);
	}
	else
	{
		std::stringstream s;
		s << "ortho_cam_" << m_cameras.size();
		newCamera.m_name = s.str();

		const tinygltf::OrthographicCamera& cam = model.cameras[cameraIndex].orthographic;
		newCamera.m_projectionTransform = Matrix::CreateOrthographic(cam.xmag, cam.ymag, cam.znear, cam.zfar);
	}

	m_cameras.push_back(newCamera);
}

void FScene::LoadLights(const tinygltf::Model& model)
{
	SCOPED_CPU_EVENT("load_lights", PIX_COLOR_DEFAULT);

	// Load lights and initialize CPU-side copy
	m_globalLightList.resize(model.lights.size());

	concurrency::parallel_for(0, (int)model.lights .size(), [&](int i)
	{
		const tinygltf::Light& light = model.lights[i];
		m_globalLightList[i].m_color = Vector3(light.color[0], light.color[1], light.color[2]);
		m_globalLightList[i].m_intensity = light.intensity;
		m_globalLightList[i].m_range = light.range;
		m_globalLightList[i].m_spotAngles = Vector2(light.spot.innerConeAngle, light.spot.outerConeAngle);

		if (light.type == "directional")
			m_globalLightList[i].m_type = Light::Directional;
		else if (light.type == "point")
			m_globalLightList[i].m_type = Light::Point;
		else if (light.type == "spot")
			m_globalLightList[i].m_type = Light::Spot;
	});

	if (!m_globalLightList.empty())
	{
		const size_t bufferSize = m_globalLightList.size() * sizeof(FLight);
		FResourceUploadContext uploader{ bufferSize };

		m_packedGlobalLightProperties = RenderBackend12::CreateBuffer(
			L"global_light_properties",
			BufferType::Raw,
			ResourceAccessMode::GpuReadOnly,
			ResourceAllocationType::Committed,
			bufferSize,
			false,
			(const uint8_t*)m_globalLightList.data(),
			&uploader);

		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_lights", D3D12_COMMAND_LIST_TYPE_DIRECT);
		uploader.SubmitUploads(cmdList);
		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
	}
}

void FScene::Clear()
{
	m_primitiveCount = 0;

	m_cameras.clear();
	m_meshBuffers.clear();
	m_blasList.clear();
	m_materialList.clear();

	m_packedMeshBufferViews.reset(nullptr);
	m_packedMeshAccessors.reset(nullptr);
	m_packedMaterials.reset(nullptr);
	m_tlas.reset(nullptr);
}

int FScene::GetDirectionalLight() const
{
	auto search = std::find_if(m_sceneLights.m_entityList.cbegin(), m_sceneLights.m_entityList.cend(),
		[this](const int lightIndex)
		{
			const FLight& light = m_globalLightList[lightIndex];
			return light.m_type == Light::Directional;
		});

	return search != m_sceneLights.m_entityList.cend() ? *search : -1;
}

size_t FScene::GetPunctualLightCount() const
{
	size_t count = 0;
	for (const int lightIndex : m_sceneLights.m_entityList)
	{
		const FLight& light = m_globalLightList[lightIndex];
		count += light.m_type == Light::Directional ? 0 : 1;
	}

	return count;
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
		m_position += Demo::s_globalConfig.CameraSpeed * deltaTime * m_look;
		updateView = true;
	}
	else if (controller->MoveBack())
	{
		m_position -= Demo::s_globalConfig.CameraSpeed * deltaTime * m_look;
		updateView = true;
	}

	// Strafe
	if (controller->StrafeLeft())
	{
		m_position -= Demo::s_globalConfig.CameraSpeed * deltaTime * m_right;
		updateView = true;
	}
	else if (controller->StrafeRight())
	{
		m_position += Demo::s_globalConfig.CameraSpeed * deltaTime * m_right;
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

	if (m_fov != Demo::s_globalConfig.Fov)
	{
		m_fov = Demo::s_globalConfig.Fov;
		m_projectionTransform = GetReverseZInfinitePerspectiveFovLH(m_fov, Demo::s_aspectRatio, 1.f);
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

		m_fov = Demo::s_globalConfig.Fov;
		m_projectionTransform = GetReverseZInfinitePerspectiveFovLH(m_fov, Demo::s_aspectRatio, 1.f);
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

	Demo::ResetPathtraceAccumulation();
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
		m_cachedTextures[name] = RenderBackend12::CreateTexture(name, TextureType::Tex2D, format, width, height, imageCount, 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, images, uploadContext);
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
	m_cachedTextures[name] = RenderBackend12::CreateTexture(name, TextureType::Tex2D, format, width, height, mipCount, 1, D3D12_RESOURCE_STATE_COPY_DEST);
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

		// Create the equirectangular source texture
		FResourceUploadContext uploadContext{ mipchain.GetPixelsSize() };
		auto srcHdrTex = RenderBackend12::CreateTexture(
			name, TextureType::Tex2D, metadata.format, metadata.width, metadata.height, mipchain.GetImageCount(), 1,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, mipchain.GetImages(), &uploadContext);

		// Compute CL
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"hdr_preprocess", D3D12_COMMAND_LIST_TYPE_DIRECT);

		D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
		SCOPED_COMMAND_QUEUE_EVENT(cmdList->m_type, "hdr_preprocess", 0);
		uploadContext.SubmitUploads(cmdList);

		// ---------------------------------------------------------------------------------------------------------
		// Generate environment cubemap
		// ---------------------------------------------------------------------------------------------------------
		const size_t cubemapSize = metadata.height;
		auto texCubeUav = RenderBackend12::CreateSurface(L"src_cubemap", SurfaceType::UAV, metadata.format, cubemapSize, cubemapSize, numMips, 1, 6);

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "cubemap_gen", 0);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"cubemapgen_rootsig",
				cmdList,
				FRootsigDesc{L"content-pipeline/cubemapgen.hlsl", L"rootsig", L"rootsig_1_1" });

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
				uint32_t mipSize = cubemapSize >> mipIndex;

				struct
				{
					uint32_t mipIndex;
					uint32_t hdrTextureIndex;
					uint32_t cubemapUavIndex;
					uint32_t mipSize;
					float radianceScale;
				} rootConstants = { mipIndex, srcHdrTex->m_srvIndex, texCubeUav->m_uavIndices[mipIndex], (uint32_t)mipSize, 25000.f };

				d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);

				// Dispatch
				size_t threadGroupCount = std::max<size_t>(std::ceil(mipSize / 16), 1);
				d3dCmdList->Dispatch(threadGroupCount, threadGroupCount, 1);
			}
		}

		// ---------------------------------------------------------------------------------------------------------
		// Prefilter Environment map
		// ---------------------------------------------------------------------------------------------------------
		const size_t filteredEnvmapSize = cubemapSize >> 1;
		const int filteredEnvmapMips = numMips - 1;
		auto texFilteredEnvmapUav = RenderBackend12::CreateSurface(L"filtered_envmap", SurfaceType::UAV, metadata.format, filteredEnvmapSize, filteredEnvmapSize, filteredEnvmapMips, 1, 6);
		texCubeUav->m_resource->Transition(cmdList, texCubeUav->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "prefilter_envmap", 0);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"split_sum_rootsig",
				cmdList,
				FRootsigDesc{ L"image-based-lighting/split-sum-approx/prefilter.hlsl", L"rootsig", L"rootsig_1_1" });

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

			for (uint32_t mipIndex = 0; mipIndex < filteredEnvmapMips; ++mipIndex)
			{
				uint32_t mipSize = filteredEnvmapSize >> mipIndex;

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
					} rootConstants = { mipSize, (uint32_t)cubemapSize, faceIndex, texCubeUav->m_srvIndex, texFilteredEnvmapUav->m_uavIndices[mipIndex], 1024, mipIndex / (float)numMips };

					d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);

					// Dispatch
					size_t threadGroupCount = std::max<size_t>(std::ceil(mipSize / 16), 1);
					d3dCmdList->Dispatch(threadGroupCount, threadGroupCount, 1);
				}
			}
		}


		// Copy from UAV to destination cubemap texture
		texFilteredEnvmapUav->m_resource->Transition(cmdList, texFilteredEnvmapUav->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE);
		auto filteredEnvmapTex = RenderBackend12::CreateTexture(envmapTextureName, TextureType::TexCube, metadata.format, filteredEnvmapSize, filteredEnvmapSize, filteredEnvmapMips, 6, D3D12_RESOURCE_STATE_COPY_DEST);
		d3dCmdList->CopyResource(filteredEnvmapTex->m_resource->m_d3dResource, texFilteredEnvmapUav->m_resource->m_d3dResource);
		filteredEnvmapTex->m_resource->Transition(cmdList, filteredEnvmapTex->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		m_cachedTextures[envmapTextureName] = std::move(filteredEnvmapTex);

		// ---------------------------------------------------------------------------------------------------------
		// Project radiance to SH basis
		// ---------------------------------------------------------------------------------------------------------
		constexpr int numCoefficients = 9; 
		constexpr uint32_t srcMipIndex = 2;
		auto shTexureUav0 = RenderBackend12::CreateSurface(L"ShProj_0", SurfaceType::UAV, metadata.format, metadata.width >> srcMipIndex, metadata.height >> srcMipIndex, 1, 1, numCoefficients);

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "SH_projection", 0);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"sh_projection_rootsig",
				cmdList,
				FRootsigDesc{ L"image-based-lighting/spherical-harmonics/projection.hlsl", L"rootsig", L"rootsig_1_1" });

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
				float radianceScale;
			} rootConstants = { srcHdrTex->m_srvIndex, shTexureUav0->m_uavIndices[0], (uint32_t)metadata.width, (uint32_t)metadata.height, srcMipIndex, 25000.f };

			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);

			size_t threadGroupCountX = std::max<size_t>(std::ceil(metadata.width / 16), 1);
			size_t threadGroupCountY = std::max<size_t>(std::ceil(metadata.height / 16), 1);
			d3dCmdList->Dispatch(threadGroupCountX, threadGroupCountY, 1);
		}

		// Each iteration will reduce by 16 x 16 (threadGroupSizeX * threadGroupSizeZ x threadGroupSizeY)
		auto shTexureUav1 = RenderBackend12::CreateSurface(L"ShProj_1", SurfaceType::UAV, metadata.format, (metadata.width >> srcMipIndex) / 16, (metadata.height >> srcMipIndex) / 16, 1, 1, numCoefficients);

		// Ping-pong UAVs
		FShaderSurface* uavs[2] = { shTexureUav0.get(), shTexureUav1.get() };
		int src = 0, dest = 1;

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "SH_integration", 0);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"sh_integration_rootsig",
				cmdList,
				FRootsigDesc{ L"image-based-lighting/spherical-harmonics/integration.hlsl", L"rootsig", L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

			// See https://gpuopen.com/wp-content/uploads/2017/07/GDC2017-Wave-Programming-D3D12-Vulkan.pdf
			const uint32_t laneCount = RenderBackend12::GetLaneCount();
			uint32_t threadGroupSizeX = 4 / (64 / laneCount);
			uint32_t threadGroupSizeY = 16;
			uint32_t threadGroupSizeZ = 4 * (64 / laneCount);
			std::wstringstream s;
			s << "THREAD_GROUP_SIZE_X=" << threadGroupSizeX <<
				" THREAD_GROUP_SIZE_Y=" << threadGroupSizeY <<
				" THREAD_GROUP_SIZE_Z=" << threadGroupSizeZ;

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({ 
				L"image-based-lighting/spherical-harmonics/integration.hlsl", 
				L"cs_main", 
				s.str() , 
				L"cs_6_6" });

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = rootsig->m_rootsig;
			psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
			psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			D3DPipelineState_t* pso = RenderBackend12::FetchComputePipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			// Dispatch (Reduction)
			width = metadata.width >> srcMipIndex, height = metadata.height >> srcMipIndex;
			uavs[src]->m_resource->UavBarrier(cmdList);

			while (width >= (threadGroupSizeX * threadGroupSizeZ) ||
				height >= threadGroupSizeY)
			{
				struct
				{
					uint32_t srcUavIndex;
					uint32_t destUavIndex;
				} rootConstants = { uavs[src]->m_uavIndices[0], uavs[dest]->m_uavIndices[0] };

				d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);

				// Reduce by 16 x 16 on each iteration
				size_t threadGroupCountX = std::max<size_t>(std::ceil(width / (threadGroupSizeX * threadGroupSizeZ)), 1);
				size_t threadGroupCountY = std::max<size_t>(std::ceil(height / threadGroupSizeY), 1);

				d3dCmdList->Dispatch(threadGroupCountX, threadGroupCountY, 1);

				uavs[dest]->m_resource->UavBarrier(cmdList);

				width = threadGroupCountX;
				height = threadGroupCountY;
				std::swap(src, dest);
			}
		}

		auto shTexureUavAccum = RenderBackend12::CreateSurface(L"ShAccum", SurfaceType::UAV, metadata.format, numCoefficients, 1);

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "SH_accum", 0);

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"sh_accum_rootsig",
				cmdList,
				FRootsigDesc { L"image-based-lighting/spherical-harmonics/accumulation.hlsl", L"rootsig", L"rootsig_1_1" });

			d3dCmdList->SetComputeRootSignature(rootsig->m_rootsig);

			std::wstringstream s;
			s << "THREAD_GROUP_SIZE_X=" << width <<
				" THREAD_GROUP_SIZE_Y=" << height;

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({ 
				L"image-based-lighting/spherical-harmonics/accumulation.hlsl", 
				L"cs_main", 
				s.str() , 
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
				uint32_t srcIndex;
				uint32_t destIndex;
				float normalizationFactor;
			} rootConstants = { uavs[src]->m_uavIndices[0], shTexureUavAccum->m_uavIndices[0], 1.f / (float)((metadata.width >> srcMipIndex) * (metadata.height >> srcMipIndex)) };

			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(rootConstants) / 4, &rootConstants, 0);
			d3dCmdList->Dispatch(1, 1, 1);
		}

		// Copy from UAV to destination texture
		shTexureUavAccum->m_resource->Transition(cmdList, shTexureUavAccum->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE);
		auto shTex = RenderBackend12::CreateTexture(shTextureName, TextureType::Tex2D, metadata.format, numCoefficients, 1, 1, 1, D3D12_RESOURCE_STATE_COPY_DEST);
		d3dCmdList->CopyResource(shTex->m_resource->m_d3dResource, shTexureUavAccum->m_resource->m_d3dResource);
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
		const D3D12_TEXTURE_ADDRESS_MODE addressW = AddressModeConversion(s.wrapR);

		m_cachedSamplers[s] = RenderBackend12::CreateSampler(filter, addressU, addressV, addressW);
		return m_cachedSamplers[s];
	}
}

void FSamplerCache::Clear()
{
	m_cachedSamplers.clear();
}

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														ImGui
//-----------------------------------------------------------------------------------------------------------------------------------------------

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT Demo::WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
}
