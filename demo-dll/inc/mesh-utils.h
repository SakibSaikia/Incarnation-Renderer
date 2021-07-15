#pragma once
#include <tiny_gltf.h>

namespace MeshUtils
{
	void CleanupMesh(int meshIndex, tinygltf::Model& model);
}