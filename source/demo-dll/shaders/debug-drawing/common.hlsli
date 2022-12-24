#include "gpu-shared-types.h"

// Helper struct that *must* alias FIndirectDrawWithRootConstants
struct FIndirectDebugPrimitiveDrawCmd
{
	float4 m_color;
	float4x4 m_transform;
	uint m_shapeType;
	uint3 __pad0;
	uint4 __pad1;
	uint4 __pad2;

	FDrawInstanced m_drawArguments;
};

// Helper struct that *must* alias FIndirectDrawWithRootConstants
struct FIndirectDebugLineDrawCmd
{
	float4 m_start;
	float4 m_end;
	float4 m_color;
	float4 __pad[5];

	FDrawInstanced m_drawArguments;
};

uint GetIndexCount(uint shapeType)
{
	ByteAddressBuffer indexCountBuffer = ResourceDescriptorHeap[SpecialDescriptors::DebugPrimitiveIndexCountSrvIndex];
	return indexCountBuffer.Load<uint>(shapeType * sizeof(uint));
}

void DrawDebugLine(float4 color, float3 start, float3 end)
{
	FIndirectDebugLineDrawCmd cmd = (FIndirectDebugLineDrawCmd)0;

	cmd.m_color = color;
	cmd.m_start = float4(start, 1.f);
	cmd.m_end = float4(end, 1.f);

	cmd.m_drawArguments.m_vertexCount = 2;
	cmd.m_drawArguments.m_instanceCount = 1;
	cmd.m_drawArguments.m_startVertexLocation = 0;
	cmd.m_drawArguments.m_startInstanceLocation = 0;

	RWByteAddressBuffer countsBuffer = ResourceDescriptorHeap[SpecialDescriptors::DebugDrawIndirectLineCountUavIndex];
	uint currentIndex;
	countsBuffer.InterlockedAdd(0, 1, currentIndex);

	RWByteAddressBuffer argsBuffer = ResourceDescriptorHeap[SpecialDescriptors::DebugDrawIndirectLineArgsUavIndex];
	uint destAddress = currentIndex * sizeof(FIndirectDebugLineDrawCmd);
	argsBuffer.Store(destAddress, cmd);
}

void DrawDebugFrustum(float4 color,
	float3 NearLeftBottom, float3 NearRightBottom, float3 NearRightTop, float3 NearLeftTop,
	float3 FarLeftBottom, float3 FarRightBottom, float3 FarRightTop, float3 FarLeftTop)
{
	DrawDebugLine(color, NearLeftBottom, NearRightBottom);
	DrawDebugLine(color, NearRightBottom, NearRightTop);
	DrawDebugLine(color, NearRightTop, NearLeftTop);
	DrawDebugLine(color, NearLeftTop, NearLeftBottom);

	DrawDebugLine(color, FarLeftBottom, FarRightBottom);
	DrawDebugLine(color, FarRightBottom, FarRightTop);
	DrawDebugLine(color, FarRightTop, FarLeftTop);
	DrawDebugLine(color, FarLeftTop, FarLeftBottom);

	DrawDebugLine(color, NearLeftBottom, FarLeftBottom);
	DrawDebugLine(color, NearRightBottom, FarRightBottom);
	DrawDebugLine(color, NearRightTop, FarRightTop);
	DrawDebugLine(color, NearLeftTop, FarLeftTop);
}

void DrawDebugPrimitive(uint shapeType, float4 color, float4x4 transform)
{
	FIndirectDebugPrimitiveDrawCmd cmd = (FIndirectDebugPrimitiveDrawCmd)0;

	cmd.m_color = color;
	cmd.m_transform = transform;
	cmd.m_shapeType = shapeType;

	cmd.m_drawArguments.m_vertexCount = GetIndexCount(shapeType);
	cmd.m_drawArguments.m_instanceCount = 1;
	cmd.m_drawArguments.m_startVertexLocation = 0;
	cmd.m_drawArguments.m_startInstanceLocation = 0;

	RWByteAddressBuffer countsBuffer = ResourceDescriptorHeap[SpecialDescriptors::DebugDrawIndirectPrimitiveCountUavIndex];
	uint currentIndex;
	countsBuffer.InterlockedAdd(0, 1, currentIndex);

	RWByteAddressBuffer argsBuffer = ResourceDescriptorHeap[SpecialDescriptors::DebugDrawIndirectPrimitiveArgsUavIndex];
	uint destAddress = currentIndex * sizeof(FIndirectDebugPrimitiveDrawCmd);
	argsBuffer.Store(destAddress, cmd);
}