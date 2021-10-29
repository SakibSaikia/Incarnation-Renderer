#ifndef __cplusplus
	#define uint32_t	uint
	#define Vector3		float3
	#define Vector4		float4
	#define Matrix		float4x4
#else
	#include <SimpleMath.h>
	using namespace DirectX::SimpleMath;
#endif


// Corresponds to GLTF BufferView
struct FMeshBufferView
{
	int m_bufferSrvIndex;
	uint32_t m_byteOffset;
	uint32_t m_byteLength;
};

// Correstponds to GLTF Accessor
struct FMeshAccessor
{
	int m_bufferViewIndex;
	uint32_t m_byteOffset;
	uint32_t m_byteStride;
};

struct FMaterial
{
	Vector3 m_emissiveFactor;
	Vector3 m_baseColorFactor;
	float m_metallicFactor;
	float m_roughnessFactor;
	float m_aoStrength;

	int m_emissiveTextureIndex;
	int m_baseColorTextureIndex;
	int m_metallicRoughnessTextureIndex;
	int m_normalTextureIndex;
	int m_aoTextureIndex;

	int m_emissiveSamplerIndex;
	int m_baseColorSamplerIndex;
	int m_metallicRoughnessSamplerIndex;
	int m_normalSamplerIndex;
	int m_aoSamplerIndex;
};

#ifndef __cplusplus

namespace MeshMaterial
{
	uint GetUint(int index, int accessorIndex, int accessorBufferIndex, int viewBufferIndex)
	{
		if (index == -1 || accessorIndex == -1 || accessorBufferIndex == -1 || viewBufferIndex == -1)
		{
			return 0;
		}

		ByteAddressBuffer accessorBuffer = ResourceDescriptorHeap[accessorBufferIndex];
		FMeshAccessor accessor = accessorBuffer.Load<FMeshAccessor>(accessorIndex * sizeof(FMeshAccessor));
		ByteAddressBuffer viewBuffer = ResourceDescriptorHeap[viewBufferIndex];
		FMeshBufferView view = viewBuffer.Load<FMeshBufferView>(accessor.m_bufferViewIndex * sizeof(FMeshBufferView));
		ByteAddressBuffer buffer = ResourceDescriptorHeap[view.m_bufferSrvIndex];

		if (accessor.m_byteStride == 4)
		{
			return buffer.Load(accessor.m_byteOffset + view.m_byteOffset + index * 4);
		}
		else if (accessor.m_byteStride == 2)
		{
			// Raw address buffer addressing needs to be clamped to a 4 byte boundary
			uint byteOffset = index * 2;
			uint dwordAlignedByteOffset = byteOffset & ~3;
			uint bufferValue = buffer.Load(accessor.m_byteOffset + view.m_byteOffset + dwordAlignedByteOffset);

			// Return Low or High half word depending on the index
			if (dwordAlignedByteOffset == byteOffset)
			{
				return bufferValue & 0xffff;
			}
			else
			{
				return (bufferValue >> 16) & 0xffff;
			}
		}
		else
		{
			return 0;
		}
	}

	uint3 GetUint3(int index, int accessorIndex, int accessorBufferIndex, int viewBufferIndex)
	{
		if (index == -1 || accessorIndex == -1 || accessorBufferIndex == -1 || viewBufferIndex == -1)
		{
			return 0;
		}

		ByteAddressBuffer accessorBuffer = ResourceDescriptorHeap[accessorBufferIndex];
		FMeshAccessor accessor = accessorBuffer.Load<FMeshAccessor>(accessorIndex * sizeof(FMeshAccessor));
		ByteAddressBuffer viewBuffer = ResourceDescriptorHeap[viewBufferIndex];
		FMeshBufferView view = viewBuffer.Load<FMeshBufferView>(accessor.m_bufferViewIndex * sizeof(FMeshBufferView));
		ByteAddressBuffer buffer = ResourceDescriptorHeap[view.m_bufferSrvIndex];

		if (accessor.m_byteStride == 4)
		{
			return buffer.Load3(accessor.m_byteOffset + view.m_byteOffset + index * 4);
		}
		else if (accessor.m_byteStride == 2)
		{
			// Raw address buffer addressing needs to be clamped to a 4 byte boundary
			uint byteOffset = index * 2;
			uint dwordAlignedByteOffset = byteOffset & ~3;
			uint2 four16BitData = buffer.Load2(accessor.m_byteOffset + view.m_byteOffset + dwordAlignedByteOffset);

			// Start at Low or High half word depending on the index
			uint3 data;
			if (dwordAlignedByteOffset == byteOffset)
			{
				data.x = four16BitData.x & 0xffff;
				data.y = (four16BitData.x >> 16) & 0xffff;
				data.z = four16BitData.y & 0xffff;
			}
			else
			{
				data.x = (four16BitData.x >> 16) & 0xffff;
				data.y = four16BitData.y & 0xffff;
				data.z = (four16BitData.y >> 16) & 0xffff;
			}

			return data;
		}
		else
		{
			return 0.xxx;
		}
	}

	float4 GetFloat4(int index, int accessorIndex, int accessorBufferIndex, int viewBufferIndex)
	{
		if (index == -1 || accessorIndex == -1 || accessorBufferIndex == -1 || viewBufferIndex == -1)
		{
			return 0.f.xxxx;
		}

		ByteAddressBuffer accessorBuffer = ResourceDescriptorHeap[accessorBufferIndex];
		FMeshAccessor accessor = accessorBuffer.Load<FMeshAccessor>(accessorIndex * sizeof(FMeshAccessor));
		ByteAddressBuffer viewBuffer = ResourceDescriptorHeap[viewBufferIndex];
		FMeshBufferView view = viewBuffer.Load<FMeshBufferView>(accessor.m_bufferViewIndex * sizeof(FMeshBufferView));
		ByteAddressBuffer buffer = ResourceDescriptorHeap[view.m_bufferSrvIndex];

		[branch]
		switch (accessor.m_byteStride)
		{
			case sizeof(float4) :
				return buffer.Load<float4>(accessor.m_byteOffset + view.m_byteOffset + index * sizeof(float4));
			default:
				return 0.f.xxxx;
		}
	}

	float3 GetFloat3(int index, int accessorIndex, int accessorBufferIndex, int viewBufferIndex)
	{
		if (index == -1 || accessorIndex == -1 || accessorBufferIndex == -1 || viewBufferIndex == -1)
		{
			return 0.f.xxx;
		}

		ByteAddressBuffer accessorBuffer = ResourceDescriptorHeap[accessorBufferIndex];
		FMeshAccessor accessor = accessorBuffer.Load<FMeshAccessor>(accessorIndex * sizeof(FMeshAccessor));
		ByteAddressBuffer viewBuffer = ResourceDescriptorHeap[viewBufferIndex];
		FMeshBufferView view = viewBuffer.Load<FMeshBufferView>(accessor.m_bufferViewIndex * sizeof(FMeshBufferView));
		ByteAddressBuffer buffer = ResourceDescriptorHeap[view.m_bufferSrvIndex];

		float4 temp;
		[branch]
		switch (accessor.m_byteStride)
		{
		case sizeof(float3):
			return buffer.Load<float3>(accessor.m_byteOffset + view.m_byteOffset + index * sizeof(float3));
		case sizeof(float4):
			temp = buffer.Load<float4>(accessor.m_byteOffset + view.m_byteOffset + index * sizeof(float4));
			return temp.xyz;
		default:
			return 0.f.xxx;
		}
	}

	float2 GetFloat2(int index, int accessorIndex, int accessorBufferIndex, int viewBufferIndex)
	{
		if (index == -1 || accessorIndex == -1 || accessorBufferIndex == -1 || viewBufferIndex == -1)
		{
			return 0.f.xx;
		}

		ByteAddressBuffer accessorBuffer = ResourceDescriptorHeap[accessorBufferIndex];
		FMeshAccessor accessor = accessorBuffer.Load<FMeshAccessor>(accessorIndex * sizeof(FMeshAccessor));
		ByteAddressBuffer viewBuffer = ResourceDescriptorHeap[viewBufferIndex];
		FMeshBufferView view = viewBuffer.Load<FMeshBufferView>(accessor.m_bufferViewIndex * sizeof(FMeshBufferView));
		ByteAddressBuffer buffer = ResourceDescriptorHeap[view.m_bufferSrvIndex];

		float4 temp;
		[branch]
		switch (accessor.m_byteStride)
		{
			case sizeof(float2):
				return buffer.Load<float2>(accessor.m_byteOffset + view.m_byteOffset + index * sizeof(float2));
			case sizeof(float3) :
				temp.xyz = buffer.Load<float3>(accessor.m_byteOffset + view.m_byteOffset + index * sizeof(float3));
				return temp.xy;
			case sizeof(float4) :
				temp = buffer.Load<float4>(accessor.m_byteOffset + view.m_byteOffset + index * sizeof(float4));
				return temp.xy;
			default:
				return 0.f.xx;
		}
	}

	FMaterial GetMaterial(int materialIndex, int materialBufferIndex)
	{
		ByteAddressBuffer materialBuffer = ResourceDescriptorHeap[materialBufferIndex];
		return materialBuffer.Load<FMaterial>(materialIndex * sizeof(FMaterial));
	}
}
#endif // __HLSL
