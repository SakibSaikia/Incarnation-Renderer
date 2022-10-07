#include "gpu-shared-types.h"

// Helper struct that *must* alias FIndirectDrawWithRootConstants
struct FIndirectDebugDrawCmd
{
	float4 m_color;
	float4x4 m_transform;
	uint m_shapeType;
	uint3 __pad0;
	uint4 __pad1;
	uint4 __pad2;

	FDrawInstanced m_drawArguments;
};

uint GetIndexCount(uint shapeType)
{
	ByteAddressBuffer indexCountBuffer = ResourceDescriptorHeap[SpecialDescriptors::DebugPrimitiveIndexCountSrvIndex];
	return indexCountBuffer.Load<uint>(shapeType * sizeof(uint));
}

void DrawDebug(uint shapeType, float4 color, float4x4 transform)
{
	FIndirectDebugDrawCmd cmd = (FIndirectDebugDrawCmd)0;

	cmd.m_color = color;
	cmd.m_transform = transform;
	cmd.m_shapeType = shapeType;

	cmd.m_drawArguments.m_vertexCount = GetIndexCount(shapeType);
	cmd.m_drawArguments.m_instanceCount = 1;
	cmd.m_drawArguments.m_startVertexLocation = 0;
	cmd.m_drawArguments.m_startInstanceLocation = 0;

	RWByteAddressBuffer countsBuffer = ResourceDescriptorHeap[SpecialDescriptors::DebugDrawIndirectCountUavIndex];
	uint currentIndex;
	countsBuffer.InterlockedAdd(0, 1, currentIndex);

	RWByteAddressBuffer argsBuffer = ResourceDescriptorHeap[SpecialDescriptors::DebugDrawIndirectArgsUavIndex];
	uint destAddress = currentIndex * sizeof(FIndirectDebugDrawCmd);
	argsBuffer.Store(destAddress, cmd);
}