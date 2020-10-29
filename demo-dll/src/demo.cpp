#include <demo.h>
#include <profiling.h>
#include <backend-d3d12.h>
#include <shadercompiler.h>
#include <renderer.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <common.h>
#include <sstream>
#include <tiny_gltf.h>
#include <concurrent_unordered_map.h>

struct FTextureCache
{
	uint32_t CacheTexture(const std::wstring& name, FResourceUploadContext* uploadContext);
	void Clear();

	concurrency::concurrent_unordered_map<std::wstring, std::unique_ptr<FBindlessResource>> m_cachedTextures;
};

namespace Demo
{
	std::string s_loadedScenePath = {};
	FScene s_scene;
	FView s_view;
	FTextureCache s_textureCache;

	// Mouse
	WPARAM s_mouseButtonState = {};
	POINT s_currentMousePos = { 0, 0 };
	POINT s_lastMousePos = { 0, 0 };
}

void FScene::Reload(const char* filePath)
{
	tinygltf::TinyGLTF loader;
	std::string errors, warnings;

	// Load GLTF
	tinygltf::Model model;
	bool ok = loader.LoadASCIIFromFile(&model, &errors, &warnings, Settings::k_scenePath);

	if (!warnings.empty())
	{
		printf("Warn: %s\n", warnings.c_str());
	}

	if (!errors.empty())
	{
		printf("Error: %s\n", errors.c_str());
	}

	DebugAssert(ok, "Failed to parse glTF");

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

	// Parse GLTF and initialize scene
	for (const tinygltf::Scene& scene : model.scenes)
	{
		for (const int nodeIndex : scene.nodes)
		{
			LoadNode(nodeIndex, model, Matrix::Identity);
		}
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
	FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
	FResourceUploadContext uploader{ maxSize };
	m_meshIndexBuffer = RenderBackend12::CreateBindlessByteAddressBuffer(
		L"scene_index_buffer",
		m_scratchIndexBufferOffset,
		m_scratchIndexBuffer,
		&uploader);

	m_meshPositionBuffer = RenderBackend12::CreateBindlessByteAddressBuffer(
		L"scene_position_buffer",
		m_scratchPositionBufferOffset,
		m_scratchPositionBuffer,
		&uploader);

	uploader.SubmitUploads(cmdList);
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

	delete[] m_scratchIndexBuffer;
	delete[] m_scratchPositionBuffer;
}

void FScene::LoadNode(int nodeIndex, const tinygltf::Model& model, const Matrix& parentTransform)
{
	const tinygltf::Node& node = model.nodes[nodeIndex];
	
	// Transform (GLTF uses column-major storage)
	Matrix nodeTransform = Matrix::Identity;
	if (!node.matrix.empty())
	{
		const auto& m = node.matrix;
		nodeTransform = Matrix { 
			(float)m[0], (float)m[4], (float)m[8], (float)m[12],
			(float)m[1], (float)m[5], (float)m[9], (float)m[13],
			(float)m[2], (float)m[6], (float)m[10],(float)m[14],
			(float)m[3], (float)m[7], (float)m[11],(float)m[15]
		};
	}
	else if (!node.translation.empty() || !node.rotation.empty() || !node.scale.empty())
	{
		Matrix translation = !node.translation.empty() ? Matrix::CreateTranslation(Vector3{ (float*)node.translation.data() }) : Matrix::Identity;
		Matrix rotation = !node.rotation.empty() ? Matrix::CreateFromQuaternion(Quaternion{ (float*)node.rotation.data() }) : Matrix::Identity;
		Matrix scale = !node.scale.empty() ? Matrix::CreateScale(Vector3{ (float*)node.scale.data() }) : Matrix::Identity;

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

	for (const int childIndex : node.children)
	{
		LoadNode(childIndex, model, nodeTransform * parentTransform);
	}
}

void FScene::LoadMesh(int meshIndex, const tinygltf::Model& model, const Matrix& parentTransform)
{
	auto CopyBufferData = [&model](int accessorIndex, uint8_t* copyDest) -> size_t
	{
		size_t bytesCopied = 0;
		const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
		const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];

		size_t dataSize = tinygltf::GetComponentSizeInBytes(accessor.componentType) * tinygltf::GetNumComponentsInType(accessor.type);
		size_t dataStride = accessor.ByteStride(bufferView);

		const uint8_t* pSrc = &model.buffers[bufferView.buffer].data[bufferView.byteOffset + accessor.byteOffset];
		for (int i = 0; i < accessor.count; ++i)
		{
			memcpy(copyDest + i * dataSize, pSrc + i * dataStride, dataSize);
			bytesCopied += dataSize;
		}

		return bytesCopied;
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
		FRenderMesh newMesh = {};
		newMesh.m_name = mesh.name;
		newMesh.m_indexOffset = m_scratchIndexBufferOffset;
		newMesh.m_positionOffset = m_scratchPositionBufferOffset;
		m_meshGeo.push_back(newMesh);
		m_meshTransforms.push_back(parentTransform);

		m_scratchIndexBufferOffset += CopyBufferData(primitive.indices, m_scratchIndexBuffer + m_scratchIndexBufferOffset);

		auto posIt = primitive.attributes.find("POSITION");
		DebugAssert(posIt != primitive.attributes.cend());
		m_scratchPositionBufferOffset += CopyBufferData(posIt->second, m_scratchPositionBuffer + m_scratchPositionBufferOffset);

		m_meshBounds.push_back(CalcBounds(posIt->second));
	}
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
		newCamera.m_projectionTransform = Matrix::CreatePerspectiveFieldOfView(cam.yfov, cam.aspectRatio, cam.znear, cam.zfar);
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

void FView::Tick()
{

}

void FView::Reset(const FScene& scene)
{
	if (scene.m_cameras.size() > 0)
	{
		// Use provided camera
		m_viewTransform = scene.m_cameras[0].m_viewTransform;
		m_projectionTransform = scene.m_cameras[0].m_projectionTransform;
	}
	else
	{
		// Default camera

	}
}

// The returned index is offset to the beginning of the descriptor heap range
uint32_t FTextureCache::CacheTexture(const std::wstring& name, FResourceUploadContext* uploadContext)
{
	auto search = m_cachedTextures.find(name);
	if (search != m_cachedTextures.cend())
	{
		return search->second->m_srvIndex - (uint32_t)BindlessIndexRange::Texture2DBegin;
	}
	else
	{
		uint8_t* pixels;
		int width, height, bpp;
		uint16_t mipLevels;
		DXGI_FORMAT format;

		if (name == L"imgui_fonts")
		{
			ImGuiIO& io = ImGui::GetIO();
			io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height, &bpp);
			format = DXGI_FORMAT_R8G8B8A8_UNORM;
			mipLevels = 1;
		}

		m_cachedTextures[name] = RenderBackend12::CreateBindlessTexture(name, format, width, height, mipLevels, bpp, pixels, uploadContext);
		return m_cachedTextures[name]->m_srvIndex - (uint32_t)BindlessIndexRange::Texture2DBegin;
	}
}

void FTextureCache::Clear()
{
	m_cachedTextures.clear();
}

bool Demo::Initialize(const HWND& windowHandle, const uint32_t resX, const uint32_t resY)
{
	Profiling::Initialize();

	bool ok = RenderBackend12::Initialize(windowHandle, resX, resY);
	ok = ok && ShaderCompiler::Initialize();

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init(windowHandle);

	return ok;
}

void Demo::Tick(float dt)
{
	if (s_loadedScenePath != Settings::k_scenePath)
	{
		RenderBackend12::FlushGPU();
		s_scene.Reload(Settings::k_scenePath);
		s_view.Reset(s_scene);
		s_loadedScenePath = Settings::k_scenePath;
	}

	{
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
		FResourceUploadContext uploader{ 32 * 1024 * 1024 };
		uint32_t fontSrvIndex = s_textureCache.CacheTexture(L"imgui_fonts", &uploader);
		ImGui::GetIO().Fonts->TexID = (ImTextureID)fontSrvIndex;
		uploader.SubmitUploads(cmdList);
		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		static bool show_demo_window = true;
		static bool show_another_window = false;
		static ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

		static float f = 0.0f;
		static int counter = 0;

		ImGui::Begin("Hello, world!");                          // Create a window called "Hello, world!" and append into it.

		ImGui::Text("This is some useful text.");               // Display some text (you can use a format strings too)
		ImGui::Checkbox("Demo Window", &show_demo_window);      // Edit bools storing our window open/close state
		ImGui::Checkbox("Another Window", &show_another_window);

		ImGui::SliderFloat("float", &f, 0.0f, 1.0f);            // Edit 1 float using a slider from 0.0f to 1.0f
		ImGui::ColorEdit3("clear color", (float*)&clear_color); // Edit 3 floats representing a color

		if (ImGui::Button("Button"))                            // Buttons return true when clicked (most widgets return true when edited/activated)
			counter++;
		ImGui::SameLine();
		ImGui::Text("counter = %d", counter);

		ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
		ImGui::End();

		ImGui::EndFrame();
		ImGui::Render();
	}
}

void Demo::Teardown(HWND& windowHandle)
{
	RenderBackend12::FlushGPU();

	s_scene.Clear();
	s_textureCache.Clear();

	if (windowHandle)
	{
		RenderBackend12::Teardown();
		ShaderCompiler::Teardown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
	}
}

void Demo::OnMouseMove(WPARAM btnState, int x, int y)
{
	s_mouseButtonState = btnState;
	s_currentMousePos = { x, y };
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT Demo::WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
}
