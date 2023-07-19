#pragma once

#include <renderer.h>
#include <controller.h>
#include <concurrent_unordered_map.h>

enum class Viewmode
{
	Normal = 0,
	LightingOnly = 1,
	Roughness = 2,
	Metallic = 3,
	BaseColor = 4,
	Emissive = 5,
	NanCheck = 6,
	Reflections = 7,
	ObjectIds = 8,
	TriangleIds = 9,
	Normalmap = 10,
	LightClusterSlices = 11,
	AmbientOcclusion = 12,
	BentNormals = 13
};

enum class EnvSkyMode
{
	HDRI = 0,
	DynamicSky = 1
};

struct FTextureCache
{
	uint32_t CacheTexture2D(
		FResourceUploadContext* uploadContext,
		const std::wstring& name,
		const DXGI_FORMAT format,
		const int width,
		const int height,
		const DirectX::Image* images,
		const size_t imageCount);

	uint32_t CacheEmptyTexture2D(
		const std::wstring& name,
		const DXGI_FORMAT format,
		const int width,
		const int height,
		const size_t mipCount);

	FLightProbe CacheHDRI(const std::wstring& name);

	void Clear();

	concurrency::concurrent_unordered_map<std::wstring, std::unique_ptr<FTexture>> m_cachedTextures;
};

template<>
struct std::hash<tinygltf::Sampler>
{
	std::size_t operator()(const tinygltf::Sampler& key) const
	{
		uint64_t seed1{}, seed2{};
		spookyhash_context context;
		spookyhash_context_init(&context, seed1, seed2);
		spookyhash_update(&context, &key.minFilter, sizeof(key.minFilter));
		spookyhash_update(&context, &key.magFilter, sizeof(key.magFilter));
		spookyhash_update(&context, &key.wrapS, sizeof(key.wrapS));
		spookyhash_update(&context, &key.wrapT, sizeof(key.wrapT));
		spookyhash_final(&context, &seed1, &seed2);

		return seed1 ^ (seed2 << 1);
	}
};

struct FSamplerCache
{
	uint32_t CacheSampler(const tinygltf::Sampler& s);
	void Clear();

	concurrency::concurrent_unordered_map<tinygltf::Sampler, uint32_t> m_cachedSamplers;
};

namespace Demo
{
	struct App
	{
		FConfig m_config;
		FScene m_scene;
		FView m_view;
		FView m_cullingView;
		FController m_controller;
		float m_aspectRatio;

		std::vector<std::wstring> m_modelList;
		std::vector<std::wstring> m_hdriList;

		bool Initialize(const HWND& windowHandle, const uint32_t resX, const uint32_t resY);
		void Teardown(HWND& windowHandle);
		void Tick(const float deltaTime);
		void OnMouseMove(WPARAM buttonState, int x, int y);
		void Render(const uint32_t resX, const uint32_t resY) const;
		const FConfig& GetConfig() const;
	};

	namespace Utils
	{
		Matrix GetReverseZInfinitePerspectiveFovLH(float fov, float r, float n);
	}

	const FConfig& GetConfig();
	float GetAspectRatio();
	FTextureCache& GetTextureCache();
	FSamplerCache& GetSamplerCache();
}