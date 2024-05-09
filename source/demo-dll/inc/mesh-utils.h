#pragma once
#include <tiny_gltf.h>
#include <SimpleMath.h>
using namespace DirectX;

struct FInlineMeshlet
{
	struct FPackedTriangle
	{
		uint32_t i0 : 10;
		uint32_t i1 : 10;
		uint32_t i2 : 10;
		uint32_t spare : 2;
	};

	// Deduplicated vertex indices
	// For example, if the original index buffer is { 4,5,6, 8,4,6, ...}, unique vertex indices become { 4,5,6,  8, ...}
	std::vector<uint32_t> m_uniqueVertexIndices;

	// Primitive indices are local per meshlet. For the above example, primitive indices are { 0,1,2,  3,0,2, ...}
	std::vector<FPackedTriangle> m_primitiveIndices;

	DirectX::BoundingSphere m_boundingSphere;
};

namespace MeshUtils
{
	bool FixupMeshes(tinygltf::Model& model);

    void Meshletize(
        uint32_t maxVerts, uint32_t maxPrims,
        const uint32_t* indices, uint32_t indexCount,
        const XMFLOAT3* positions, uint32_t vertexCount,
        std::vector<struct FInlineMeshlet>& output);
}