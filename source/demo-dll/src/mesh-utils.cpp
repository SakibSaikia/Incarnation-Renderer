// Includes meshlet generation code from https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/Samples/Desktop/D3D12MeshShaders

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <mesh-utils.h>
#include <MikkTSpace/mikktspace.h>
#include <ppl.h>
#include <profiling.h>
#include <common.h>
#include <SimpleMath.h>
#include <unordered_map>
#include <unordered_set>
#include <memory>

using namespace DirectX;
using namespace DirectX::SimpleMath;

namespace
{
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

    XMVECTOR MinimumBoundingSphere(XMFLOAT3* points, uint32_t count)
    {
        assert(points != nullptr && count != 0);

        // Find the min & max points indices along each axis.
        uint32_t minAxis[3] = { 0, 0, 0 };
        uint32_t maxAxis[3] = { 0, 0, 0 };

        for (uint32_t i = 1; i < count; ++i)
        {
            float* point = (float*)(points + i);

            for (uint32_t j = 0; j < 3; ++j)
            {
                float* min = (float*)(&points[minAxis[j]]);
                float* max = (float*)(&points[maxAxis[j]]);

                minAxis[j] = point[j] < min[j] ? i : minAxis[j];
                maxAxis[j] = point[j] > max[j] ? i : maxAxis[j];
            }
        }

        // Find axis with maximum span.
        XMVECTOR distSqMax = g_XMZero;
        uint32_t axis = 0;

        for (uint32_t i = 0; i < 3u; ++i)
        {
            XMVECTOR min = XMLoadFloat3(&points[minAxis[i]]);
            XMVECTOR max = XMLoadFloat3(&points[maxAxis[i]]);

            XMVECTOR distSq = XMVector3LengthSq(max - min);
            if (XMVector3Greater(distSq, distSqMax))
            {
                distSqMax = distSq;
                axis = i;
            }
        }

        // Calculate an initial starting center point & radius.
        XMVECTOR p1 = XMLoadFloat3(&points[minAxis[axis]]);
        XMVECTOR p2 = XMLoadFloat3(&points[maxAxis[axis]]);

        XMVECTOR center = (p1 + p2) * 0.5f;
        XMVECTOR radius = XMVector3Length(p2 - p1) * 0.5f;
        XMVECTOR radiusSq = radius * radius;

        // Add all our points to bounding sphere expanding radius & recalculating center point as necessary.
        for (uint32_t i = 0; i < count; ++i)
        {
            XMVECTOR point = XMLoadFloat3(points + i);
            XMVECTOR distSq = XMVector3LengthSq(point - center);

            if (XMVector3Greater(distSq, radiusSq))
            {
                XMVECTOR dist = XMVectorSqrt(distSq);
                XMVECTOR k = (radius / dist) * 0.5f + XMVectorReplicate(0.5f);

                center = center * k + point * (g_XMOne - k);
                radius = (radius + dist) * 0.5f;
            }
        }

        // Populate a single XMVECTOR with center & radius data.
        XMVECTOR select0001 = XMVectorSelectControl(0, 0, 0, 1);
        return XMVectorSelect(center, radius, select0001);
    }

    struct EdgeEntry
    {
        uint32_t   i0;
        uint32_t   i1;
        uint32_t   i2;

        uint32_t   Face;
        EdgeEntry* Next;
    };

    template <typename T>
    inline size_t Hash(const T& val)
    {
        return std::hash<T>()(val);
    }

    size_t CRCHash(const uint32_t* dwords, uint32_t dwordCount)
    {
        size_t h = 0;

        for (uint32_t i = 0; i < dwordCount; ++i)
        {
            uint32_t highOrd = h & 0xf8000000;
            h = h << 5;
            h = h ^ (highOrd >> 27);
            h = h ^ size_t(dwords[i]);
        }

        return h;
    }

    // Sort in reverse order to use vector as a queue with pop_back.
    bool CompareScores(const std::pair<uint32_t, float>& a, const std::pair<uint32_t, float>& b)
    {
        return a.second > b.second;
    }

    // Compute number of triangle vertices already exist in the meshlet
    uint32_t ComputeReuse(const FInlineMeshlet& meshlet, uint32_t(&triIndices)[3])
    {
        uint32_t count = 0;

        for (uint32_t i = 0; i < static_cast<uint32_t>(meshlet.m_uniqueVertexIndices.size()); ++i)
        {
            for (uint32_t j = 0; j < 3u; ++j)
            {
                if (meshlet.m_uniqueVertexIndices[i] == triIndices[j])
                {
                    ++count;
                }
            }
        }

        return count;
    }

    XMVECTOR ComputeNormal(XMFLOAT3* tri)
    {
        XMVECTOR p0 = XMLoadFloat3(&tri[0]);
        XMVECTOR p1 = XMLoadFloat3(&tri[1]);
        XMVECTOR p2 = XMLoadFloat3(&tri[2]);

        XMVECTOR v01 = p0 - p1;
        XMVECTOR v02 = p0 - p2;

        return XMVector3Normalize(XMVector3Cross(v01, v02));
    }

    // Computes a candidacy score based on spatial locality, orientational coherence, and vertex re-use within a meshlet.
    float ComputeScore(const FInlineMeshlet& meshlet, XMVECTOR sphere, XMVECTOR normal, uint32_t(&triIndices)[3], XMFLOAT3* triVerts)
    {
        const float reuseWeight = 0.334f;
        const float locWeight = 0.333f;
        const float oriWeight = 0.333f;

        // Vertex reuse
        uint32_t reuse = ComputeReuse(meshlet, triIndices);
        XMVECTOR reuseScore = g_XMOne - (XMVectorReplicate(float(reuse)) / 3.0f);

        // Distance from center point
        XMVECTOR maxSq = g_XMZero;
        for (uint32_t i = 0; i < 3u; ++i)
        {
            XMVECTOR v = sphere - XMLoadFloat3(&triVerts[i]);
            maxSq = XMVectorMax(maxSq, XMVector3Dot(v, v));
        }
        XMVECTOR r = XMVectorSplatW(sphere);
        XMVECTOR r2 = r * r;
        XMVECTOR locScore = XMVectorLog(maxSq / r2 + g_XMOne);

        // Angle between normal and meshlet cone axis
        XMVECTOR n = ComputeNormal(triVerts);
        XMVECTOR d = XMVector3Dot(n, normal);
        XMVECTOR oriScore = (-d + g_XMOne) / 2.0f;

        XMVECTOR b = reuseWeight * reuseScore + locWeight * locScore + oriWeight * oriScore;

        return XMVectorGetX(b);
    }

    template <typename T>
    void BuildAdjacencyList(
        const T* indices, uint32_t indexCount,
        const XMFLOAT3* positions, uint32_t vertexCount,
        uint32_t* adjacency)
    {
        const uint32_t triCount = indexCount / 3;
        // Find point reps (unique positions) in the position stream
        // Create a mapping of non-unique vertex indices to point reps
        std::vector<T> pointRep;
        pointRep.resize(vertexCount);

        std::unordered_map<size_t, T> uniquePositionMap;
        uniquePositionMap.reserve(vertexCount);

        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            XMFLOAT3 position = *(positions + i);
            size_t hash = Hash(position);

            auto it = uniquePositionMap.find(hash);
            if (it != uniquePositionMap.end())
            {
                // Position already encountered - reference previous index
                pointRep[i] = it->second;
            }
            else
            {
                // New position found - add to hash table and LUT
                uniquePositionMap.insert(std::make_pair(hash, static_cast<T>(i)));
                pointRep[i] = static_cast<T>(i);
            }
        }

        // Create a linked list of edges for each vertex to determine adjacency
        const uint32_t hashSize = vertexCount / 3;

        std::unique_ptr<EdgeEntry* []> hashTable(new EdgeEntry * [hashSize]);
        std::unique_ptr<EdgeEntry[]> entries(new EdgeEntry[triCount * 3]);

        std::memset(hashTable.get(), 0, sizeof(EdgeEntry*) * hashSize);
        uint32_t entryIndex = 0;

        for (uint32_t iFace = 0; iFace < triCount; ++iFace)
        {
            uint32_t index = iFace * 3;

            // Create a hash entry in the hash table for each each.
            for (uint32_t iEdge = 0; iEdge < 3; ++iEdge)
            {
                T i0 = pointRep[indices[index + (iEdge % 3)]];
                T i1 = pointRep[indices[index + ((iEdge + 1) % 3)]];
                T i2 = pointRep[indices[index + ((iEdge + 2) % 3)]];

                auto& entry = entries[entryIndex++];
                entry.i0 = i0;
                entry.i1 = i1;
                entry.i2 = i2;

                uint32_t key = entry.i0 % hashSize;

                entry.Next = hashTable[key];
                entry.Face = iFace;

                hashTable[key] = &entry;
            }
        }


        // Initialize the adjacency list
        std::memset(adjacency, uint32_t(-1), indexCount * sizeof(uint32_t));

        for (uint32_t iFace = 0; iFace < triCount; ++iFace)
        {
            uint32_t index = iFace * 3;

            for (uint32_t point = 0; point < 3; ++point)
            {
                if (adjacency[iFace * 3 + point] != uint32_t(-1))
                    continue;

                // Look for edges directed in the opposite direction.
                T i0 = pointRep[indices[index + ((point + 1) % 3)]];
                T i1 = pointRep[indices[index + (point % 3)]];
                T i2 = pointRep[indices[index + ((point + 2) % 3)]];

                // Find a face sharing this edge
                uint32_t key = i0 % hashSize;

                EdgeEntry* found = nullptr;
                EdgeEntry* foundPrev = nullptr;

                for (EdgeEntry* current = hashTable[key], *prev = nullptr; current != nullptr; prev = current, current = current->Next)
                {
                    if (current->i1 == i1 && current->i0 == i0)
                    {
                        found = current;
                        foundPrev = prev;
                        break;
                    }
                }

                // Cache this face's normal
                XMVECTOR n0;
                {
                    XMVECTOR p0 = XMLoadFloat3(&positions[i1]);
                    XMVECTOR p1 = XMLoadFloat3(&positions[i0]);
                    XMVECTOR p2 = XMLoadFloat3(&positions[i2]);

                    XMVECTOR e0 = p0 - p1;
                    XMVECTOR e1 = p1 - p2;

                    n0 = XMVector3Normalize(XMVector3Cross(e0, e1));
                }

                // Use face normal dot product to determine best edge-sharing candidate.
                float bestDot = -2.0f;
                for (EdgeEntry* current = found, *prev = foundPrev; current != nullptr; prev = current, current = current->Next)
                {
                    if (bestDot == -2.0f || (current->i1 == i1 && current->i0 == i0))
                    {
                        XMVECTOR p0 = XMLoadFloat3(&positions[current->i0]);
                        XMVECTOR p1 = XMLoadFloat3(&positions[current->i1]);
                        XMVECTOR p2 = XMLoadFloat3(&positions[current->i2]);

                        XMVECTOR e0 = p0 - p1;
                        XMVECTOR e1 = p1 - p2;

                        XMVECTOR n1 = XMVector3Normalize(XMVector3Cross(e0, e1));

                        float dot = XMVectorGetX(XMVector3Dot(n0, n1));

                        if (dot > bestDot)
                        {
                            found = current;
                            foundPrev = prev;
                            bestDot = dot;
                        }
                    }
                }

                // Update hash table and adjacency list
                if (found && found->Face != uint32_t(-1))
                {
                    // Erase the found from the hash table linked list.
                    if (foundPrev != nullptr)
                    {
                        foundPrev->Next = found->Next;
                    }
                    else
                    {
                        hashTable[key] = found->Next;
                    }

                    // Update adjacency information
                    adjacency[iFace * 3 + point] = found->Face;

                    // Search & remove this face from the table linked list
                    uint32_t key2 = i1 % hashSize;

                    for (EdgeEntry* current = hashTable[key2], *prev = nullptr; current != nullptr; prev = current, current = current->Next)
                    {
                        if (current->Face == iFace && current->i1 == i0 && current->i0 == i1)
                        {
                            if (prev != nullptr)
                            {
                                prev->Next = current->Next;
                            }
                            else
                            {
                                hashTable[key2] = current->Next;
                            }

                            break;
                        }
                    }

                    bool linked = false;
                    for (uint32_t point2 = 0; point2 < point; ++point2)
                    {
                        if (found->Face == adjacency[iFace * 3 + point2])
                        {
                            linked = true;
                            adjacency[iFace * 3 + point] = uint32_t(-1);
                            break;
                        }
                    }

                    if (!linked)
                    {
                        uint32_t edge2 = 0;
                        for (; edge2 < 3; ++edge2)
                        {
                            T k = indices[found->Face * 3 + edge2];
                            if (k == uint32_t(-1))
                                continue;

                            if (pointRep[k] == i0)
                                break;
                        }

                        if (edge2 < 3)
                        {
                            adjacency[found->Face * 3 + edge2] = iFace;
                        }
                    }
                }
            }
        }
    }

    // Determines whether a candidate triangle can be added to a specific meshlet; if it can, does so.
    bool AddToMeshlet(uint32_t maxVerts, uint32_t maxPrims, FInlineMeshlet& meshlet, uint32_t(&tri)[3])
    {
        // Are we already full of vertices?
        if (meshlet.m_uniqueVertexIndices.size() == maxVerts)
            return false;

        // Are we full, or can we store an additional primitive?
        if (meshlet.m_primitiveIndices.size() == maxPrims)
            return false;

        static const uint32_t Undef = uint32_t(-1);
        uint32_t indices[3] = { Undef, Undef, Undef };
        uint32_t newCount = 3;

        for (uint32_t i = 0; i < meshlet.m_uniqueVertexIndices.size(); ++i)
        {
            for (uint32_t j = 0; j < 3; ++j)
            {
                if (meshlet.m_uniqueVertexIndices[i] == tri[j])
                {
                    indices[j] = i;
                    --newCount;
                }
            }
        }

        // Will this triangle fit?
        if (meshlet.m_uniqueVertexIndices.size() + newCount > maxVerts)
            return false;

        // Add unique vertex indices to unique vertex index list
        for (uint32_t j = 0; j < 3; ++j)
        {
            if (indices[j] == Undef)
            {
                indices[j] = static_cast<uint32_t>(meshlet.m_uniqueVertexIndices.size());
                meshlet.m_uniqueVertexIndices.push_back(tri[j]);
            }
        }

        // Add the new primitive 
        typename FInlineMeshlet::FPackedTriangle prim = {};
        prim.i0 = indices[0];
        prim.i1 = indices[1];
        prim.i2 = indices[2];

        meshlet.m_primitiveIndices.push_back(prim);

        return true;
    }

    bool IsMeshletFull(uint32_t maxVerts, uint32_t maxPrims, const FInlineMeshlet& meshlet)
    {
        assert(meshlet.m_uniqueVertexIndices.size() <= maxVerts);
        assert(meshlet.m_primitiveIndices.size() <= maxPrims);

        return meshlet.m_uniqueVertexIndices.size() == maxVerts
            || meshlet.m_primitiveIndices.size() == maxPrims;
    }
}

namespace std
{
    template <> struct hash<XMFLOAT3> { size_t operator()(const XMFLOAT3& v) const { return CRCHash(reinterpret_cast<const uint32_t*>(&v), sizeof(v) / 4); } };
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

void MeshUtils::Meshletize(
    uint32_t maxVerts, uint32_t maxPrims,
    const uint32_t* indices, uint32_t indexCount,
    const XMFLOAT3* positions, uint32_t vertexCount,
    std::vector<FInlineMeshlet>& output
)
{
    const uint32_t triCount = indexCount / 3;

    // Build a primitive adjacency list
    std::vector<uint32_t> adjacency;
    adjacency.resize(indexCount);

    BuildAdjacencyList(indices, indexCount, positions, vertexCount, adjacency.data());

    // Rest our outputs
    output.clear();
    output.emplace_back();
    auto* curr = &output.back();

    // Bitmask of all triangles in mesh to determine whether a specific one has been added.
    std::vector<bool> checklist;
    checklist.resize(triCount);

    std::vector<XMFLOAT3> m_positions;
    std::vector<XMFLOAT3> normals;
    std::vector<std::pair<uint32_t, float>> candidates;
    std::unordered_set<uint32_t> candidateCheck;

    XMVECTOR psphere, normal;

    // Arbitrarily start at triangle zero.
    uint32_t triIndex = 0;
    candidates.push_back(std::make_pair(triIndex, 0.0f));
    candidateCheck.insert(triIndex);

    // Continue adding triangles until 
    while (!candidates.empty())
    {
        uint32_t index = candidates.back().first;
        candidates.pop_back();

        uint32_t tri[3] =
        {
            indices[index * 3 + 2],
            indices[index * 3 + 1],
            indices[index * 3],
        };

        assert(tri[0] < vertexCount);
        assert(tri[1] < vertexCount);
        assert(tri[2] < vertexCount);

        // Try to add triangle to meshlet
        if (AddToMeshlet(maxVerts, maxPrims, *curr, tri))
        {
            // Success! Mark as added.
            checklist[index] = true;

            // Add m_positions & normal to list
            XMFLOAT3 points[3] =
            {
                positions[tri[0]],
                positions[tri[1]],
                positions[tri[2]],
            };

            m_positions.push_back(points[0]);
            m_positions.push_back(points[1]);
            m_positions.push_back(points[2]);

            XMFLOAT3 Normal;
            XMStoreFloat3(&Normal, ComputeNormal(points));
            normals.push_back(Normal);

            // Compute new bounding sphere & normal axis
            psphere = MinimumBoundingSphere(m_positions.data(), static_cast<uint32_t>(m_positions.size()));
            
            // SRS - Cache the bounding sphere for visibility checks
            XMFLOAT4 currBounds;
            XMStoreFloat4(&currBounds, psphere);
            curr->m_boundingSphere = BoundingSphere(XMFLOAT3{ currBounds.x, currBounds.y, currBounds.z }, currBounds.w);

            XMVECTOR nsphere = MinimumBoundingSphere(normals.data(), static_cast<uint32_t>(normals.size()));
            normal = XMVector3Normalize(nsphere);

            // Find and add all applicable adjacent triangles to candidate list
            const uint32_t adjIndex = index * 3;

            uint32_t adj[3] =
            {
                adjacency[adjIndex],
                adjacency[adjIndex + 1],
                adjacency[adjIndex + 2],
            };

            for (uint32_t i = 0; i < 3u; ++i)
            {
                // Invalid triangle in adjacency slot
                if (adj[i] == -1)
                    continue;

                // Already processed triangle
                if (checklist[adj[i]])
                    continue;

                // Triangle already in the candidate list
                if (candidateCheck.count(adj[i]))
                    continue;

                candidates.push_back(std::make_pair(adj[i], FLT_MAX));
                candidateCheck.insert(adj[i]);
            }

            // Re-score remaining candidate triangles
            for (uint32_t i = 0; i < static_cast<uint32_t>(candidates.size()); ++i)
            {
                uint32_t candidate = candidates[i].first;

                uint32_t triIndices[3] =
                {
                    indices[candidate * 3],
                    indices[candidate * 3 + 1],
                    indices[candidate * 3 + 2],
                };

                assert(triIndices[0] < vertexCount);
                assert(triIndices[1] < vertexCount);
                assert(triIndices[2] < vertexCount);

                XMFLOAT3 triVerts[3] =
                {
                    positions[triIndices[0]],
                    positions[triIndices[1]],
                    positions[triIndices[2]],
                };

                candidates[i].second = ComputeScore(*curr, psphere, normal, triIndices, triVerts);
            }

            // Determine whether we need to move to the next meshlet.
            if (IsMeshletFull(maxVerts, maxPrims, *curr))
            {
                m_positions.clear();
                normals.clear();
                candidateCheck.clear();

                // Use one of our existing candidates as the next meshlet seed.
                if (!candidates.empty())
                {
                    candidates[0] = candidates.back();
                    candidates.resize(1);
                    candidateCheck.insert(candidates[0].first);
                }

                output.emplace_back();
                curr = &output.back();
            }
            else
            {
                std::sort(candidates.begin(), candidates.end(), &CompareScores);
            }
        }
        else
        {
            if (candidates.empty())
            {
                m_positions.clear();
                normals.clear();
                candidateCheck.clear();

                output.emplace_back();
                curr = &output.back();
            }
        }

        // Ran out of candidates; add a new seed candidate to start the next meshlet.
        if (candidates.empty())
        {
            while (triIndex < triCount && checklist[triIndex])
                ++triIndex;

            if (triIndex == triCount)
                break;

            candidates.push_back(std::make_pair(triIndex, 0.0f));
            candidateCheck.insert(triIndex);
        }
    }

    // The last meshlet may have never had any primitives added to it - in which case we want to remove it.
    if (output.back().m_primitiveIndices.empty())
    {
        output.pop_back();
    }
}