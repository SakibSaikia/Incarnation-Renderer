#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <mesh-utils.h>
#include <MikkTSpace/mikktspace.h>
#include <ppl.h>
#include <profiling.h>

namespace
{
	inline void DebugAssert(bool success, const char* msg = nullptr)
	{
#if defined _DEBUG
		if (!success)
		{
			if (msg)
			{
				OutputDebugStringA("\n*****\n");
				OutputDebugStringA(msg);
				OutputDebugStringA("\n*****\n");
			}

			_CrtDbgBreak();
		}
#endif
	}

	struct PrimitiveIdentifier
	{
		tinygltf::Model* m_model;
		int m_meshIndex;
		int m_primitiveIndex;
	};

	float* GetAttributeData(std::string attributeName, const SMikkTSpaceContext* context, const int face, const int vert)
	{
		const size_t indexBufferIdx = face * 3 + vert;

		PrimitiveIdentifier* primId = (PrimitiveIdentifier*)context->m_pUserData;
		tinygltf::Model* model = primId->m_model;
		tinygltf::Mesh& mesh = model->meshes[primId->m_meshIndex];
		tinygltf::Primitive& primitive = mesh.primitives[primId->m_primitiveIndex];

		tinygltf::Accessor& indexAccessor = model->accessors[primitive.indices];
		tinygltf::BufferView& indexBufferView = model->bufferViews[indexAccessor.bufferView];
		tinygltf::Buffer& indexBuffer = model->buffers[indexBufferView.buffer];

		auto vertexIt = primitive.attributes.find(attributeName);
		tinygltf::Accessor& vertexAccessor = model->accessors[vertexIt->second];
		tinygltf::BufferView& vertexBufferView = model->bufferViews[vertexAccessor.bufferView];
		tinygltf::Buffer& vertexBuffer = model->buffers[vertexBufferView.buffer];

		size_t indexSize = tinygltf::GetComponentSizeInBytes(indexAccessor.componentType) * tinygltf::GetNumComponentsInType(indexAccessor.type);
		uint32_t index;
		if (indexSize == sizeof(uint16_t))
		{
			uint16_t* indices = (uint16_t*)(indexBuffer.data.data() + indexBufferView.byteOffset + indexAccessor.byteOffset);
			index = (uint32_t)indices[indexBufferIdx];
		}
		else
		{
			DebugAssert(indexSize == sizeof(uint32_t));
			uint32_t* indices = (uint32_t*)(indexBuffer.data.data() + indexBufferView.byteOffset + indexAccessor.byteOffset);
			index = indices[indexBufferIdx];
		}

		float* verts = (float*)(vertexBuffer.data.data() + vertexBufferView.byteOffset + vertexAccessor.byteOffset);
		return &verts[index * tinygltf::GetNumComponentsInType(vertexAccessor.type)];
	}

	int GetNumFaces(const SMikkTSpaceContext* context)
	{
		PrimitiveIdentifier* primId = (PrimitiveIdentifier*)context->m_pUserData;

		const tinygltf::Model* model = primId->m_model;
		const tinygltf::Mesh& mesh = model->meshes[primId->m_meshIndex];
		const tinygltf::Primitive& primitive = mesh.primitives[primId->m_primitiveIndex];

		const tinygltf::Accessor& indexAccessor = model->accessors[primitive.indices];
		DebugAssert(indexAccessor.count % 3 == 0);
		return indexAccessor.count / 3;
	}

	int GetNumVerticesOfFace(const SMikkTSpaceContext* context, const int face)
	{
		return 3;
	}

	void GetPosition(const SMikkTSpaceContext* context, float posOut[], const int face, const int vert)
	{
		float* pData = GetAttributeData("POSITION", context, face, vert);
		posOut[0] = pData[0];
		posOut[1] = pData[1];
		posOut[2] = pData[2];
	}

	void GetNormal(const SMikkTSpaceContext* context, float normalOut[], const int face, const int vert)
	{
		float* pData = GetAttributeData("NORMAL", context, face, vert);
		normalOut[0] = pData[0];
		normalOut[1] = pData[1];
		normalOut[2] = pData[2];
	}

	void GetUV(const SMikkTSpaceContext* context, float uvOut[], const int face, const int vert)
	{
		float* pData = GetAttributeData("TEXCOORD_0", context, face, vert);
		uvOut[0] = pData[0];
		uvOut[1] = pData[1];
	}

	void SetTSpaceBasic(const SMikkTSpaceContext* context, const float tangent[], const float sign, const int face, const int vert)
	{
		float* pData = GetAttributeData("TANGENT", context, face, vert);
		pData[0] = tangent[0];
		pData[1] = tangent[1];
		pData[2] = tangent[2];
		pData[3] = sign;
	}
}

bool MeshUtils::FixupMeshes(tinygltf::Model& model)
{
	SCOPED_CPU_EVENT("fixup_meshes", PIX_COLOR_DEFAULT);

	bool requiresResave = false;
	std::vector<PrimitiveIdentifier> fixupPrimitives;

	for (int meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex)
	{
		tinygltf::Mesh& mesh = model.meshes[meshIndex];

		for (int primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
		{
			// Generate tangents if the material requires a normal map and the mesh doesn't include tangents
			tinygltf::Primitive& primitive = mesh.primitives[primitiveIndex];
			tinygltf::Material material = model.materials[primitive.material];
			if (material.normalTexture.index != -1 && primitive.attributes.find("TANGENT") == primitive.attributes.cend())
			{
				// Get vert count based on the POSITION accessor
				auto posIt = primitive.attributes.find("POSITION");
				const tinygltf::Accessor& positionAccessor = model.accessors[posIt->second];
				size_t vertCount = positionAccessor.count;

				// New Tangent Buffer
				model.buffers.emplace_back();

				// New Tangent Buffer View
				tinygltf::BufferView tangentBufferView = {};
				tangentBufferView.buffer = model.buffers.size() - 1;
				model.bufferViews.push_back(tangentBufferView);

				// New Tangent Accessor
				tinygltf::Accessor tangentAccessor = {};
				tangentAccessor.bufferView = model.bufferViews.size() - 1;
				tangentAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
				tangentAccessor.type = TINYGLTF_TYPE_VEC4;
				tangentAccessor.count = vertCount;
				model.accessors.push_back(tangentAccessor);

				// New Tangent Attribute
				primitive.attributes["TANGENT"] = model.accessors.size() - 1;

				// Resize buffer
				size_t tangentSize = tinygltf::GetComponentSizeInBytes(tangentAccessor.componentType) * tinygltf::GetNumComponentsInType(tangentAccessor.type);
				model.buffers.back().data.resize(vertCount * tangentSize);

				PrimitiveIdentifier primId = {};
				primId.m_model = &model;
				primId.m_meshIndex = meshIndex;
				primId.m_primitiveIndex = primitiveIndex;
				fixupPrimitives.push_back(primId);

				requiresResave |= true;
			}
		}
	}

	concurrency::parallel_for_each(fixupPrimitives.begin(), fixupPrimitives.end(), [](PrimitiveIdentifier& primitive)
	{
		// Initialize MikkTSpace
		SMikkTSpaceInterface tspaceInterface = {};
		tspaceInterface.m_getNumFaces = &GetNumFaces;
		tspaceInterface.m_getNumVerticesOfFace = &GetNumVerticesOfFace;
		tspaceInterface.m_getPosition = &GetPosition;
		tspaceInterface.m_getNormal = &GetNormal;
		tspaceInterface.m_getTexCoord = &GetUV;
		tspaceInterface.m_setTSpaceBasic = &SetTSpaceBasic;

		SMikkTSpaceContext tspaceContext = {};
		tspaceContext.m_pInterface = &tspaceInterface;
		tspaceContext.m_pUserData = &primitive;

		// Generate TSpace
		genTangSpaceDefault(&tspaceContext);
	});


	return requiresResave;
}