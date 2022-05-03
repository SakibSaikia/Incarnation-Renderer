#include "common/math.hlsli"
#include "common/color-space.hlsli"
#include "common/uniform-sampling.hlsli"
#include "common/mesh-material.hlsli"
#include "gpu-shared-types.h"
#include "geo-raster/encoding.hlsli"

#define rootsig \
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_MIN_MAG_MIP_POINT, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP), " \
    "RootConstants(b0, num32BitConstants=13, visibility = SHADER_VISIBILITY_PIXEL)"

SamplerState g_pointSampler : register(s0);

cbuffer cb : register(b0)
{
	int g_visbufferTextureIndex;
	int g_gbuffer0TextureIndex;
	int g_gbuffer1TextureIndex;
	int g_gbuffer2TextureIndex;
	int g_indirectArgsBufferIndex;
	int g_sceneMeshAccessorsIndex;
	int g_sceneMeshBufferViewsIndex;
	int g_scenePrimitivesIndex;
	int g_viewmode;
	uint g_resX;
	uint g_resY;
	uint g_mouseX;
	uint g_mouseY;
}

struct vs_to_ps
{
	float4 pos : SV_POSITION;
	float2 uv : INTERPOLATED_UV_0;
};

vs_to_ps vs_main(uint id : SV_VertexID)
{
	vs_to_ps o;
	o.uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(o.uv * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);
	return o;
}

float4 ps_main(vs_to_ps input) : SV_Target
{
	// Roughness
	if (g_viewmode == 2)
	{
		Texture2D gbufferMetallicRoughnessAoTex = ResourceDescriptorHeap[g_gbuffer2TextureIndex];
		return gbufferMetallicRoughnessAoTex.Load(int3(input.uv.x * g_resX, input.uv.y * g_resY, 0)).gggg;
	}
	// Metallic
	else if (g_viewmode == 3)
	{
		Texture2D gbufferMetallicRoughnessAoTex = ResourceDescriptorHeap[g_gbuffer2TextureIndex];
		return gbufferMetallicRoughnessAoTex.Load(int3(input.uv.x * g_resX, input.uv.y * g_resY, 0)).rrrr;
	}
	// Base color
	else if (g_viewmode == 4)
	{
		Texture2D gbufferBaseColorTex = ResourceDescriptorHeap[g_gbuffer0TextureIndex];
		return gbufferBaseColorTex.Load(int3(input.uv.x * g_resX, input.uv.y * g_resY, 0));
	}
	// Object IDs
	else if (g_viewmode == 8)
	{
		Texture2D<int> visbufferTex = ResourceDescriptorHeap[g_visbufferTextureIndex];
		int visbufferValue = visbufferTex.Load(int3(input.uv.x * g_resX, input.uv.y * g_resY, 0));

		uint objectId, triangleId;
		DecodeVisibilityBuffer(visbufferValue, objectId, triangleId);

		if (floor(input.uv.x * g_resX) == g_mouseX && floor(input.uv.y * g_resY) == g_mouseY)
		{
			// Use object id to retrieve the primitive info
			ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[g_scenePrimitivesIndex];
			const FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(objectId * sizeof(FGpuPrimitive));

			FDrawWithRootConstants cmd = (FDrawWithRootConstants)0;
			cmd.rootConstants[0] = objectId;
			cmd.rootConstants[1] = 0;
			cmd.drawArguments.VertexCount = primitive.m_indexCount;
			cmd.drawArguments.InstanceCount = 1;
			cmd.drawArguments.StartVertexLocation = 0;
			cmd.drawArguments.StartInstanceLocation = 0;

			RWByteAddressBuffer argsBuffer = ResourceDescriptorHeap[g_indirectArgsBufferIndex];
			argsBuffer.Store(0, cmd);
		}

		return hsv2rgb(float3(Halton(objectId, 3) * 360.f, 0.85f, 0.95f)).rgbr;
	}
	// Triangle IDs
	else if (g_viewmode == 9)
	{
		Texture2D<int> visbufferTex = ResourceDescriptorHeap[g_visbufferTextureIndex];
		int visbufferValue = visbufferTex.Load(int3(input.uv.x * g_resX, input.uv.y * g_resY, 0));

		uint objectId, triangleId;
		DecodeVisibilityBuffer(visbufferValue, objectId, triangleId);

		if (floor(input.uv.x * g_resX) == g_mouseX && floor(input.uv.y * g_resY) == g_mouseY)
		{
			// Use object id to retrieve the primitive info
			ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[g_scenePrimitivesIndex];
			const FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(objectId * sizeof(FGpuPrimitive));
			uint startVertIndex = MeshMaterial::GetUint(0, primitive.m_indexAccessor, g_sceneMeshAccessorsIndex, g_sceneMeshBufferViewsIndex);

			FDrawWithRootConstants cmd = (FDrawWithRootConstants)0;
			cmd.rootConstants[0] = objectId;
			cmd.rootConstants[1] = triangleId * 3;
			cmd.drawArguments.VertexCount = primitive.m_indicesPerTriangle;
			cmd.drawArguments.InstanceCount = 1;
			cmd.drawArguments.StartVertexLocation = 0;
			cmd.drawArguments.StartInstanceLocation = 0;

			RWByteAddressBuffer argsBuffer = ResourceDescriptorHeap[g_indirectArgsBufferIndex];
			argsBuffer.Store(0, cmd);
		}

		return hsv2rgb(float3(Halton(triangleId, 5) * 360.f, 0.85f, 0.95f)).rgbr;
	}
	// Normals
	else if (g_viewmode == 10)
	{
		Texture2D gbufferNormalsTex = ResourceDescriptorHeap[g_gbuffer1TextureIndex];
		float3 N = gbufferNormalsTex.Load(int3(input.uv.x * g_resX, input.uv.y * g_resY, 0)).xyz;
		N = N * 0.5f + 0.5.xxx;
		return float4(N, 1.f);
	}

	return 0.xxxx;
}