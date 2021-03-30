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

	FLightProbe CacheHdrTexture(const std::wstring& name);

	void Clear();

	concurrency::concurrent_unordered_map<std::wstring, std::unique_ptr<FBindlessShaderResource>> m_cachedTextures;
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
	float s_aspectRatio;

	const FScene* GetScene()
	{
		return &s_scene;
	}

	const FView* GetView()
	{
		return &s_view;
	}
}

bool Demo::Initialize(const HWND& windowHandle, const uint32_t resX, const uint32_t resY)
{
	s_aspectRatio = resX / (float)resY;

	Profiling::Initialize();

	bool ok = RenderBackend12::Initialize(windowHandle, resX, resY);
	ok = ok && ShaderCompiler::Initialize();

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init(windowHandle);

	return ok;
}

void Demo::Tick(float deltaTime)
{
	// Reload scene file if required
	if (s_scene.m_sceneFilename.empty() ||
		s_scene.m_sceneFilename != Settings::k_sceneFilename)
	{
		RenderBackend12::FlushGPU();
		s_scene.Reload(Settings::k_sceneFilename);
		RenderBackend12::FlushGPU();
		s_view.Reset(s_scene);
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

	{
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
		Profiling::Teardown();
	}
}

void Demo::OnMouseMove(WPARAM buttonState, int x, int y)
{
	s_controller.MouseMove(buttonState, POINT{ x, y });
}

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Scene
//-----------------------------------------------------------------------------------------------------------------------------------------------

void FScene::Reload(const std::string& filename)
{
	tinygltf::TinyGLTF loader;
	std::string errors, warnings;

	// Load GLTF
	tinygltf::Model model;
	bool ok = loader.LoadASCIIFromFile(&model, &errors, &warnings, GetFilepathA(filename));

	if (!warnings.empty())
	{
		printf("Warn: %s\n", warnings.c_str());
	}

	if (!errors.empty())
	{
		printf("Error: %s\n", errors.c_str());
	}

	DebugAssert(ok, "Failed to parse glTF");
	m_sceneFilename = filename;

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
	m_scratchNormalBuffer = new uint8_t[maxSize];
	m_scratchUvBuffer = new uint8_t[maxSize];

	// GlTF uses a right handed coordinate. Use the following root transform to convert it to LH.
	Matrix RH2LH = Matrix
	{
		Vector3{1.f, 0.f , 0.f},
		Vector3{0.f, 1.f , 0.f},
		Vector3{0.f, 0.f, -1.f}
	};

	// Parse GLTF and initialize scene
	// See https://github.com/KhronosGroup/glTF-Tutorials/blob/master/gltfTutorial/gltfTutorial_003_MinimalGltfFile.md
	for (const tinygltf::Scene& scene : model.scenes)
	{
		for (const int nodeIndex : scene.nodes)
		{
			LoadNode(nodeIndex, model, RH2LH);
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
	FResourceUploadContext uploader{ maxSize };
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

	m_meshNormalBuffer = RenderBackend12::CreateBindlessBuffer(
		L"scene_normal_buffer",
		m_scratchNormalBufferOffset,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		m_scratchNormalBuffer,
		&uploader);

	m_meshUvBuffer = RenderBackend12::CreateBindlessBuffer(
		L"scene_uv_buffer",
		m_scratchUvBufferOffset,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		m_scratchUvBuffer,
		&uploader);

	FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
	uploader.SubmitUploads(cmdList);
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

	delete[] m_scratchIndexBuffer;
	delete[] m_scratchPositionBuffer;
	delete[] m_scratchNormalBuffer;
	delete[] m_scratchUvBuffer;

	m_globalLightProbe = Demo::s_textureCache.CacheHdrTexture(L"lilienstein_2k.hdr");
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
			(float)m[0], (float)m[1], (float)m[2], (float)m[3],
			(float)m[4], (float)m[5], (float)m[6], (float)m[7],
			(float)m[8], (float)m[9], (float)m[10],(float)m[11],
			(float)m[12], (float)m[13], (float)m[14],(float)m[15]
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
	auto CopyIndexData = [&model](const tinygltf::Accessor& accessor, uint8_t* copyDest) -> size_t
	{
		size_t bytesCopied = 0;
		const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
		size_t dataSize = tinygltf::GetComponentSizeInBytes(accessor.componentType) * tinygltf::GetNumComponentsInType(accessor.type);
		size_t dataStride = accessor.ByteStride(bufferView);

		const uint8_t* pSrc = &model.buffers[bufferView.buffer].data[bufferView.byteOffset + accessor.byteOffset];
		uint32_t* indexDest = (uint32_t*)copyDest;

		if (dataSize == 2)
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

		// FLOAT3 position data
		auto posIt = primitive.attributes.find("POSITION");
		DebugAssert(posIt != primitive.attributes.cend());
		const tinygltf::Accessor& positionAccessor = model.accessors[posIt->second];
		const size_t positionSize = tinygltf::GetComponentSizeInBytes(positionAccessor.componentType) * tinygltf::GetNumComponentsInType(positionAccessor.type);
		DebugAssert(positionSize == 3 * sizeof(float));

		// FLOAT3 normal data
		auto normalIt = primitive.attributes.find("NORMAL");
		DebugAssert(normalIt != primitive.attributes.cend());
		const tinygltf::Accessor& normalAccessor = model.accessors[normalIt->second];
		const size_t normalSize = tinygltf::GetComponentSizeInBytes(normalAccessor.componentType) * tinygltf::GetNumComponentsInType(normalAccessor.type);
		DebugAssert(normalSize == 3 * sizeof(float));

		// FLOAT2 UV data
		auto uvIt = primitive.attributes.find("TEXCOORD_0");
		DebugAssert(uvIt != primitive.attributes.cend());
		const tinygltf::Accessor& uvAccessor = model.accessors[uvIt->second];
		const size_t uvSize = tinygltf::GetComponentSizeInBytes(uvAccessor.componentType) * tinygltf::GetNumComponentsInType(uvAccessor.type);
		DebugAssert(uvSize == 2 * sizeof(float));

		tinygltf::Material material = model.materials[primitive.material];

		FRenderMesh newMesh = {};
		newMesh.m_name = mesh.name;
		newMesh.m_indexOffset = m_scratchIndexBufferOffset / sizeof(uint32_t);
		newMesh.m_positionOffset = m_scratchPositionBufferOffset / positionSize;
		newMesh.m_normalOffset = m_scratchNormalBufferOffset / normalSize;
		newMesh.m_uvOffset = m_scratchUvBufferOffset / uvSize;
		newMesh.m_indexCount = indexAccessor.count;
		newMesh.m_materialName = material.name;
		newMesh.m_emissiveFactor = Vector3{ (float)material.emissiveFactor[0], (float)material.emissiveFactor[1], (float)material.emissiveFactor[2] };
		newMesh.m_baseColorFactor = Vector3{ (float)material.pbrMetallicRoughness.baseColorFactor[0], (float)material.pbrMetallicRoughness.baseColorFactor[1], (float)material.pbrMetallicRoughness.baseColorFactor[2] };
		newMesh.m_metallicFactor = (float)material.pbrMetallicRoughness.metallicFactor;
		newMesh.m_roughnessFactor = (float)material.pbrMetallicRoughness.roughnessFactor;
		newMesh.m_baseColorTextureIndex = material.pbrMetallicRoughness.baseColorTexture.index != -1 ? LoadTexture(model.images[model.textures[material.pbrMetallicRoughness.baseColorTexture.index].source], true) : -1;
		newMesh.m_metallicRoughnessTextureIndex = material.pbrMetallicRoughness.metallicRoughnessTexture.index != -1 ? LoadTexture(model.images[model.textures[material.pbrMetallicRoughness.metallicRoughnessTexture.index].source], false) : -1;
		newMesh.m_normalTextureIndex = material.normalTexture.index != -1 ? LoadTexture(model.images[model.textures[material.normalTexture.index].source], false) : -1;
		newMesh.m_baseColorSamplerIndex = material.pbrMetallicRoughness.baseColorTexture.index != -1 ? LoadSampler(model.samplers[model.textures[material.pbrMetallicRoughness.baseColorTexture.index].sampler]) : -1;
		newMesh.m_metallicRoughnessSamplerIndex = material.pbrMetallicRoughness.metallicRoughnessTexture.index != -1 ? LoadSampler(model.samplers[model.textures[material.pbrMetallicRoughness.metallicRoughnessTexture.index].sampler]) : -1;
		newMesh.m_normalSamplerIndex = material.normalTexture.index != -1 ? LoadSampler(model.samplers[model.textures[material.normalTexture.index].sampler]) : -1;
		m_meshGeo.push_back(newMesh);
		m_meshTransforms.push_back(parentTransform);

		m_scratchIndexBufferOffset += CopyIndexData(indexAccessor, m_scratchIndexBuffer + m_scratchIndexBufferOffset);
		m_scratchPositionBufferOffset += CopyBufferData(positionAccessor, positionSize, m_scratchPositionBuffer + m_scratchPositionBufferOffset);
		m_scratchNormalBufferOffset += CopyBufferData(normalAccessor, normalSize, m_scratchNormalBuffer + m_scratchNormalBufferOffset);
		m_scratchUvBufferOffset += CopyBufferData(uvAccessor, uvSize, m_scratchUvBuffer + m_scratchUvBufferOffset);

		m_meshBounds.push_back(CalcBounds(posIt->second));
	}
}

int FScene::LoadTexture(const tinygltf::Image& image, const bool srgb)
{
	DebugAssert(!image.uri.empty(), "Embedded image data is not yet supported.");

	DXGI_FORMAT srcFormat = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT compressedFormat = DXGI_FORMAT_UNKNOWN;
	if (image.pixel_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE && image.component == 4)
	{
		srcFormat = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
		compressedFormat = srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
	}

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
	AssertIfFailed(DirectX::GenerateMipMaps(srcImage, DirectX::TEX_FILTER_LINEAR, numMips, mipchain));

	// Block compression
	DirectX::ScratchImage compressedScratch;
	AssertIfFailed(DirectX::Compress(mipchain.GetImages(), numMips, mipchain.GetMetadata(), compressedFormat, DirectX::TEX_COMPRESS_PARALLEL, DirectX::TEX_THRESHOLD_DEFAULT, compressedScratch));

	std::wstring name{ image.uri.begin(), image.uri.end() };
	FResourceUploadContext uploader{ compressedScratch.GetPixelsSize() };
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

int FScene::LoadSampler(const tinygltf::Sampler& sampler)
{
	return -1;
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
	constexpr float speed = 5.f;
	bool updateView = false;

	// Walk
	if (controller->MoveForward())
	{
		m_position += speed * deltaTime * m_look;
		updateView = true;
	}
	else if (controller->MoveBack())
	{
		m_position -= speed * deltaTime * m_look;
		updateView = true;
	}

	// Strafe
	if (controller->StrafeLeft())
	{
		m_position -= speed * deltaTime * m_right;
		updateView = true;
	}
	else if (controller->StrafeRight())
	{
		m_position += speed * deltaTime * m_right;
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
}

void FView::Reset(const FScene& scene)
{
	if (scene.m_cameras.size() > 0)
	{
		// Use provided camera
		m_viewTransform = scene.m_cameras[0].m_viewTransform;
		m_projectionTransform = scene.m_cameras[0].m_projectionTransform;

		m_position = m_viewTransform.Translation();
		m_right = m_viewTransform.Right();
		m_up = m_viewTransform.Up();
		m_look = m_viewTransform.Backward();
	}
	else
	{
		// Default camera
		m_position = { 0.f, 0.f, -15.f };
		m_right = { 1.f, 0.f, 0.f };
		m_up = { 0.f, 1.f, 0.f };
		m_look = { 0.f, 0.f, 1.f };

		UpdateViewTransform();
		m_projectionTransform = GetReverseZInfinitePerspectiveFovLH(0.25f * DirectX::XM_PI, Demo::s_aspectRatio, 1.f);
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

FLightProbe FTextureCache::CacheHdrTexture(const std::wstring& name)
{
	const std::wstring envmapTextureName = name + L".envmap";
	const std::wstring shTextureName = name + L".shtex";

	auto search0 = m_cachedTextures.find(envmapTextureName);
	auto search1 = m_cachedTextures.find(shTextureName);
	if (search0 != m_cachedTextures.cend() && search1 != m_cachedTextures.cend())
	{
		return FLightProbe{
			(int)RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::TextureCube, search0->second->m_srvIndex),
			(int)RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::Texture2D, search1->second->m_srvIndex),
			-1
		};
	}
	else
	{
		RenderBackend12::BeginCapture();

		// Read HDR spehere map from file
		DirectX::TexMetadata metadata;
		DirectX::ScratchImage scratch;
		AssertIfFailed(DirectX::LoadFromHDRFile(GetFilepathW(name).c_str(), &metadata, scratch));

		// Calculate mips upto 4x4 for block compression
		int numMips = 0;
		size_t width = metadata.width, height = metadata.height;
		while (width >= 4 && height >= 4)
		{
			numMips++;
			width = width >> 1;
			height = height >> 1;
		}

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
		SCOPED_COMMAND_QUEUE_EVENT(cmdList->m_type, L"hdr_preprocess", 0);
		uploadContext.SubmitUploads(cmdList);

		// ---------------------------------------------------------------------------------------------------------
		// Generate environment cubemap
		// ---------------------------------------------------------------------------------------------------------
		const size_t cubemapSize = metadata.height;
		auto texCubeUav = RenderBackend12::CreateBindlessUavTexture(L"texcube_uav", metadata.format, cubemapSize, cubemapSize, numMips, 6);

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, L"cubemap_gen", 0);

			// Root Signature
			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({ L"cubemapgen.hlsl", L"rootsig" });
			d3dCmdList->SetComputeRootSignature(rootsig.get());

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({ L"cubemapgen.hlsl", L"cs_main", L"THREAD_GROUP_SIZE_X=16 THREAD_GROUP_SIZE_Y=16" }, L"cs_6_4");

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
				uint32_t mipIndex;
				uint32_t hdrTextureIndex;
				uint32_t cubemapUavIndex;
				uint32_t cubemapSize;
			};

			// Convert from sperical map to cube map
			CbLayout computeCb =
			{
				.mipIndex = 0,
				.hdrTextureIndex = RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::Texture2D, srcHdrTex->m_srvIndex),
				.cubemapUavIndex = RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::RWTexture2DArray, texCubeUav->m_uavIndices[0]),
				.cubemapSize = (uint32_t)cubemapSize
			};

			d3dCmdList->SetComputeRoot32BitConstants(0, sizeof(CbLayout) / 4, &computeCb, 0);
			d3dCmdList->SetComputeRootDescriptorTable(1, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::Texture2DBegin));
			d3dCmdList->SetComputeRootDescriptorTable(2, RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (uint32_t)BindlessDescriptorRange::RWTexture2DArrayBegin));

			// Dispatch
			size_t threadGroupCount = std::max<size_t>(std::ceil(cubemapSize / 16), 1);
			d3dCmdList->Dispatch(threadGroupCount, threadGroupCount, 1);
		}

		// ---------------------------------------------------------------------------------------------------------
		// Prefilter Environment map
		// ---------------------------------------------------------------------------------------------------------

		// Transition mip 0 of the cubemap (https://docs.microsoft.com/en-us/windows/win32/direct3d12/subresources)
		texCubeUav->Transition(cmdList, D3D12CalcSubresource(0, 0, 0, numMips, 6), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		texCubeUav->Transition(cmdList, D3D12CalcSubresource(0, 1, 0, numMips, 6), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		texCubeUav->Transition(cmdList, D3D12CalcSubresource(0, 2, 0, numMips, 6), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		texCubeUav->Transition(cmdList, D3D12CalcSubresource(0, 3, 0, numMips, 6), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		texCubeUav->Transition(cmdList, D3D12CalcSubresource(0, 4, 0, numMips, 6), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		texCubeUav->Transition(cmdList, D3D12CalcSubresource(0, 5, 0, numMips, 6), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, L"prefilter_cubemap", 0);

			// Root Signature
			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({ L"prefilter.hlsl", L"rootsig" });
			d3dCmdList->SetComputeRootSignature(rootsig.get());

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({ L"prefilter.hlsl", L"cs_main", L"THREAD_GROUP_SIZE_X=16 THREAD_GROUP_SIZE_Y=16" }, L"cs_6_4");

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
				uint32_t faceIndex;
				uint32_t envmapSrvIndex;
				uint32_t uavIndex;
				uint32_t sampleCount;
				float roughness;
			};

			for (uint32_t mipIndex = 1; mipIndex < numMips; ++mipIndex)
			{
				uint32_t mipSize = cubemapSize >> mipIndex;

				for (uint32_t faceIndex = 0; faceIndex < 6; ++faceIndex)
				{
					CbLayout computeCb =
					{
						.mipSize = mipSize,
						.faceIndex = faceIndex,
						.envmapSrvIndex = RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::TextureCube, texCubeUav->m_srvIndex),
						.uavIndex = RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::RWTexture2DArray, texCubeUav->m_uavIndices[mipIndex]),
						.sampleCount = 1024,
						.roughness = (float)mipIndex
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
		texCubeUav->Transition(cmdList, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE);
		auto cubemapTex = RenderBackend12::CreateBindlessTexture(envmapTextureName, BindlessResourceType::TextureCube, metadata.format, cubemapSize, cubemapSize, numMips, 6, D3D12_RESOURCE_STATE_COPY_DEST);
		d3dCmdList->CopyResource(cubemapTex->m_resource->m_d3dResource, texCubeUav->m_resource->m_d3dResource);
		cubemapTex->Transition(cmdList, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_cachedTextures[envmapTextureName] = std::move(cubemapTex);

		// ---------------------------------------------------------------------------------------------------------
		// Project radiance to SH basis
		// ---------------------------------------------------------------------------------------------------------
		constexpr int numCoefficients = 9; 
		constexpr uint32_t srcMipIndex = 2;
		auto shTexureUav0 = RenderBackend12::CreateBindlessUavTexture(L"ShProj_uav0", metadata.format, metadata.width >> srcMipIndex, metadata.height >> srcMipIndex, 1, numCoefficients);

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, L"SH_projection", 0);

			// Root Signature
			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({ L"sh-projection.hlsl", L"rootsig" });
			d3dCmdList->SetComputeRootSignature(rootsig.get());

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({ L"sh-projection.hlsl", L"cs_main", L"THREAD_GROUP_SIZE_X=16 THREAD_GROUP_SIZE_Y=16" }, L"cs_6_4");

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
				metadata.width,
				metadata.height,
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
		auto shTexureUav1 = RenderBackend12::CreateBindlessUavTexture(L"ShProj_uav1", metadata.format, (metadata.width >> srcMipIndex) / 16, (metadata.height >> srcMipIndex) / 16, 1, numCoefficients);

		// Ping-pong UAVs
		FBindlessUav* uavs[2] = { shTexureUav0.get(), shTexureUav1.get() };
		int src = 0, dest = 1;

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, L"SH_integration", 0);

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
			IDxcBlob* csBlob = RenderBackend12::CacheShader({ L"sh-integration.hlsl", L"cs_main", s.str() }, L"cs_6_4");

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

		auto shTexureUavAccum = RenderBackend12::CreateBindlessUavTexture(L"ShAccum_uav", metadata.format, numCoefficients, 1, 1, 1);

		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, L"SH_accum", 0);

			// Root Signature
			winrt::com_ptr<D3DRootSignature_t> rootsig = RenderBackend12::FetchRootSignature({ L"sh-accumulation.hlsl", L"rootsig" });
			d3dCmdList->SetComputeRootSignature(rootsig.get());

			std::wstringstream s;
			s << "THREAD_GROUP_SIZE_X=" << width <<
				" THREAD_GROUP_SIZE_Y=" << height;

			// PSO
			IDxcBlob* csBlob = RenderBackend12::CacheShader({ L"sh-accumulation.hlsl", L"cs_main", s.str() }, L"cs_6_4");

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
		shTexureUavAccum->Transition(cmdList, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE);
		auto shTex = RenderBackend12::CreateBindlessTexture(shTextureName, BindlessResourceType::Texture2D, metadata.format, numCoefficients, 1, 1, 1, D3D12_RESOURCE_STATE_COPY_DEST);
		d3dCmdList->CopyResource(shTex->m_resource->m_d3dResource, shTexureUavAccum->m_resource->m_d3dResource);
		shTex->Transition(cmdList, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_cachedTextures[shTextureName] = std::move(shTex);


		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

		RenderBackend12::EndCapture();

		return FLightProbe{
			(int)RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::TextureCube, m_cachedTextures[envmapTextureName]->m_srvIndex),
			(int)RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType::Texture2D, m_cachedTextures[shTextureName]->m_srvIndex),
			-1
		};
	}
}

void FTextureCache::Clear()
{
	m_cachedTextures.clear();
}

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														ImGui
//-----------------------------------------------------------------------------------------------------------------------------------------------

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT Demo::WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
}
