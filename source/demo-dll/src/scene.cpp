#include <demo.h>
#include <profiling.h>
#include <backend-d3d12.h>
#include <common.h>
#include <mesh-utils.h>
#include <gpu-shared-types.h>
#include <concurrent_unordered_map.h>
#include <ppltasks.h>
#include <ppl.h>
#include <dxcapi.h>
#include <scene.h>

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

	if (Demo::GetConfig().UseContentCache && std::filesystem::exists(destFilename))
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
	std::filesystem::path filepath{ filename };
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
	FScene::s_loadProgress = 0.f;

	tinygltf::TinyGLTF loader;
	m_textureCachePath = GetContentCachePath(modelFilepath);
	const char* path = m_textureCachePath.c_str();
	loader.SetImageLoader(&LoadImageCallback, (void*)path);

	// Load from model cache if a cached version exists
	m_modelCachePath = GetContentCachePath(modelFilepath);
	std::filesystem::path cachedFilepath = std::filesystem::path{ m_modelCachePath } / std::filesystem::path{ ws2s(filename) };
	if (Demo::GetConfig().UseContentCache && std::filesystem::exists(cachedFilepath))
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

		FScene::s_loadProgress += FScene::s_modelLoadTimeFrac;
	}

	m_modelFilename = filename;

	// Clear previous scene
	Clear();

	// Load assets
	bool requiresResave = MeshUtils::FixupMeshes(model);
	FScene::s_loadProgress += FScene::s_meshFixupTimeFrac;
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

	// Cache the sun transform in the model
	int sunIndex = GetDirectionalLight();
	if (sunIndex != -1)
	{
		m_originalSunTransform = m_sceneLights.m_transformList[sunIndex];
	}

	// Update the sun transform based on Time of Day
	UpdateSunDirection();

	if (Demo::GetConfig().EnvSkyMode == (int)EnvSkyMode::DynamicSky)
	{
		UpdateDynamicSky();
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
	m_skylight = Demo::GetTextureCache().CacheHDRI(filename);
	m_hdriFilename = filename;
	FScene::s_loadProgress += FScene::s_cacheHDRITimeFrac;
}

void FScene::LoadNode(int nodeIndex, tinygltf::Model& model, const Matrix& parentTransform)
{
	const tinygltf::Node& node = model.nodes[nodeIndex];

	// Transform (GLTF uses column-major storage)
	Matrix nodeTransform = Matrix::Identity;
	if (!node.matrix.empty())
	{
		const auto& m = node.matrix;
		nodeTransform = Matrix{
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

	float progressIncrement = FScene::s_meshBufferLoadTimeFrac / (float)model.buffers.size();

	FResourceUploadContext uploader{ uploadSize };

	m_meshBuffers.resize(model.buffers.size());
	for (int bufferIndex = 0; bufferIndex < model.buffers.size(); ++bufferIndex)
	{
		m_meshBuffers[bufferIndex].reset(RenderBackend12::CreateNewShaderBuffer({
			.name = PrintString(L"scene_mesh_buffer_%d", bufferIndex),
			.type = FShaderBuffer::Type::Raw,
			.accessMode = FResource::AccessMode::GpuReadOnly,
			.alloc = FResource::Allocation::Persistent(),
			.size = model.buffers[bufferIndex].data.size(),
			.upload = {
				.pData = model.buffers[bufferIndex].data.data(),
				.context = &uploader 
			}
		}));

		FScene::s_loadProgress += progressIncrement;
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
			views[viewIndex].m_bufferSrvIndex = m_meshBuffers[meshBufferIndex]->m_descriptorIndices.SRV;
			views[viewIndex].m_byteLength = model.bufferViews[viewIndex].byteLength;
			views[viewIndex].m_byteOffset = model.bufferViews[viewIndex].byteOffset;
		});

	const size_t bufferSize = views.size() * sizeof(FMeshBufferView);
	FResourceUploadContext uploader{ bufferSize };

	m_packedMeshBufferViews.reset(RenderBackend12::CreateNewShaderBuffer({
		.name = L"scene_mesh_buffer_views",
		.type = FShaderBuffer::Type::Raw,
		.accessMode = FResource::AccessMode::GpuReadOnly,
		.alloc = FResource::Allocation::Persistent(),
		.size = bufferSize,
		.upload = {
			.pData = (const uint8_t*)views.data(),
			.context = &uploader
		}
	}));

	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_mesh_buffer_views", D3D12_COMMAND_LIST_TYPE_DIRECT);
	uploader.SubmitUploads(cmdList);
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

	FScene::s_loadProgress += FScene::s_meshBufferViewsLoadTimeFrac;
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

	m_packedMeshAccessors.reset(RenderBackend12::CreateNewShaderBuffer({
		.name = L"scene_mesh_accessors",
		.type = FShaderBuffer::Type::Raw,
		.accessMode = FResource::AccessMode::GpuReadOnly,
		.alloc = FResource::Allocation::Persistent(),
		.size = bufferSize,
		.upload = {
			.pData = (const uint8_t*)accessors.data(),
			.context = &uploader 
		}
	}));

	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_mesh_accessors", D3D12_COMMAND_LIST_TYPE_DIRECT);
	uploader.SubmitUploads(cmdList);
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

	FScene::s_loadProgress += FScene::s_meshAccessorsLoadTimeFrac;
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

		m_packedPrimitives.reset(RenderBackend12::CreateNewShaderBuffer({
			.name = L"scene_primitives",
			.type = FShaderBuffer::Type::Raw,
			.accessMode = FResource::AccessMode::GpuReadOnly,
			.alloc = FResource::Allocation::Persistent(),
			.size = bufferSize,
			.upload = {
				.pData = (const uint8_t*)primitives.data(),
				.context = &uploader 
			}
		}));

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

		m_packedPrimitiveCounts.reset(RenderBackend12::CreateNewShaderBuffer({
			.name = L"scene_primitive_counts",
			.type = FShaderBuffer::Type::Raw,
			.accessMode = FResource::AccessMode::GpuReadOnly,
			.alloc = FResource::Allocation::Persistent(),
			.size = bufferSize,
			.upload = {
				.pData = (const uint8_t*)primitiveCounts.data(),
				.context = &uploader 
			}
		}));

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
		FResourceUploadContext uploader{ indexBufferSize };

		m_packedLightIndices.reset(RenderBackend12::CreateNewShaderBuffer({
			.name = L"scene_light_indices",
			.type = FShaderBuffer::Type::Raw,
			.accessMode = FResource::AccessMode::GpuReadOnly,
			.alloc = FResource::Allocation::Persistent(),
			.size = indexBufferSize,
			.upload = {
				.pData = (const uint8_t*)m_sceneLights.m_entityList.data(),
				.context = &uploader 
			}
		}));

		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"upload_lights", D3D12_COMMAND_LIST_TYPE_DIRECT);
		uploader.SubmitUploads(cmdList);
		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
	}
}

void FScene::CreateAccelerationStructures(const tinygltf::Model& model)
{
	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"create_acceleration_structure", D3D12_COMMAND_LIST_TYPE_DIRECT);
	FFenceMarker gpuFinishFence = cmdList->GetFence(FCommandList::SyncPoint::GpuFinish);

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

				std::unique_ptr<FShaderBuffer> blasScratch{ RenderBackend12::CreateNewShaderBuffer({
					.name = L"blas_scratch",
					.type = FShaderBuffer::Type::AccelerationStructure,
					.accessMode = FResource::AccessMode::GpuWriteOnly,
					.alloc = FResource::Allocation::Transient(gpuFinishFence),
					.size = GetAlignedSize(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, blasPreBuildInfo.ScratchDataSizeInBytes) })};

				m_blasList[meshName].reset(RenderBackend12::CreateNewShaderBuffer({
					.name = PrintString(L"%s_blas", s2ws(meshName)),
					.type = FShaderBuffer::Type::AccelerationStructure,
					.accessMode = FResource::AccessMode::GpuReadWrite,
					.alloc = FResource::Allocation::Persistent(),
					.size = GetAlignedSize(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, blasPreBuildInfo.ResultDataMaxSizeInBytes) }));

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
		std::unique_ptr<FSystemBuffer> instanceDescBuffer{ RenderBackend12::CreateNewSystemBuffer({
			.name = L"instance_descs_buffer",
			.accessMode = FResource::AccessMode::CpuWriteOnly,
			.alloc = FResource::Allocation::Transient(gpuFinishFence),
			.size = instanceDescBufferSize,
			.uploadCallback = [pData = instanceDescs.data(), instanceDescBufferSize](uint8_t* pDest)
			{
				memcpy(pDest, pData, instanceDescBufferSize);
			}
		})};

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputsDesc = {};
		tlasInputsDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		tlasInputsDesc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		tlasInputsDesc.InstanceDescs = instanceDescBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress();
		tlasInputsDesc.NumDescs = instanceDescs.size();
		tlasInputsDesc.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPreBuildInfo = {};
		RenderBackend12::GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputsDesc, &tlasPreBuildInfo);

		std::unique_ptr<FShaderBuffer> tlasScratch{ RenderBackend12::CreateNewShaderBuffer({
			.name = L"tlas_scratch",
			.type = FShaderBuffer::Type::AccelerationStructure,
			.accessMode = FResource::AccessMode::GpuWriteOnly,
			.alloc = FResource::Allocation::Transient(gpuFinishFence),
			.size = GetAlignedSize(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, tlasPreBuildInfo.ScratchDataSizeInBytes) })};

		m_tlas.reset(RenderBackend12::CreateNewShaderBuffer({
			.name = L"tlas_buffer",
			.type = FShaderBuffer::Type::AccelerationStructure,
			.accessMode = FResource::AccessMode::GpuReadWrite,
			.alloc = FResource::Allocation::Persistent(),
			.size = GetAlignedSize(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, tlasPreBuildInfo.ResultDataMaxSizeInBytes) }));

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

	const float progressIncrement = FScene::s_materialLoadTimeFrac / (float)model.materials.size();

	//concurrency::parallel_for(0, (int)model.materials.size(), [&](int i)
	for (int i = 0; i < model.materials.size(); ++i)
	{
		m_materialList[i] = LoadMaterial(model, i);
		FScene::s_loadProgress += progressIncrement;
	}//);


	const size_t bufferSize = m_materialList.size() * sizeof(FMaterial);
	FResourceUploadContext uploader{ bufferSize };

	m_packedMaterials.reset(RenderBackend12::CreateNewShaderBuffer({
		.name = L"scene_materials",
		.type = FShaderBuffer::Type::Raw,
		.accessMode = FResource::AccessMode::GpuReadOnly,
		.alloc = FResource::Allocation::Persistent(),
		.size = bufferSize,
		.upload = {
			.pData = (const uint8_t*)m_materialList.data(),
			.context = &uploader 
		}
	}));

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
	mat.m_emissiveSamplerIndex = material.emissiveTexture.index != -1 ? Demo::GetSamplerCache().CacheSampler(model.samplers[model.textures[material.emissiveTexture.index].sampler]) : -1;
	mat.m_baseColorSamplerIndex = material.pbrMetallicRoughness.baseColorTexture.index != -1 ? Demo::GetSamplerCache().CacheSampler(model.samplers[model.textures[material.pbrMetallicRoughness.baseColorTexture.index].sampler]) : -1;
	mat.m_metallicRoughnessSamplerIndex = material.pbrMetallicRoughness.metallicRoughnessTexture.index != -1 ? Demo::GetSamplerCache().CacheSampler(model.samplers[model.textures[material.pbrMetallicRoughness.metallicRoughnessTexture.index].sampler]) : -1;
	mat.m_normalSamplerIndex = material.normalTexture.index != -1 ? Demo::GetSamplerCache().CacheSampler(model.samplers[model.textures[material.normalTexture.index].sampler]) : -1;
	mat.m_aoSamplerIndex = material.occlusionTexture.index != -1 ? Demo::GetSamplerCache().CacheSampler(model.samplers[model.textures[material.occlusionTexture.index].sampler]) : -1;

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
			mat.m_transmissionSamplerIndex = Demo::GetSamplerCache().CacheSampler(model.samplers[model.textures[texId].sampler]);
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
			mat.m_clearcoatSamplerIndex = Demo::GetSamplerCache().CacheSampler(model.samplers[model.textures[texId].sampler]);
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

		mat.m_clearcoatRoughnessSamplerIndex = clearcoatRoughnessTexId != -1 ? Demo::GetSamplerCache().CacheSampler(model.samplers[model.textures[clearcoatRoughnessTexId].sampler]) : -1;
		mat.m_clearcoatNormalSamplerIndex = clearcoatNormalTexId != -1 ? Demo::GetSamplerCache().CacheSampler(model.samplers[model.textures[clearcoatNormalTexId].sampler]) : -1;
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
	SCOPED_CPU_EVENT("load_texture", PIX_COLOR_DEFAULT);
	DebugAssert(!image.uri.empty(), "Embedded image data is not yet supported.");

	if (image.image.empty())
	{
		SCOPED_CPU_EVENT("content_cache_hit", PIX_COLOR_DEFAULT);

		// Compressed image was found in texture cache
		const std::wstring cachedFilepath = s2ws(image.uri);
		DebugAssert(std::filesystem::exists(cachedFilepath), "File not found in texture cache");

		// Load from cache
		DirectX::TexMetadata metadata;
		DirectX::ScratchImage scratch;
		{
			SCOPED_CPU_EVENT("load_dds", PIX_COLOR_DEFAULT);
			AssertIfFailed(DirectX::LoadFromDDSFile(cachedFilepath.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, scratch));
		}

		// Upload
		std::wstring name = std::filesystem::path{ cachedFilepath }.filename().wstring();
		FResourceUploadContext uploader{ RenderBackend12::GetResourceSize(scratch) };
		uint32_t bindlessIndex = Demo::GetTextureCache().CacheTexture2D(
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
		SCOPED_CPU_EVENT("content_cache_miss", PIX_COLOR_DEFAULT);
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
		HRESULT hr;
		{
			SCOPED_CPU_EVENT("generate_mips", PIX_COLOR_DEFAULT);
			hr = DirectX::GenerateMipMaps(srcImage, DirectX::TEX_FILTER_LINEAR, numMips, mipchain);
		}


		if (SUCCEEDED((hr)))
		{
			SCOPED_CPU_EVENT("block_compression", PIX_COLOR_DEFAULT);

			// Block compression
			DirectX::ScratchImage compressedScratch;
			{
				SCOPED_CPU_EVENT("block_compression", PIX_COLOR_DEFAULT);
				AssertIfFailed(DirectX::Compress(mipchain.GetImages(), numMips, mipchain.GetMetadata(), compressedFormat, DirectX::TEX_COMPRESS_PARALLEL, DirectX::TEX_THRESHOLD_DEFAULT, compressedScratch));
			}

			// Save to disk
			if (Demo::GetConfig().UseContentCache)
			{
				SCOPED_CPU_EVENT("save_to_disk", PIX_COLOR_DEFAULT);
				std::filesystem::path dirPath{ m_textureCachePath };
				std::filesystem::path srcFilename{ image.uri };
				std::filesystem::path destFilename = dirPath / srcFilename.stem();
				destFilename += std::filesystem::path{ ".dds" };
				DirectX::TexMetadata compressedMetadata = compressedScratch.GetMetadata();
				AssertIfFailed(DirectX::SaveToDDSFile(compressedScratch.GetImages(), compressedScratch.GetImageCount(), compressedMetadata, DirectX::DDS_FLAGS_NONE, destFilename.wstring().c_str()));
			}

			std::wstring name{ image.uri.begin(), image.uri.end() };
			FResourceUploadContext uploader{ RenderBackend12::GetResourceSize(compressedScratch) };
			uint32_t bindlessIndex = Demo::GetTextureCache().CacheTexture2D(
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
			SCOPED_CPU_EVENT("uncompressed", PIX_COLOR_DEFAULT);

			// No mips and no compression. Don't save to cache and load directly
			DirectX::ScratchImage scratch;
			scratch.InitializeFromImage(srcImage);
			std::wstring name{ image.uri.begin(), image.uri.end() };
			FResourceUploadContext uploader{ RenderBackend12::GetResourceSize(scratch) };
			uint32_t bindlessIndex = Demo::GetTextureCache().CacheTexture2D(
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
	SCOPED_CPU_EVENT("vmf_filtering", PIX_COLOR_DEFAULT);

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

	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"prefilter_normal_roughness", D3D12_COMMAND_LIST_TYPE_DIRECT);
	FFenceMarker gpuFinishFence = cmdList->GetFence(FCommandList::SyncPoint::GpuFinish);

	// Create source textures
	const size_t uploadSize = RenderBackend12::GetResourceSize(normalScratch) + RenderBackend12::GetResourceSize(metallicRoughnessScratch);
	FResourceUploadContext uploader{ uploadSize };

	std::unique_ptr<FTexture> srcNormalmap{ RenderBackend12::CreateNewTexture({
		.name = L"src_normalmap",
		.type = FTexture::Type::Tex2D,
		.alloc = FResource::Allocation::Transient(gpuFinishFence),
		.format = normalmapImage.format,
		.width = normalmapImage.width,
		.height = normalmapImage.height,
		.resourceState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		.upload = {
			.images = &normalmapImage,
			.context = &uploader 
		}
	})};

	std::unique_ptr<FTexture> srcMetallicRoughnessmap{ RenderBackend12::CreateNewTexture({
		.name = L"src_metallic_roughness",
		.type = FTexture::Type::Tex2D,
		.alloc = FResource::Allocation::Transient(gpuFinishFence),
		.format = metallicRoughnessImage.format,
		.width = metallicRoughnessImage.width,
		.height = metallicRoughnessImage.height,
		.resourceState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		.upload = {
			.images = &metallicRoughnessImage,
			.context = &uploader
		}
	})};

	// Create UAVs for prefiltering
	size_t normalmapMipCount = RenderUtils12::CalcMipCount(normalmapImage.width, normalmapImage.height, true);	
	std::unique_ptr<FShaderSurface> normalmapFilterUav{ RenderBackend12::CreateNewShaderSurface({
		.name = L"dest_normalmap",
		.type = FShaderSurface::Type::UAV,
		.alloc = FResource::Allocation::Transient(gpuFinishFence),
		.format = normalmapImage.format,
		.width = normalmapImage.width,
		.height = normalmapImage.height,
		.mipLevels = normalmapMipCount })};

	size_t metallicRoughnessMipCount = RenderUtils12::CalcMipCount(metallicRoughnessImage.width, metallicRoughnessImage.height, true);
	std::unique_ptr<FShaderSurface> metallicRoughnessFilterUav{ RenderBackend12::CreateNewShaderSurface({
		.name = L"dest_metallicRoughnessmap",
		.type = FShaderSurface::Type::UAV,
		.alloc = FResource::Allocation::Transient(gpuFinishFence),
		.format = metallicRoughnessImage.format,
		.width = metallicRoughnessImage.width,
		.height = metallicRoughnessImage.height,
		.mipLevels = metallicRoughnessMipCount })};

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
			FRootSignature::Desc{ L"content-pipeline/prefilter-normal-roughness.hlsl", L"rootsig", L"rootsig_1_1" });

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
				mipIndex, (uint32_t)normalmapImage.width, (uint32_t)normalmapImage.height, srcNormalmap->m_srvIndex, srcMetallicRoughnessmap->m_srvIndex, normalmapFilterUav->m_descriptorIndices.UAVs[mipIndex], metallicRoughnessFilterUav->m_descriptorIndices.UAVs[mipIndex]
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
	RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });
	FFenceMarker completionFence = cmdList->GetFence(FCommandList::SyncPoint::GpuFinish);

	// Initialize destination textures where the filtered results will be copied
	int normalmapSrvIndex = (int)Demo::GetTextureCache().CacheEmptyTexture2D(s2ws(normalmap.uri), normalmapCompressionFormat, normalmap.width, normalmap.height, normalmapMipCount);
	int metalRoughnessSrvIndex = (int)Demo::GetTextureCache().CacheEmptyTexture2D(s2ws(metallicRoughnessmap.uri), metalRoughnessCompressionFormat, metallicRoughnessmap.width, metallicRoughnessmap.height, metallicRoughnessMipCount);

	// Copy back normal texture and compress
	auto normalmapReadbackContext = std::make_shared<FResourceReadbackContext>(normalmapFilterUav->m_resource);
	FFenceMarker normalmapStageCompleteMarker = normalmapReadbackContext->StageSubresources(completionFence);
	auto normalmapProcessingJob = concurrency::create_task([normalmapStageCompleteMarker, this]()
		{
			normalmapStageCompleteMarker.Wait();
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
		FFenceMarker metallicRoughnessStageCompleteMarker = metallicRoughnessReadbackContext->StageSubresources(completionFence);
		auto metallicRoughnessProcessingJob = concurrency::create_task([metallicRoughnessStageCompleteMarker, this]()
			{
				metallicRoughnessStageCompleteMarker.Wait();
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
	if (Demo::GetConfig().UseContentCache)
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

	FResource* texResource = Demo::GetTextureCache().m_cachedTextures[s2ws(filename)]->m_resource;

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
		newCamera.m_name = PrintString("perspective_cam_%d", m_cameras.size());
		const tinygltf::PerspectiveCamera& cam = model.cameras[cameraIndex].perspective;
		newCamera.m_projectionTransform = Demo::Utils::GetReverseZInfinitePerspectiveFovLH(cam.yfov, cam.aspectRatio, Demo::GetConfig().CameraNearPlane);
	}
	else
	{
		newCamera.m_name = PrintString("ortho_cam_%d", m_cameras.size());
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

	concurrency::parallel_for(0, (int)model.lights.size(), [&](int i)
	{
		const tinygltf::Light& light = model.lights[i];
		m_globalLightList[i].m_color = Vector3(light.color[0], light.color[1], light.color[2]);
		m_globalLightList[i].m_intensity = light.intensity > 0.f ? light.intensity : 150;
		m_globalLightList[i].m_range = light.range;
		m_globalLightList[i].m_spotAngles = Vector2(light.spot.innerConeAngle, light.spot.outerConeAngle);

		if (light.type == "directional")
			m_globalLightList[i].m_type = Light::Directional;
		else if (light.type == "point")
			m_globalLightList[i].m_type = Light::Point;
		else if (light.type == "spot")
			m_globalLightList[i].m_type = Light::Spot;
	});

	FScene::s_loadProgress += FScene::s_lightsLoadTimeFrac;
}

void FScene::Clear()
{
	m_primitiveCount = 0;

	m_cameras.clear();
	m_meshBuffers.clear();
	m_blasList.clear();
	m_materialList.clear();
	m_sceneMeshes.Clear();
	m_sceneMeshDecals.Clear();
	m_sceneLights.Clear();

	m_packedMeshBufferViews.reset(nullptr);
	m_packedMeshAccessors.reset(nullptr);
	m_packedMaterials.reset(nullptr);
	m_tlas.reset(nullptr);
	m_dynamicSkyEnvmap.reset();
	m_dynamicSkySH.reset();
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

// See A.6 in https://courses.cs.duke.edu/cps124/fall02/resources/p91-preetham.pdf
void FScene::UpdateSunDirection()
{
	using namespace Demo;
	using namespace DirectX;

	int directionalLightIndex = GetDirectionalLight();

	if (GetConfig().ToD_Enable)
	{
		// latitude
		const float l = XMConvertToRadians(GetConfig().ToD_Latitude);

		// Solar declination
		const float delta = 0.4093f * std::sin(XM_2PI * (GetConfig().ToD_JulianDate - 81.f) / 368.f);

		const float sin_l = std::sin(l);
		const float cos_l = std::cos(l);
		const float sin_delta = std::sin(delta);
		const float cos_delta = std::cos(delta);
		const float t = XM_PI * GetConfig().ToD_DecimalHours / 12.f;
		const float sin_t = std::sin(t);
		const float cos_t = std::cos(t);

		// Elevation
		float theta = 0.5f * XM_PI - std::asin(sin_l * sin_delta - cos_l * cos_delta * cos_t);

		// Azimuth
		float phi = std::atan(-cos_delta * sin_t / (cos_l * sin_delta - sin_l * cos_delta * cos_t));

		const float sin_theta = std::sin(theta);
		const float cos_theta = std::cos(theta);
		const float sin_phi = std::sin(phi);
		const float cos_phi = std::cos(phi);

		// Sun dir based on Time of Day
		m_sunDir.x = sin_theta * cos_phi;
		m_sunDir.z = sin_theta * sin_phi;
		m_sunDir.y = cos_theta;
		m_sunDir.Normalize();

		// If a directional light is present, update its transform to match the time of day
		if (directionalLightIndex != -1)
		{
			Matrix& sunTransform = m_sceneLights.m_transformList[directionalLightIndex];
			sunTransform = Matrix::CreateWorld(Vector3::Zero, m_sunDir, Vector3::Up);
		}
	}
	else if (directionalLightIndex != -1)
	{
		// If time of day is disabled, use the directional light in the scene to 
		// reconstruct the sun direction
		Matrix& sunTransform = m_sceneLights.m_transformList[directionalLightIndex];
		sunTransform = m_originalSunTransform;
		m_sunDir = sunTransform.Forward();
	}
	else
	{
		// Otherwise, use a default direction for the sun (used by sky model even if a directional light is not active)
		m_sunDir = Vector3(1, 0.1, 1);
		m_sunDir.Normalize();
	}
}

void FScene::UpdateDynamicSky(bool bUseAsyncCompute)
{
	// This can be called several times when the TimeOfDay slider is adjusted.
	// So, add a cooldown so that a new update is not queued until the previous one has finished.
	static bool bReady = true;
	if (!bReady)
		return;

	bReady = false;
	D3D12_COMMAND_LIST_TYPE cmdListType = bUseAsyncCompute ? D3D12_COMMAND_LIST_TYPE_COMPUTE : D3D12_COMMAND_LIST_TYPE_DIRECT;
	FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"update_dynamic_sky", cmdListType);
	FFenceMarker gpuFinishFence = cmdList->GetFence(FCommandList::SyncPoint::GpuFinish);

	const int numSHCoefficients = 9;
	const DXGI_FORMAT shFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
	const DXGI_FORMAT radianceFormat = DXGI_FORMAT_R11G11B10_FLOAT;
	const int cubemapRes = Demo::GetConfig().EnvmapResolution;
	size_t numMips = RenderUtils12::CalcMipCount(cubemapRes, cubemapRes, false);

	std::shared_ptr<FShaderSurface> newEnvmap{ RenderBackend12::CreateNewShaderSurface({
		.name = L"dynamic_sky_envmap",
		.type = FShaderSurface::Type::UAV,
		.alloc = FResource::Allocation::Transient(gpuFinishFence),
		.format = radianceFormat,
		.width = (size_t)cubemapRes,
		.height = (size_t) cubemapRes,
		.mipLevels = numMips,
		.arraySize = 6 })};

	std::shared_ptr<FShaderSurface> newSH{ RenderBackend12::CreateNewShaderSurface({
		.name = L"dynamic_sky_sh",
		.type = FShaderSurface::Type::UAV,
		.alloc = FResource::Allocation::Transient(gpuFinishFence),
		.format = shFormat,
		.width = numSHCoefficients,
		.height = 1 })};

	{
		//FScopedGpuCapture capture(cmdList);

		if (bUseAsyncCompute)
		{
			Renderer::SyncQueueToBeginPass(cmdListType, Renderer::VisibilityPass);
		}

		SCOPED_COMMAND_QUEUE_EVENT(cmdList->m_type, "update_dynamic_sky", 0);

		// Render dynamic sky to 2D surface to a latlong texture
		const int resX = 2 * cubemapRes, resY = cubemapRes;
		std::unique_ptr<FShaderSurface> dynamicSkySurface{ RenderBackend12::CreateNewShaderSurface({
			.name = L"dynamic_sky_tex",
			.type = FShaderSurface::Type::UAV,
			.alloc = FResource::Allocation::Transient(gpuFinishFence),
			.format = radianceFormat,
			.width = (size_t)resX,
			.height = (size_t)resY,
			.mipLevels = numMips })};

		Renderer::GenerateDynamicSkyTexture(cmdList, dynamicSkySurface->m_descriptorIndices.UAVs[0], resX, resY, m_sunDir);

		// Downsample to generate mips
		{
			SCOPED_COMMAND_LIST_EVENT(cmdList, "mip_generation", 0);
			int mipResX = resX >> 1, mipResY = resY >> 1;
			int srcMip = 0;
			while (mipResY >= 1)
			{
				Renderer::DownsampleUav(cmdList, dynamicSkySurface->m_descriptorIndices.UAVs[srcMip], dynamicSkySurface->m_descriptorIndices.UAVs[srcMip + 1], mipResX, mipResY);
				dynamicSkySurface->m_resource->UavBarrier(cmdList);

				mipResX = mipResX >> 1;
				mipResY = mipResY >> 1;
				srcMip++;
			}
		}

		// Convert to cubemap
		const size_t cubemapSize = cubemapRes;
		std::unique_ptr<FShaderSurface> texCubeUav{ RenderBackend12::CreateNewShaderSurface({
			.name = L"src_cubemap",
			.type = FShaderSurface::Type::UAV,
			.alloc = FResource::Allocation::Transient(gpuFinishFence),
			.format = radianceFormat,
			.width = cubemapSize,
			.height = cubemapSize,
			.mipLevels = numMips,
			.arraySize = 6 })};

		Renderer::ConvertLatlong2Cubemap(cmdList, dynamicSkySurface->m_descriptorIndices.SRV, texCubeUav->m_descriptorIndices.UAVs, cubemapSize, numMips);

		// Prefilter the cubemap
		texCubeUav->m_resource->Transition(cmdList, texCubeUav->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		Renderer::PrefilterCubemap(cmdList, texCubeUav->m_descriptorIndices.SRV, newEnvmap->m_descriptorIndices.UAVs, cubemapRes, 0, numMips);

		// SH Encode
		Renderer::ShEncode(cmdList, newSH.get(), dynamicSkySurface->m_descriptorIndices.SRV, shFormat, resX, resY);

		// Transition to read state
		newEnvmap->m_resource->Transition(cmdList, newEnvmap->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		newSH->m_resource->Transition(cmdList, newSH->m_resource->GetTransitionToken(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		RenderBackend12::ExecuteCommandlists(cmdListType, { cmdList });
	}

	// Update reference when the update is complete on the GPU
	concurrency::create_task([gpuFinishFence]()
	{
		// Wait for the update to finish
		gpuFinishFence.Wait();
	}).then([bUseAsyncCompute]()
	{
		// Wait till the end of the frame before we flush and swap the envmap
		if (bUseAsyncCompute)
		{
			RenderBackend12::GetCurrentFrameFence().Wait();
		}
	}).then([this, newEnvmap, newSH]() mutable
	{
		// Swap with new envmap
		Renderer::Status::Pause();
		m_dynamicSkyEnvmap = newEnvmap;
		m_dynamicSkySH = newSH;
		m_skylight.m_envmapTextureIndex = m_dynamicSkyEnvmap->m_descriptorIndices.SRV;
		m_skylight.m_shTextureIndex = m_dynamicSkySH->m_descriptorIndices.SRV;
		bReady = true;
		Renderer::Status::Resume();
	});
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