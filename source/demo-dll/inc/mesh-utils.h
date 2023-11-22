#pragma once
#include <tiny_gltf.h>

#include <SimpleMath.h>
using namespace DirectX;

template <typename T>
struct InlineMeshlet
{
    struct PackedTriangle
    {
        uint32_t i0 : 10;
        uint32_t i1 : 10;
        uint32_t i2 : 10;
        uint32_t spare : 2;
    };

    std::vector<T> UniqueVertexIndices;
    std::vector<PackedTriangle> PrimitiveIndices;
};

namespace MeshUtils
{
	bool FixupMeshes(tinygltf::Model& model);

    template <typename T>
    void Meshletize(
        uint32_t maxVerts, uint32_t maxPrims,
        const T* indices, uint32_t indexCount,
        const XMFLOAT3* positions, uint32_t vertexCount,
        std::vector<InlineMeshlet<T>>& output);
}