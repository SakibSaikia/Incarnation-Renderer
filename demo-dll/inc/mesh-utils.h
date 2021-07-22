#pragma once
#include <tiny_gltf.h>

namespace MeshUtils
{
	bool CleanupMesh(int meshIndex, tinygltf::Model& model);
}