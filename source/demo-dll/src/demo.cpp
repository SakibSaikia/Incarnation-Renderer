#include <demo.h>
#include <profiling.h>
#include <backend-d3d12.h>
#include <shadercompiler.h>
#include <renderer.h>
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <common.h>
#include <sstream>
#include <mesh-utils.h>
#include <concurrent_unordered_map.h>
#include <spookyhash_api.h>
#include <ppltasks.h>

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

	concurrency::concurrent_unordered_map<std::wstring, std::unique_ptr<FBindlessShaderResource>> m_cachedTextures;
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
	FScene s_scene;
	FView s_view;
	FController s_controller;
	FTextureCache s_textureCache;
	FSamplerCache s_samplerCache;
	float s_aspectRatio;
	std::unique_ptr<FBindlessShaderResource> s_envBRDF;
	bool s_pauseRendering = false;

	std::vector<std::wstring> s_modelList;
	std::vector<std::wstring> s_hdriList;

	bool IsRenderingPaused()
	{
		return s_pauseRendering;
	}

	const FScene* GetScene()
	{
		return &s_scene;
	}

	const FView* GetView()
	{
		return &s_view;
	}

	uint32_t GetEnvBrdfSrvIndex()
	{
		return RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::Texture2D, s_envBRDF->m_srvIndex);
	}

	void UpdateUI(float deltaTime);
}

class ScopedPauseRendering
{
public:
	ScopedPauseRendering()
	{
		Demo::s_pauseRendering = true;
		RenderBackend12::FlushGPU();
	}

	~ScopedPauseRendering()
	{
		RenderBackend12::FlushGPU();
		Demo::s_pauseRendering = false;
	}
};

#define SCOPED_PAUSE_RENDERING ScopedPauseRendering temp

bool Demo::Initialize(const HWND& windowHandle, const uint32_t resX, const uint32_t resY)
{
	s_aspectRatio = resX / (float)resY;

	bool ok = RenderBackend12::Initialize(windowHandle, resX, resY);
	ok = ok && ShaderCompiler::Initialize();

	s_envBRDF = GenerateEnvBrdfTexture(512, 512);

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

	// Reload scene model if required
	if (s_scene.m_modelFilename.empty() ||
		s_scene.m_modelFilename != Config::g_modelFilename)
	{
		SCOPED_PAUSE_RENDERING;
		s_scene.ReloadModel(Config::g_modelFilename);
		s_view.Reset(&s_scene);
	}

	// Reload scene environment if required
	if (s_scene.m_environmentFilename.empty() ||
		s_scene.m_environmentFilename != Config::g_environmentFilename)
	{
		SCOPED_PAUSE_RENDERING;
		s_scene.ReloadEnvironment(Config::g_environmentFilename);
	}

	// Tick components
	s_controller.Tick(deltaTime);
	s_view.Tick(deltaTime, &s_controller);

	// Handle scene rotation
	{
		// Mouse rotation but as applied in view space
		static float rotX = 0.f;
		static float rotY = 0.f;
		Matrix rotation = Matrix::Identity;

		rotX -= s_controller.RotateSceneX();
		if (rotX != 0.f)
		{
			rotation *= Matrix::CreateFromAxisAngle(s_view.m_up, rotX);
		}

		rotY -= s_controller.RotateSceneY();
		if (rotY != 0.f)
		{
			rotation *= Matrix::CreateFromAxisAngle(s_view.m_right, rotY);
		}

		// Rotate to view space, apply view space rotation and then rotate back to world space
		s_scene.m_rootTransform = rotation;
	}

	UpdateUI(deltaTime);
}

void Demo::Teardown(HWND& windowHandle)
{
	RenderBackend12::FlushGPU();

	s_scene.Clear();
	s_textureCache.Clear();
	s_samplerCache.Clear();

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

	FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
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

	ImGui::Begin("Menu");
	{
		ImGui::Text("%.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
		ImGui::Spacing();

		// -----------------------------------------------------------------------------------------------------------------------------------------

		if (ImGui::CollapsingHeader("Scene"))
		{
			// Model
			static int curModelIndex = std::find(s_modelList.begin(), s_modelList.end(), Config::g_modelFilename) - s_modelList.begin();
			std::string comboLabel = ws2s(s_modelList[curModelIndex]);
			if (ImGui::BeginCombo("Model", comboLabel.c_str(), ImGuiComboFlags_None))
			{
				for (int n = 0; n < s_modelList.size(); n++)
				{
					const bool bSelected = (curModelIndex == n);
					if (ImGui::Selectable(ws2s(s_modelList[n]).c_str(), bSelected))
					{
						curModelIndex = n;
						Config::g_modelFilename = s_modelList[n];
					}

					if (bSelected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}

				ImGui::EndCombo();
			}

			// Background Environment
			static int curHdriIndex = std::find(s_hdriList.begin(), s_hdriList.end(), Config::g_environmentFilename) - s_hdriList.begin();
			comboLabel = ws2s(s_hdriList[curHdriIndex]);
			if (ImGui::BeginCombo("Environment", comboLabel.c_str(), ImGuiComboFlags_None))
			{
				for (int n = 0; n < s_hdriList.size(); n++)
				{
					const bool bSelected = (curHdriIndex == n);
					if (ImGui::Selectable(ws2s(s_hdriList[n]).c_str(), bSelected))
					{
						curHdriIndex = n;
						Config::g_environmentFilename = s_hdriList[n];
					}

					if (bSelected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}

				ImGui::EndCombo();
			}
		}

		// --------------------------------------------------------------------------------------------------------------------------------------------

		if (ImGui::CollapsingHeader("Camera"))
		{
			ImGui::SliderFloat("Speed", &Config::g_cameraSpeed, 1.0f, 20.0f);

			static float fovDeg = DirectX::XMConvertToDegrees(Config::g_fov);
			ImGui::SliderFloat("FOV", &fovDeg, 0.0f, 140.0f);
			Config::g_fov = DirectX::XMConvertToRadians(fovDeg);

			ImGui::SliderFloat("Exposure", &Config::g_exposure, 1.0f, 20.0f);

			if (ImGui::Button("Reset"))
			{
				s_view.Reset(&Demo::s_scene);
			}
		}

		// --------------------------------------------------------------------------------------------------------------------------------------------

		if (ImGui::CollapsingHeader("Debug"))
		{
			if (ImGui::TreeNode("View Modes"))
			{
				ImGui::Checkbox("Lighting Only", &Config::g_lightingOnlyView);
				ImGui::TreePop();
			}

			if (ImGui::TreeNode("Light Components"))
			{
				ImGui::Checkbox("Direct Lighting", &Config::g_enableDirectLighting);
				ImGui::Checkbox("Diffuse IBL", &Config::g_enableDiffuseIBL);
				ImGui::Checkbox("Specular IBL", &Config::g_enableSpecularIBL);
				ImGui::TreePop();
			}
		}
	}
	ImGui::End();

	ImGui::EndFrame();
	ImGui::Render();
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

	if (Config::g_useContentCache && std::filesystem::exists(destFilename))
	{
		// Skip image data initialization. We will load compressed file from the cache instead
		image->image.clear();
		image->uri = destFilename.string();
		return true;
	}
	else
	{
		// Initialize image data using built-in loader
		return tinygltf::LoadImageData(image, image_idx, err, warn, req_width, req_height, bytes, size, user_data);
	}
}

std::string GetCachePath(const std::string filename, const std::string dirName)
{
	std::filesystem::path filepath { filename };
	std::filesystem::path dir{ dirName };
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
	m_textureCachePath = GetCachePath(modelFilepath, ".texture-cache");
	const char* path = m_textureCachePath.c_str();
	loader.SetImageLoader(&LoadImageCallback, (void*)path);

	// Load from model cache if a cached version exists
	m_modelCachePath = GetCachePath(modelFilepath, ".model-cache");
	std::filesystem::path cachedFilepath = std::filesystem::path{ m_modelCachePath } / std::filesystem::path{ ws2s(filename) };
	if (Config::g_useContentCache && std::filesystem::exists(cachedFilepath))
	{
		modelFilepath = cachedFilepath.string();
	}

	// Load GLTF
	std::string errors, warnings;
	tinygltf::Model model;
	bool ok = loader.LoadASCIIFromFile(&model, &errors, &warnings, modelFilepath);

	if (!warnings.empty())
	{
		printf("Warn: %s\n", warnings.c_str());
	}

	if (!errors.empty())
	{
		printf("Error: %s\n", errors.c_str());
	}

	DebugAssert(ok, "Failed to parse glTF");
	m_modelFilename = filename;

	// Clear previous scene
	Clear();

	// Scratch data 
	size_t maxSize = 0;
	for (const tinygltf::Buffer& buf : model.buffers)
	{
		maxSize += buf.data.size();
	}

	m_scratchIndexBuffer = new uint8_t[maxSize];
	m_scratchPositionBuffer = new uint8_t[maxSize];
	m_scratchUvBuffer = new uint8_t[maxSize];
	m_scratchNormalBuffer = new uint8_t[maxSize];
	m_scratchTangentBuffer = new uint8_t[maxSize];
	m_scratchBitangentBuffer = new uint8_t[maxSize];

	m_scratchIndexBufferOffset = 0;
	m_scratchPositionBufferOffset = 0;
	m_scratchUvBufferOffset = 0;
	m_scratchNormalBufferOffset = 0;
	m_scratchTangentBufferOffset = 0;
	m_scratchBitangentBufferOffset = 0;

	// GlTF uses a right handed coordinate. Use the following root transform to convert it to LH.
	Matrix RH2LH = Matrix
	{
		Vector3{1.f, 0.f , 0.f},
		Vector3{0.f, 1.f , 0.f},
		Vector3{0.f, 0.f, -1.f}
	};

	// Parse GLTF and initialize scene
	// See https://github.com/KhronosGroup/glTF-Tutorials/blob/master/gltfTutorial/gltfTutorial_003_MinimalGltfFile.md
	bool requiresResave = false;
	for (tinygltf::Scene& scene : model.scenes)
	{
		for (const int nodeIndex : scene.nodes)
		{
			requiresResave |= LoadNode(nodeIndex, model, RH2LH);
		}
	}

	// If the model required fixup during load, resave a cached copy so that 
	// subsequent loads are faster.
	if (requiresResave)
	{
		ok = loader.WriteGltfSceneToFile(&model, cachedFilepath.string(), false, false, true, false);
		DebugAssert(ok, "Failed to save cached glTF model");
	}

	// Scene bounds
	std::vector<DirectX::BoundingBox> meshWorldBounds(m_meshBounds.size());
	for (int i = 0; i < m_meshBounds.size(); ++i)
	{
		m_meshBounds[i].Transform(meshWorldBounds[i], m_meshTransforms[i]);
	}

	m_sceneBounds = meshWorldBounds[0];
	for (const auto& bb : meshWorldBounds)
	{
		DirectX::BoundingBox::CreateMerged(m_sceneBounds, m_sceneBounds, bb);
	}

	// Create and upload scene buffers
	// @TODO - Fix arbitrary multiplier
	FResourceUploadContext uploader{ 3 * maxSize }; 
	m_meshIndexBuffer = RenderBackend12::CreateBindlessBuffer(
		L"scene_index_buffer",
		m_scratchIndexBufferOffset,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		m_scratchIndexBuffer,
		&uploader);

	m_meshPositionBuffer = RenderBackend12::CreateBindlessBuffer(
		L"scene_position_buffer",
		m_scratchPositionBufferOffset,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		m_scratchPositionBuffer,
		&uploader);

	m_meshUvBuffer = RenderBackend12::CreateBindlessBuffer(
		L"scene_uv_buffer",
		m_scratchUvBufferOffset,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		m_scratchUvBuffer,
		&uploader);

	m_meshNormalBuffer = RenderBackend12::CreateBindlessBuffer(
		L"scene_normal_buffer",
		m_scratchNormalBufferOffset,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		m_scratchNormalBuffer,
		&uploader);

	if (m_scratchTangentBufferOffset != 0)
	{
		m_meshTangentBuffer = RenderBackend12::CreateBindlessBuffer(
			L"scene_tangent_buffer",
			m_scratchTangentBufferOffset,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			m_scratchTangentBuffer,
			&uploader);
	}

	if (m_scratchBitangentBufferOffset != 0)
	{
		m_meshBitangentBuffer = RenderBackend12::CreateBindlessBuffer(
			L"scene_bitangent_buffer",
			m_scratchBitangentBufferOffset,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			m_scratchBitangentBuffer,
			&uploader);
	}

	FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
	uploader.SubmitUploads(cmdList);
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

	delete[] m_scratchIndexBuffer;
	delete[] m_scratchPositionBuffer;
	delete[] m_scratchUvBuffer;
	delete[] m_scratchNormalBuffer;
	delete[] m_scratchTangentBuffer;
	delete[] m_scratchBitangentBuffer;

	// Wait for all loading jobs to finish
	auto joinTask = concurrency::when_all(std::begin(m_loadingJobs), std::end(m_loadingJobs));
	joinTask.wait();
	m_loadingJobs.clear();
}

void FScene::ReloadEnvironment(const std::wstring& filename)
{
	m_globalLightProbe = Demo::s_textureCache.CacheHDRI(filename);
	m_environmentFilename = filename;
}

bool FScene::LoadNode(int nodeIndex, tinygltf::Model& model, const Matrix& parentTransform)
{
	bool requiresResave = false;
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
		requiresResave |= MeshUtils::CleanupMesh(node.mesh, model);
		LoadMesh(node.mesh, model, nodeTransform * parentTransform);
	}

	for (const int childIndex : node.children)
	{
		requiresResave |= LoadNode(childIndex, model, nodeTransform * parentTransform);
	}

	return requiresResave;
}

void FScene::LoadMesh(int meshIndex, const tinygltf::Model& model, const Matrix& parentTransform)
{
	SCOPED_CPU_EVENT("load_mesh", PIX_COLOR_DEFAULT);

	auto CopyIndexData = [&model](const tinygltf::Accessor& accessor, uint8_t* copyDest) -> size_t
	{
		size_t bytesCopied = 0;
		const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
		size_t dataSize = tinygltf::GetComponentSizeInBytes(accessor.componentType) * tinygltf::GetNumComponentsInType(accessor.type);
		size_t dataStride = accessor.ByteStride(bufferView);

		const uint8_t* pSrc = &model.buffers[bufferView.buffer].data[bufferView.byteOffset + accessor.byteOffset];
		uint32_t* indexDest = (uint32_t*)copyDest;

		if (dataSize == sizeof(uint16_t))
		{
			// 16-bit indices
			auto pSrc = (const uint16_t*)&model.buffers[bufferView.buffer].data[bufferView.byteOffset + accessor.byteOffset];
			uint32_t* pDest = (uint32_t*)copyDest;

			for (int i = 0; i < accessor.count; ++i)
			{
				*(pDest++) = *(pSrc++);
				bytesCopied += sizeof(uint32_t);
			}
		}
		else
		{
			// 32-bit indices
			DebugAssert(dataSize == sizeof(uint32_t));
			auto pSrc = (const uint32_t*)&model.buffers[bufferView.buffer].data[bufferView.byteOffset + accessor.byteOffset];
			uint32_t* pDest = (uint32_t*)copyDest;

			for (int i = 0; i < accessor.count; ++i)
			{
				*(pDest++) = *(pSrc++);
				bytesCopied += sizeof(uint32_t);
			}
		}

		return bytesCopied;
	};

	auto CopyBufferData = [&model](const tinygltf::Accessor& accessor, const uint32_t dataSize, uint8_t* copyDest) -> size_t
	{
		size_t bytesCopied = 0;
		const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
		size_t dataStride = accessor.ByteStride(bufferView);

		const uint8_t* pSrc = &model.buffers[bufferView.buffer].data[bufferView.byteOffset + accessor.byteOffset];
		for (int i = 0; i < accessor.count; ++i)
		{
			memcpy(copyDest + i * dataSize, pSrc + i * dataStride, dataSize);
			bytesCopied += dataSize;
		}

		return bytesCopied;
	};

	auto GenerateAndCopyBitangentData = [&model](const tinygltf::Accessor& normalAccessor, const tinygltf::Accessor& tangentAccessor, const uint32_t dataSize, uint8_t* copyDest) -> size_t
	{
		size_t bytesCopied = 0;
		const tinygltf::BufferView& normalBufferView = model.bufferViews[normalAccessor.bufferView];
		const tinygltf::BufferView& tangentBufferView = model.bufferViews[tangentAccessor.bufferView];

		DebugAssert(normalAccessor.ByteStride(normalBufferView) == sizeof(Vector3));
		DebugAssert(tangentAccessor.ByteStride(tangentBufferView) == sizeof(Vector4));
		const Vector3* pNormal = (Vector3*)&model.buffers[normalBufferView.buffer].data[normalBufferView.byteOffset + normalAccessor.byteOffset];
		const Vector4* pTangent = (Vector4*)&model.buffers[tangentBufferView.buffer].data[tangentBufferView.byteOffset + tangentAccessor.byteOffset];

		DebugAssert(normalAccessor.count == tangentAccessor.count);
		size_t numVectors = normalAccessor.count;

		Vector3* pBitangent = (Vector3*)copyDest;
		for (int i = 0; i < normalAccessor.count; ++i)
		{
			*pBitangent = pNormal->Cross(Vector3(pTangent->x, pTangent->y, pTangent->z)) * pTangent->w;

			pNormal++;
			pTangent++;
			pBitangent++;
		}

		return numVectors * sizeof(Vector3);
	};

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


	const tinygltf::Mesh& mesh = model.meshes[meshIndex];

	// Each primitive is a separate render mesh with its own vertex and index buffers
	for (const tinygltf::Primitive& primitive : mesh.primitives)
	{
		// Index data (converted to uint32_t)
		const tinygltf::Accessor& indexAccessor = model.accessors[primitive.indices];
		const size_t indexBytesCopied = CopyIndexData(indexAccessor, m_scratchIndexBuffer + m_scratchIndexBufferOffset);

		// FLOAT3 position data
		auto posIt = primitive.attributes.find("POSITION");
		DebugAssert(posIt != primitive.attributes.cend());
		const tinygltf::Accessor& positionAccessor = model.accessors[posIt->second];
		const size_t positionSize = tinygltf::GetComponentSizeInBytes(positionAccessor.componentType) * tinygltf::GetNumComponentsInType(positionAccessor.type);
		DebugAssert(positionSize == 3 * sizeof(float));
		const size_t positionBytesCopied = CopyBufferData(positionAccessor, positionSize, m_scratchPositionBuffer + m_scratchPositionBufferOffset);

		// FLOAT2 UV data
		auto uvIt = primitive.attributes.find("TEXCOORD_0");
		size_t uvSize = 0;
		size_t uvBytesCopied = 0;
		if (uvIt != primitive.attributes.cend())
		{
			const tinygltf::Accessor& uvAccessor = model.accessors[uvIt->second];
			uvSize = tinygltf::GetComponentSizeInBytes(uvAccessor.componentType) * tinygltf::GetNumComponentsInType(uvAccessor.type);
			DebugAssert(uvSize == 2 * sizeof(float));
			uvBytesCopied = CopyBufferData(uvAccessor, uvSize, m_scratchUvBuffer + m_scratchUvBufferOffset);
		}

		// FLOAT3 normal data
		auto normalIt = primitive.attributes.find("NORMAL");
		DebugAssert(normalIt != primitive.attributes.cend());
		const tinygltf::Accessor& normalAccessor = model.accessors[normalIt->second];
		const size_t normalSize = tinygltf::GetComponentSizeInBytes(normalAccessor.componentType) * tinygltf::GetNumComponentsInType(normalAccessor.type);
		DebugAssert(normalSize == 3 * sizeof(float));
		const size_t normalBytesCopied = CopyBufferData(normalAccessor, normalSize, m_scratchNormalBuffer + m_scratchNormalBufferOffset);

		// FLOAT3 tangent & bitangent data (OPTIONAL)
		size_t tangentBytesCopied = 0;
		size_t bitangentBytesCopied = 0;
		auto tangentIt = primitive.attributes.find("TANGENT");
		if (tangentIt != primitive.attributes.cend())
		{
			const tinygltf::Accessor& tangentAccessor = model.accessors[tangentIt->second];
			const size_t tangentSize = tinygltf::GetComponentSizeInBytes(tangentAccessor.componentType) * tinygltf::GetNumComponentsInType(tangentAccessor.type);
			DebugAssert(tangentSize == 4 * sizeof(float));
			tangentBytesCopied = CopyBufferData(tangentAccessor, 3 * sizeof(float), m_scratchTangentBuffer + m_scratchTangentBufferOffset);
			bitangentBytesCopied = GenerateAndCopyBitangentData(normalAccessor, tangentAccessor, 3 * sizeof(float), m_scratchBitangentBuffer + m_scratchBitangentBufferOffset);
		}

		FRenderMesh newMesh = {};
		newMesh.m_name = mesh.name;
		newMesh.m_indexOffset = m_scratchIndexBufferOffset / sizeof(uint32_t);
		newMesh.m_positionOffset = m_scratchPositionBufferOffset / positionSize;
		newMesh.m_uvOffset = uvBytesCopied > 0 ? m_scratchUvBufferOffset / uvSize : 0;
		newMesh.m_normalOffset = m_scratchNormalBufferOffset / normalSize;
		newMesh.m_tangentOffset = tangentBytesCopied > 0 ? m_scratchTangentBufferOffset / normalSize : 0;
		newMesh.m_bitangentOffset = bitangentBytesCopied > 0 ? m_scratchBitangentBufferOffset / normalSize : 0;
		newMesh.m_indexCount = indexAccessor.count;
		newMesh.m_material = LoadMaterial(model, primitive.material);
		m_meshGeo.push_back(newMesh);
		m_meshTransforms.push_back(parentTransform);

		m_scratchIndexBufferOffset += indexBytesCopied;
		m_scratchPositionBufferOffset += positionBytesCopied;
		m_scratchUvBufferOffset += uvBytesCopied;
		m_scratchNormalBufferOffset += normalBytesCopied;
		m_scratchTangentBufferOffset += tangentBytesCopied;
		m_scratchBitangentBufferOffset += bitangentBytesCopied;

		m_meshBounds.push_back(CalcBounds(posIt->second));
	}
}

FMaterial FScene::LoadMaterial(const tinygltf::Model& model, const int materialIndex)
{
	SCOPED_CPU_EVENT("load_material", PIX_COLOR_DEFAULT);

	tinygltf::Material material = model.materials[materialIndex];

	// The occlusion texture is sometimes packed with the metal/roughness texture. This is currently not supported since the filtered normal roughness texture point to the same location as the cached AO texture
	DebugAssert(material.occlusionTexture.index == -1 || (material.occlusionTexture.index != material.pbrMetallicRoughness.metallicRoughnessTexture.index), "Not supported");

	FMaterial mat = {};
	mat.m_materialName = material.name;
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

	// If a normalmap and roughness map are specified, prefilter to reduce specular aliasing
	if (material.normalTexture.index != -1 && material.pbrMetallicRoughness.metallicRoughnessTexture.index != -1)
	{
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
		mat.m_metallicRoughnessTextureIndex = material.pbrMetallicRoughness.metallicRoughnessTexture.index != -1 ? LoadTexture(model.images[model.textures[material.pbrMetallicRoughness.metallicRoughnessTexture.index].source], DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_BC5_UNORM) : -1; // Note that this uses a swizzled format to extract the G and B channels for metal/roughness
		mat.m_normalTextureIndex = material.normalTexture.index != -1 ? LoadTexture(model.images[model.textures[material.normalTexture.index].source], DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC5_SNORM) : -1;
	}

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

		FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
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
			std::filesystem::path dirPath{ m_textureCachePath };
			std::filesystem::path srcFilename{ image.uri };
			std::filesystem::path destFilename = dirPath / srcFilename.stem();
			destFilename += std::filesystem::path{ ".dds" };
			DirectX::TexMetadata compressedMetadata = compressedScratch.GetMetadata();
			AssertIfFailed(DirectX::SaveToDDSFile(compressedScratch.GetImages(), compressedScratch.GetImageCount(), compressedMetadata, DirectX::DDS_FLAGS_NONE, destFilename.wstring().c_str()));

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

			FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
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

			FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
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
	auto srcNormalmap = RenderBackend12::CreateBindlessTexture(L"src_normalmap", BindlessResourceType::Texture2D, normalmapImage.format, normalmapImage.width, normalmapImage.height, 1, 1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &normalmapImage, &uploader);
	auto srcMetallicRoughnessmap = RenderBackend12::CreateBindlessTexture(L"src_metallic_roughness", BindlessResourceType::Texture2D, metallicRoughnessImage.format, metallicRoughnessImage.width, metallicRoughnessImage.height, 1, 1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &metallicRoughnessImage, &uploader);

	// Create UAVs for prefiltering
	size_t normalmapMipCount = RenderUtils12::CalcMipCount(normalmapImage.width, normalmapImage.height, true);
	size_t metallicRoughnessMipCount = RenderUtils12::CalcMipCount(metallicRoughnessImage.width, metallicRoughnessImage.height, true);
	auto normalmapFilterUav = RenderBackend12::CreateBindlessUavTexture(L"dest_normalmap", normalmapImage.format, normalmapImage.width, normalmapImage.height, normalmapMipCount, 1);
	auto metallicRoughnessFilterUav = RenderBackend12::CreateBindlessUavTexture(L"dest_metallicRoughnessmap", metallicRoughnessImage.format, metallicRoughnessImage.width, metallicRoughnessImage.height, metallicRoughnessMipCount, 1);

	FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
	cmdList->SetName(L"prefilter_normal_roughness");

	D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
	SCOPED_COMMAND_QUEUE_EVENT(cmdList->m_type, "prefilter_normal_roughness", 0);
	uploader.SubmitUploads(cmdList);

	{
		SCOPED_COMMAND_LIST_EVENT(cmdList, "prefilter_normal_roughness", 0);

		// Root Signature
		winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({ L"prefilter-normal-roughness.hlsl", L"rootsig" });
		d3dCmdList->SetComputeRootSignature(rootsig.get());

		// PSO
		IDxcBlob* csBlob = RenderBackend12::CacheShader({ L"prefilter-normal-roughness.hlsl", L"cs_main", L"THREAD_GROUP_SIZE_X=16 THREAD_GROUP_SIZE_Y=16" }, L"cs_6_6");

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = rootsig.get();
		psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
		psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
		psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

		D3DPipelineState_t* pso = RenderBackend12::FetchComputePipelineState(psoDesc);
		d3dCmdList->SetPipelineState(pso);

		// Shader resources
		D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetBindlessShaderResourceHeap() };
		d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

		DebugAssert(normalmapImage.width == metallicRoughnessImage.width && normalmapImage.height == metallicRoughnessImage.height, "Assuming texture dimensions are same for now");

		// Prefilter
		size_t numMips = std::min<size_t>(normalmapMipCount, metallicRoughnessMipCount);
		size_t mipWidth = std::min<size_t>(normalmapImage.width, metallicRoughnessImage.width);
		size_t mipHeight = std::min<size_t>(normalmapImage.height, metallicRoughnessImage.height);
		for (uint32_t mipIndex = 0; mipIndex < numMips; ++mipIndex)
		{
			struct CbLayout
			{
				uint32_t mipIndex;
				uint32_t textureWidth;
				uint32_t textureHeight;
				uint32_t normalMapTextureIndex;
				uint32_t metallicRoughnessTextureIndex;
				uint32_t normalmapUavIndex;
				uint32_t metallicRoughnessUavIndex;
			};

			CbLayout computeCb =
			{
				.mipIndex = mipIndex,
				.textureWidth = (uint32_t)normalmapImage.width,
				.textureHeight = (uint32_t)normalmapImage.height,
				.normalMapTextureIndex = RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::Texture2D, srcNormalmap->m_srvIndex),
				.metallicRoughnessTextureIndex = RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::Texture2D, srcMetallicRoughnessmap->m_srvIndex),
				.normalmapUavIndex = RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::RWTexture2D, normalmapFilterUav->m_uavIndices[mipIndex]),
				.metallicRoughnessUavIndex = RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::RWTexture2D, metallicRoughnessFilterUav->m_uavIndices[mipIndex]),
			};

			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(CbLayout) / 4, &computeCb, 0);
			d3dCmdList->SetComputeRootDescriptorTable(1, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::Texture2DBegin));
			d3dCmdList->SetComputeRootDescriptorTable(2, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::RWTexture2DBegin));

			// Dispatch
			size_t threadGroupCountX = std::max<size_t>(std::ceil(mipWidth / 16), 1);
			size_t threadGroupCountY = std::max<size_t>(std::ceil(mipHeight / 16), 1);
			d3dCmdList->Dispatch(threadGroupCountX, threadGroupCountY, 1);

			mipWidth = mipWidth >> 1;
			mipHeight = mipHeight >> 1;
		}
	}

	// Transition to COMMON because we will be doing a GPU readback after filtering is done on the GPU
	normalmapFilterUav->Transition(cmdList, normalmapFilterUav->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON);
	metallicRoughnessFilterUav->Transition(cmdList, metallicRoughnessFilterUav->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON);

	// Execute CL
	RenderBackend12::BeginCapture();
	FFenceMarker fenceMarker = RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
	RenderBackend12::EndCapture();

	// Initialize destination textures where the filtered results will be copied
	int normalmapSrvIndex = (int) Demo::s_textureCache.CacheEmptyTexture2D(s2ws(normalmap.uri), normalmapCompressionFormat, normalmap.width, normalmap.height, normalmapMipCount);
	int metalRoughnessSrvIndex = (int) Demo::s_textureCache.CacheEmptyTexture2D(s2ws(metallicRoughnessmap.uri), metalRoughnessCompressionFormat, metallicRoughnessmap.width, metallicRoughnessmap.height, metallicRoughnessMipCount);

	// Copy back normal texture and compress
	auto normalmapReadbackContext = std::make_shared<FResourceReadbackContext>(normalmapFilterUav->m_resource);
	FFenceMarker normalmapStageCompleteMarker = normalmapReadbackContext->StageSubresources(normalmapFilterUav->m_resource, fenceMarker);
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
	FFenceMarker metallicRoughnessStageCompleteMarker = metallicRoughnessReadbackContext->StageSubresources(metallicRoughnessFilterUav->m_resource, fenceMarker);
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
		D3D12_SUBRESOURCE_DATA data = context->GetData(i);
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
	std::filesystem::path dirPath{ m_textureCachePath };
	std::filesystem::path srcFilename{ filename };
	std::filesystem::path destFilename = dirPath / srcFilename.stem();
	destFilename += std::filesystem::path{ ".dds" };
	DirectX::TexMetadata compressedMetadata = compressedScratch.GetMetadata();
	AssertIfFailed(DirectX::SaveToDDSFile(compressedScratch.GetImages(), compressedScratch.GetImageCount(), compressedMetadata, DirectX::DDS_FLAGS_NONE, destFilename.wstring().c_str()));

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

	FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
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
		newCamera.m_projectionTransform = GetReverseZInfinitePerspectiveFovLH(cam.yfov, cam.aspectRatio, cam.znear);
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

void FScene::Clear()
{
	m_cameras.clear();
	m_meshGeo.clear();
	m_meshTransforms.clear();
	m_meshBounds.clear();
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
		m_position += Config::g_cameraSpeed * deltaTime * m_look;
		updateView = true;
	}
	else if (controller->MoveBack())
	{
		m_position -= Config::g_cameraSpeed * deltaTime * m_look;
		updateView = true;
	}

	// Strafe
	if (controller->StrafeLeft())
	{
		m_position -= Config::g_cameraSpeed * deltaTime * m_right;
		updateView = true;
	}
	else if (controller->StrafeRight())
	{
		m_position += Config::g_cameraSpeed * deltaTime * m_right;
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

	if (m_fov != Config::g_fov)
	{
		m_fov = Config::g_fov;
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

		m_fov = Config::g_fov;
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
		return RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::Texture2D, search->second->m_srvIndex);
	}
	else
	{
		m_cachedTextures[name] = RenderBackend12::CreateBindlessTexture(name, BindlessResourceType::Texture2D, format, width, height, imageCount, 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, images, uploadContext);
		return RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::Texture2D, m_cachedTextures[name]->m_srvIndex);
	}
}

uint32_t FTextureCache::CacheEmptyTexture2D(
	const std::wstring& name,
	const DXGI_FORMAT format,
	const int width,
	const int height,
	const size_t mipCount)
{
	m_cachedTextures[name] = RenderBackend12::CreateBindlessTexture(name, BindlessResourceType::Texture2D, format, width, height, mipCount, 1, D3D12_RESOURCE_STATE_COPY_DEST);
	return RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::Texture2D, m_cachedTextures[name]->m_srvIndex);
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
			(int)RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::TextureCube, search0->second->m_srvIndex),
			(int)RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::Texture2D, search1->second->m_srvIndex),
		};
	}
	else
	{
		// Read HDR spehere map from file
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
		auto srcHdrTex = RenderBackend12::CreateBindlessTexture(
			name, BindlessResourceType::Texture2D, metadata.format, metadata.width, metadata.height, mipchain.GetImageCount(), 1,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, mipchain.GetImages(), &uploadContext);

		// Compute CL
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
		cmdList->SetName(L"hdr_preprocess");

		D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
		SCOPED_COMMAND_QUEUE_EVENT(cmdList->m_type, "hdr_preprocess", 0);
		uploadContext.SubmitUploads(cmdList);

		// ---------------------------------------------------------------------------------------------------------
		// Generate environment cubemap
		// ---------------------------------------------------------------------------------------------------------
		const size_t cubemapSize = metadata.height;
		auto texCubeUav = RenderBackend12::CreateBindlessUavTexture(L"src_cubemap", metadata.format, cubemapSize, cubemapSize, numMips, 6);

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "cubemap_gen", 0);

			// Root Signature
			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({ L"cubemapgen.hlsl", L"rootsig" });
			d3dCmdList->SetComputeRootSignature(rootsig.get());

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({ L"cubemapgen.hlsl", L"cs_main", L"THREAD_GROUP_SIZE_X=16 THREAD_GROUP_SIZE_Y=16" }, L"cs_6_6");

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = rootsig.get();
			psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
			psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			D3DPipelineState_t* pso = RenderBackend12::FetchComputePipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			// Shader resources
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetBindlessShaderResourceHeap() };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Convert from sperical map to cube map
			for (uint32_t mipIndex = 0; mipIndex < numMips; ++mipIndex)
			{
				uint32_t mipSize = cubemapSize >> mipIndex;

				struct CbLayout
				{
					uint32_t mipIndex;
					uint32_t hdrTextureIndex;
					uint32_t cubemapUavIndex;
					uint32_t mipSize;
					float radianceScale;
				};

				CbLayout computeCb =
				{
					.mipIndex = mipIndex,
					.hdrTextureIndex = RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::Texture2D, srcHdrTex->m_srvIndex),
					.cubemapUavIndex = RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::RWTexture2DArray, texCubeUav->m_uavIndices[mipIndex]),
					.mipSize = (uint32_t)mipSize,
					.radianceScale = 25000.f
				};

				d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(CbLayout) / 4, &computeCb, 0);
				d3dCmdList->SetComputeRootDescriptorTable(1, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::Texture2DBegin));
				d3dCmdList->SetComputeRootDescriptorTable(2, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::RWTexture2DArrayBegin));

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
		auto texFilteredEnvmapUav = RenderBackend12::CreateBindlessUavTexture(L"filtered_envmap", metadata.format, filteredEnvmapSize, filteredEnvmapSize, filteredEnvmapMips, 6);
		texCubeUav->Transition(cmdList, texCubeUav->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "prefilter_envmap", 0);

			// Root Signature
			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({ L"prefilter.hlsl", L"rootsig" });
			d3dCmdList->SetComputeRootSignature(rootsig.get());

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({ L"prefilter.hlsl", L"cs_main", L"THREAD_GROUP_SIZE_X=16 THREAD_GROUP_SIZE_Y=16" }, L"cs_6_6");

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = rootsig.get();
			psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
			psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			D3DPipelineState_t* pso = RenderBackend12::FetchComputePipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			// Shader resources
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetBindlessShaderResourceHeap() };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			struct CbLayout
			{
				uint32_t mipSize;
				uint32_t cubemapSize;
				uint32_t faceIndex;
				uint32_t envmapSrvIndex;
				uint32_t uavIndex;
				uint32_t sampleCount;
				float roughness;
			};

			for (uint32_t mipIndex = 0; mipIndex < filteredEnvmapMips; ++mipIndex)
			{
				uint32_t mipSize = filteredEnvmapSize >> mipIndex;

				for (uint32_t faceIndex = 0; faceIndex < 6; ++faceIndex)
				{
					CbLayout computeCb =
					{
						.mipSize = mipSize,
						.cubemapSize = (uint32_t)cubemapSize,
						.faceIndex = faceIndex,
						.envmapSrvIndex = RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::TextureCube, texCubeUav->m_srvIndex),
						.uavIndex = RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::RWTexture2DArray, texFilteredEnvmapUav->m_uavIndices[mipIndex]),
						.sampleCount = 1024,
						.roughness = mipIndex / (float)numMips
					};

					d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(CbLayout) / 4, &computeCb, 0);
					d3dCmdList->SetComputeRootDescriptorTable(1, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::TextureCubeBegin));
					d3dCmdList->SetComputeRootDescriptorTable(2, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::RWTexture2DArrayBegin));

					// Dispatch
					size_t threadGroupCount = std::max<size_t>(std::ceil(mipSize / 16), 1);
					d3dCmdList->Dispatch(threadGroupCount, threadGroupCount, 1);
				}
			}
		}


		// Copy from UAV to destination cubemap texture
		texFilteredEnvmapUav->Transition(cmdList, texFilteredEnvmapUav->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE);
		auto filteredEnvmapTex = RenderBackend12::CreateBindlessTexture(envmapTextureName, BindlessResourceType::TextureCube, metadata.format, filteredEnvmapSize, filteredEnvmapSize, filteredEnvmapMips, 6, D3D12_RESOURCE_STATE_COPY_DEST);
		d3dCmdList->CopyResource(filteredEnvmapTex->m_resource->m_d3dResource, texFilteredEnvmapUav->m_resource->m_d3dResource);
		filteredEnvmapTex->Transition(cmdList, filteredEnvmapTex->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_cachedTextures[envmapTextureName] = std::move(filteredEnvmapTex);

		// ---------------------------------------------------------------------------------------------------------
		// Project radiance to SH basis
		// ---------------------------------------------------------------------------------------------------------
		constexpr int numCoefficients = 9; 
		constexpr uint32_t srcMipIndex = 2;
		auto shTexureUav0 = RenderBackend12::CreateBindlessUavTexture(L"ShProj_0", metadata.format, metadata.width >> srcMipIndex, metadata.height >> srcMipIndex, 1, numCoefficients);

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "SH_projection", 0);

			// Root Signature
			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({ L"sh-projection.hlsl", L"rootsig" });
			d3dCmdList->SetComputeRootSignature(rootsig.get());

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({ L"sh-projection.hlsl", L"cs_main", L"THREAD_GROUP_SIZE_X=16 THREAD_GROUP_SIZE_Y=16" }, L"cs_6_6");

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = rootsig.get();
			psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
			psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			D3DPipelineState_t* pso = RenderBackend12::FetchComputePipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			// Shader resources
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetBindlessShaderResourceHeap() };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			struct CbLayout
			{
				uint32_t inputHdriIndex;
				uint32_t outputUavIndex;
				uint32_t hdriWidth;
				uint32_t hdriHeight;
				uint32_t srcMip;
			} cb =
			{
				RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::Texture2D, srcHdrTex->m_srvIndex),
				RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::RWTexture2DArray, shTexureUav0->m_uavIndices[0]),
				(uint32_t)metadata.width,
				(uint32_t)metadata.height,
				srcMipIndex
			};

			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(CbLayout) / 4, &cb, 0);
			d3dCmdList->SetComputeRootDescriptorTable(1, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::Texture2DBegin));
			d3dCmdList->SetComputeRootDescriptorTable(2, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::RWTexture2DArrayBegin));

			size_t threadGroupCountX = std::max<size_t>(std::ceil(metadata.width / 16), 1);
			size_t threadGroupCountY = std::max<size_t>(std::ceil(metadata.height / 16), 1);
			d3dCmdList->Dispatch(threadGroupCountX, threadGroupCountY, 1);
		}

		// Each iteration will reduce by 16 x 16 (threadGroupSizeX * threadGroupSizeZ x threadGroupSizeY)
		auto shTexureUav1 = RenderBackend12::CreateBindlessUavTexture(L"ShProj_1", metadata.format, (metadata.width >> srcMipIndex) / 16, (metadata.height >> srcMipIndex) / 16, 1, numCoefficients);

		// Ping-pong UAVs
		FBindlessUav* uavs[2] = { shTexureUav0.get(), shTexureUav1.get() };
		int src = 0, dest = 1;

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "SH_integration", 0);

			// Root Signature
			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({ L"sh-integration.hlsl", L"rootsig" });
			d3dCmdList->SetComputeRootSignature(rootsig.get());

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
			IDxcBlob* csBlob = RenderBackend12::CacheShader({ L"sh-integration.hlsl", L"cs_main", s.str() }, L"cs_6_6");

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = rootsig.get();
			psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
			psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			D3DPipelineState_t* pso = RenderBackend12::FetchComputePipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			// Shader resources
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetBindlessShaderResourceHeap() };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Dispatch (Reduction)
			width = metadata.width >> srcMipIndex, height = metadata.height >> srcMipIndex;
			uavs[src]->UavBarrier(cmdList);

			while (width >= (threadGroupSizeX * threadGroupSizeZ) ||
				height >= threadGroupSizeY)
			{
				struct CbLayout
				{
					uint32_t srcUavIndex;
					uint32_t destUavIndex;
				} cb =
				{
					RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::RWTexture2DArray, uavs[src]->m_uavIndices[0]),
					RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::RWTexture2DArray, uavs[dest]->m_uavIndices[0])
				};

				d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(CbLayout) / 4, &cb, 0);
				d3dCmdList->SetComputeRootDescriptorTable(1, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::RWTexture2DArrayBegin));

				// Reduce by 16 x 16 on each iteration
				size_t threadGroupCountX = std::max<size_t>(std::ceil(width / (threadGroupSizeX * threadGroupSizeZ)), 1);
				size_t threadGroupCountY = std::max<size_t>(std::ceil(height / threadGroupSizeY), 1);

				d3dCmdList->Dispatch(threadGroupCountX, threadGroupCountY, 1);

				uavs[dest]->UavBarrier(cmdList);

				width = threadGroupCountX;
				height = threadGroupCountY;
				std::swap(src, dest);
			}
		}

		auto shTexureUavAccum = RenderBackend12::CreateBindlessUavTexture(L"ShAccum", metadata.format, numCoefficients, 1, 1, 1);

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "SH_accum", 0);

			// Root Signature
			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({ L"sh-accumulation.hlsl", L"rootsig" });
			d3dCmdList->SetComputeRootSignature(rootsig.get());

			std::wstringstream s;
			s << "THREAD_GROUP_SIZE_X=" << width <<
				" THREAD_GROUP_SIZE_Y=" << height;

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({ L"sh-accumulation.hlsl", L"cs_main", s.str() }, L"cs_6_6");

			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.pRootSignature = rootsig.get();
			psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
			psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			D3DPipelineState_t* pso = RenderBackend12::FetchComputePipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			// Shader resources
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetBindlessShaderResourceHeap() };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			struct CbLayout
			{
				uint32_t srcIndex;
				uint32_t destIndex;
				float normalizationFactor;
			} cb =
			{
				RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::RWTexture2DArray, uavs[src]->m_uavIndices[0]),
				RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::RWTexture2D, shTexureUavAccum->m_uavIndices[0]),
				1.f / (float)((metadata.width >> srcMipIndex) * (metadata.height >> srcMipIndex))
			};

			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(CbLayout) / 4, &cb, 0);
			d3dCmdList->SetComputeRootDescriptorTable(1, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::RWTexture2DBegin));
			d3dCmdList->SetComputeRootDescriptorTable(2, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::RWTexture2DArrayBegin));
			d3dCmdList->Dispatch(1, 1, 1);
		}

		// Copy from UAV to destination texture
		shTexureUavAccum->Transition(cmdList, shTexureUavAccum->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE);
		auto shTex = RenderBackend12::CreateBindlessTexture(shTextureName, BindlessResourceType::Texture2D, metadata.format, numCoefficients, 1, 1, 1, D3D12_RESOURCE_STATE_COPY_DEST);
		d3dCmdList->CopyResource(shTex->m_resource->m_d3dResource, shTexureUavAccum->m_resource->m_d3dResource);
		shTex->Transition(cmdList, shTex->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_cachedTextures[shTextureName] = std::move(shTex);

		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

		return FLightProbe{
			(int)RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::TextureCube, m_cachedTextures[envmapTextureName]->m_srvIndex),
			(int)RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::Texture2D, m_cachedTextures[shTextureName]->m_srvIndex),
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

		m_cachedSamplers[s] = RenderBackend12::CreateBindlessSampler(filter, addressU, addressV, addressW);
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
