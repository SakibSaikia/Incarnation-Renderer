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

Texture2D g_bindless2DTextures[] : register(t0, space0);
ByteAddressBuffer g_bindlessBuffers[] : register(t1, space1);
TextureCube g_bindlessCubeTextures[] : register(t2, space2);
RaytracingAccelerationStructure g_accelerationStructures[] : register(t3, space3);
SamplerState g_bindlessSamplers[] : register(s0, space0);

namespace MeshMaterial
{
	uint GetUint(int index, int accessorIndex, int accessorBufferIndex, int viewBufferIndex)
	{
		if (index == -1 || accessorIndex == -1 || accessorBufferIndex == -1 || viewBufferIndex == -1)
		{
			return 0;
		}

		FMeshAccessor accessor = g_bindlessBuffers[accessorBufferIndex].Load<FMeshAccessor>(accessorIndex * sizeof(FMeshAccessor));
		FMeshBufferView view = g_bindlessBuffers[viewBufferIndex].Load<FMeshBufferView>(accessor.m_bufferViewIndex * sizeof(FMeshBufferView));
		ByteAddressBuffer buffer = g_bindlessBuffers[view.m_bufferSrvIndex];

		if (accessor.m_byteStride == 4)
		{
			return buffer.Load(accessor.m_byteOffset + view.m_byteOffset + index * 4);
		}
		else if (accessor.m_byteStride == 2)
		{
			// Raw address buffer addressing needs to be clamped to a 4 byte boundary
			int clampedIndex = (index * 2) / 4;
			uint bufferValue = buffer.Load(accessor.m_byteOffset + view.m_byteOffset + clampedIndex * 4);

			// Return Low or High half word depending on the index
			if ((index & 0x1) == 0)
			{
				return bufferValue & 0x0000ffff;
			}
			else
			{
				return (bufferValue & 0xffff0000) >> 16;
			}
		}
		else
		{
			return 0;
		}
	}

	float4 GetFloat4(int index, int accessorIndex, int accessorBufferIndex, int viewBufferIndex)
	{
		if (index == -1 || accessorIndex == -1 || accessorBufferIndex == -1 || viewBufferIndex == -1)
		{
			return 0.f.xxxx;
		}

		FMeshAccessor accessor = g_bindlessBuffers[accessorBufferIndex].Load<FMeshAccessor>(accessorIndex * sizeof(FMeshAccessor));
		FMeshBufferView view = g_bindlessBuffers[viewBufferIndex].Load<FMeshBufferView>(accessor.m_bufferViewIndex * sizeof(FMeshBufferView));
		ByteAddressBuffer buffer = g_bindlessBuffers[view.m_bufferSrvIndex];

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

		FMeshAccessor accessor = g_bindlessBuffers[accessorBufferIndex].Load<FMeshAccessor>(accessorIndex * sizeof(FMeshAccessor));
		FMeshBufferView view = g_bindlessBuffers[viewBufferIndex].Load<FMeshBufferView>(accessor.m_bufferViewIndex * sizeof(FMeshBufferView));
		ByteAddressBuffer buffer = g_bindlessBuffers[view.m_bufferSrvIndex];

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

		FMeshAccessor accessor = g_bindlessBuffers[accessorBufferIndex].Load<FMeshAccessor>(accessorIndex * sizeof(FMeshAccessor));
		FMeshBufferView view = g_bindlessBuffers[viewBufferIndex].Load<FMeshBufferView>(accessor.m_bufferViewIndex * sizeof(FMeshBufferView));
		ByteAddressBuffer buffer = g_bindlessBuffers[view.m_bufferSrvIndex];

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
		return g_bindlessBuffers[materialBufferIndex].Load<FMaterial>(materialIndex * sizeof(FMaterial));
	}
}
#endif // __HLSL

