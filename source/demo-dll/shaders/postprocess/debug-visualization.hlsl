#include "common/math.hlsli"
#include "common/color-space.hlsli"
#include "common/uniform-sampling.hlsli"
#include "common/mesh-material.hlsli"
#include "gpu-shared-types.h"
#include "geo-raster/encoding.hlsli"
#include "debug-drawing/common.hlsli"

#define rootsig \
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_MIN_MAG_MIP_POINT, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP)," \
    "RootConstants(b0, num32BitConstants=9, visibility = SHADER_VISIBILITY_PIXEL)," \
	"CBV(b1)," \
    "CBV(b2)"

SamplerState g_pointSampler : register(s0);

cbuffer cb : register(b0)
{
	int g_visbufferTextureIndex;
	int g_gbuffer0TextureIndex;
	int g_gbuffer1TextureIndex;
	int g_gbuffer2TextureIndex;
	int g_depthBufferTextureIndex;
	int g_aoTextureIndex;
	int g_bentNormalsTextureIndex;
	int g_indirectArgsBufferIndex;
	uint g_lightClusterSlices;
}

ConstantBuffer<FViewConstants> g_viewCb : register(b1);
ConstantBuffer<FSceneConstants> g_sceneCb : register(b2);

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
	if (g_viewCb.m_viewmode == 2)
	{
		Texture2D gbufferMetallicRoughnessAoTex = ResourceDescriptorHeap[g_gbuffer2TextureIndex];
		return gbufferMetallicRoughnessAoTex.Load(int3(input.uv.x * g_viewCb.m_resX, input.uv.y * g_viewCb.m_resY, 0)).gggg;
	}
	// Metallic
	else if (g_viewCb.m_viewmode == 3)
	{
		Texture2D gbufferMetallicRoughnessAoTex = ResourceDescriptorHeap[g_gbuffer2TextureIndex];
		return gbufferMetallicRoughnessAoTex.Load(int3(input.uv.x * g_viewCb.m_resX, input.uv.y * g_viewCb.m_resY, 0)).rrrr;
	}
	// Base color
	else if (g_viewCb.m_viewmode == 4)
	{
		Texture2D gbufferBaseColorTex = ResourceDescriptorHeap[g_gbuffer0TextureIndex];
		return gbufferBaseColorTex.Load(int3(input.uv.x * g_viewCb.m_resX, input.uv.y * g_viewCb.m_resY, 0));
	}
	// Object IDs
	else if (g_viewCb.m_viewmode == 8)
	{
		Texture2D<int> visbufferTex = ResourceDescriptorHeap[g_visbufferTextureIndex];
		int visbufferValue = visbufferTex.Load(int3(input.uv.x * g_viewCb.m_resX, input.uv.y * g_viewCb.m_resY, 0));

	#if USING_MESHLETS
		uint objectId, triangleId;
        DecodeMeshletVisibility(visbufferValue, objectId, triangleId);
		
		ByteAddressBuffer packedMeshletsBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedSceneMeshletsBufferIndex];
        const FGpuMeshlet meshlet = packedMeshletsBuffer.Load<FGpuMeshlet>(objectId * sizeof(FGpuMeshlet));
		float4 boundingSphere = meshlet.m_boundingSphere;
		int meshIndex = meshlet.m_meshIndex;
        const uint vertCount = meshlet.m_triangleCount * 3;
	#else
		uint objectId, triangleId;
        DecodePrimitiveVisibility(visbufferValue, objectId, triangleId);
		
        ByteAddressBuffer primitivesBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedScenePrimitivesBufferIndex];
        const FGpuPrimitive primitive = primitivesBuffer.Load<FGpuPrimitive>(objectId * sizeof(FGpuPrimitive));
        float4 boundingSphere = primitive.m_boundingSphere;
        int meshIndex = primitive.m_meshIndex;
        const uint vertCount = primitive.m_indexCount;
	#endif

		if (floor(input.uv.x * g_viewCb.m_resX) == g_viewCb.m_mouseX && floor(input.uv.y * g_viewCb.m_resY) == g_viewCb.m_mouseY)
		{
			FIndirectDrawWithRootConstants cmd = (FIndirectDrawWithRootConstants)0;
			cmd.m_rootConstants[0] = objectId;
			cmd.m_rootConstants[1] = 0;
			cmd.m_drawArguments.m_vertexCount = vertCount;
			cmd.m_drawArguments.m_instanceCount = 1;
			cmd.m_drawArguments.m_startVertexLocation = 0;
			cmd.m_drawArguments.m_startInstanceLocation = 0;

			RWByteAddressBuffer argsBuffer = ResourceDescriptorHeap[g_indirectArgsBufferIndex];
			argsBuffer.Store(0, cmd);
			
		#if SHOW_OBJECT_BOUNDS // Render the bounds of the highligted primitive or meshlet
            ByteAddressBuffer meshTransformsBuffer = ResourceDescriptorHeap[g_sceneCb.m_packedSceneMeshTransformsBufferIndex];
            float4x4 meshTransform = meshTransformsBuffer.Load<float4x4>(meshIndex * sizeof(float4x4));
            float4 boundsPos = float4(boundingSphere.x, boundingSphere.y, boundingSphere.z, 1.f);
            boundsPos = mul(boundsPos, meshTransform);
            float4x4 scaledBoundsTransform =
            {
                boundingSphere.w, 0.f, 0.f, 0.f,
                0.f, boundingSphere.w, 0.f, 0.f,
                0.f, 0.f, boundingSphere.w, 0.f,
                boundsPos.x, boundsPos.y, boundsPos.z, 1.f
            };
            DrawDebugPrimitive((uint) DebugShape::Sphere, float4(0, 1.f, 0.f, 1.f), scaledBoundsTransform);
		#endif
        }

		return hsv2rgb(float3(Halton(objectId, 3) * 360.f, 0.85f, 0.95f)).rgbr;
	}
	// Triangle IDs
	else if (g_viewCb.m_viewmode == 9)
	{
		Texture2D<int> visbufferTex = ResourceDescriptorHeap[g_visbufferTextureIndex];
		int visbufferValue = visbufferTex.Load(int3(input.uv.x * g_viewCb.m_resX, input.uv.y * g_viewCb.m_resY, 0));

	#if USING_MESHLETS
		uint objectId, triangleId;
        DecodeMeshletVisibility(visbufferValue, objectId, triangleId);
	#else
		uint objectId, triangleId;
        DecodePrimitiveVisibility(visbufferValue, objectId, triangleId);
	#endif

		if (floor(input.uv.x * g_viewCb.m_resX) == g_viewCb.m_mouseX && floor(input.uv.y * g_viewCb.m_resY) == g_viewCb.m_mouseY)
		{
			FIndirectDrawWithRootConstants cmd = (FIndirectDrawWithRootConstants)0;
			cmd.m_rootConstants[0] = objectId;
			cmd.m_rootConstants[1] = triangleId;
			cmd.m_drawArguments.m_vertexCount = 3;
			cmd.m_drawArguments.m_instanceCount = 1;
			cmd.m_drawArguments.m_startVertexLocation = 0;
			cmd.m_drawArguments.m_startInstanceLocation = 0;

			RWByteAddressBuffer argsBuffer = ResourceDescriptorHeap[g_indirectArgsBufferIndex];
			argsBuffer.Store(0, cmd);
		}

		return hsv2rgb(float3(Halton(triangleId, 5) * 360.f, 0.85f, 0.95f)).rgbr;
	}
	// Normals
	else if (g_viewCb.m_viewmode == 10)
	{
		Texture2D<float2> gbufferNormalsTex = ResourceDescriptorHeap[g_gbuffer1TextureIndex];
		float3 N = OctDecode(gbufferNormalsTex.Load(int3(input.uv.x * g_viewCb.m_resX, input.uv.y * g_viewCb.m_resY, 0)));
		N = N * 0.5f + 0.5.xxx;
		return float4(N, 1.f);
	}
	// Light Cluster Slices
	else if(g_viewCb.m_viewmode == 11)
	{
		Texture2D<float> depthTex = ResourceDescriptorHeap[g_depthBufferTextureIndex];

		float4 pixelNdc;
		pixelNdc.x = 2.f * input.uv.x - 1.f;
		pixelNdc.y = -2.f * input.uv.y + 1.f;
		pixelNdc.z = max(0.001, depthTex.Load(int3(input.uv.x* g_viewCb.m_resX, input.uv.y* g_viewCb.m_resY, 0)));
		pixelNdc.w = 1.f;

        float4 pixelViewSpace = mul(pixelNdc, g_viewCb.m_invProjTransform);
		float z = pixelViewSpace.z / pixelViewSpace.w;

		const uint numSlices = g_lightClusterSlices;
		const float zFar = 1000.f;
		const float zNear = 5.f;
		const float scale = numSlices / log(zFar / zNear);
		const float bias = -log(zNear) * scale;
		const uint sliceIndex = floor(scale * log(z) + bias);

		return hsv2rgb(float3(sliceIndex / (float)numSlices * 360.f, 0.85f, 0.95f)).rgbr;
	}
	// Ambient Occlusion
	else if (g_viewCb.m_viewmode == 12)
	{
		Texture2D aoTex = ResourceDescriptorHeap[g_aoTextureIndex];
		return aoTex.Load(int3(input.uv.x * g_viewCb.m_resX, input.uv.y * g_viewCb.m_resY, 0)).rrrr;
	}
	// Bent Normals
	else if (g_viewCb.m_viewmode == 13)
	{
		Texture2D<float2> bentNormalsTex = ResourceDescriptorHeap[g_bentNormalsTextureIndex];
		float3 bentNormal = OctDecode(bentNormalsTex.Load(int3(input.uv.x * g_viewCb.m_resX, input.uv.y * g_viewCb.m_resY, 0)));
		bentNormal = bentNormal * 0.5f + 0.5.xxx;
		return float4(bentNormal, 1.f);
	}

	return 0.xxxx;
}