#pragma once

#include <SimpleMath.h>
using namespace DirectX::SimpleMath;

namespace tinygltf
{
	class Model;
}

struct FRenderMesh
{
	std::string m_name;
	size_t m_indexCount;
	uint32_t m_indexOffset;
	uint32_t m_positionOffset;
	uint32_t m_tangentBasisOffset;
	uint32_t m_uvOffset;
};

struct FCamera
{
	std::string m_name;
	Matrix m_viewTransform;
	Matrix m_projectionTransform;
};

struct FScene
{
	void Reload(const char* filePath);
	void LoadNode(int nodeIndex, const tinygltf::Model& model, const Matrix& transform);
	void LoadMesh(int meshIndex, const tinygltf::Model& model, const Matrix& transform);
	void LoadCamera(int meshIndex, const tinygltf::Model& model, const Matrix& transform);
	void Clear();

	// Scene entity lists
	std::vector<FRenderMesh> m_meshGeo;
	std::vector<Matrix> m_meshTransforms;
	std::vector<DirectX::BoundingBox> m_meshBounds; // object space
	std::vector<FCamera> m_cameras;

	// Scene geo
	std::unique_ptr<FBindlessResource> m_meshIndexBuffer;
	std::unique_ptr<FBindlessResource> m_meshPositionBuffer;
	std::unique_ptr<FBindlessResource> m_meshTangentBasisBuffer;
	std::unique_ptr<FBindlessResource> m_meshUVBuffer;
	DirectX::BoundingBox m_sceneBounds; // world space

private:
	uint8_t* m_scratchIndexBuffer;
	uint8_t* m_scratchPositionBuffer;
	uint8_t* m_scratchTangentBasisBuffer;
	uint8_t* m_scratchUVBuffer;

	size_t m_scratchIndexBufferOffset;
	size_t m_scratchPositionBufferOffset;
	size_t m_scratchTangentBasisBufferOffset;
	size_t m_scratchUVBufferOffset;
};

struct FView
{
	void Tick();
	void Reset(const FScene& scene);

	Matrix m_viewTransform;
	Matrix m_projectionTransform;
};