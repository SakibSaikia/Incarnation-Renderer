#pragma once
#include <filesystem>

namespace Settings
{
	constexpr DXGI_FORMAT k_backBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
	constexpr char k_sceneFilename[] = "MetalRoughSpheres.gltf";
}

inline void AssertIfFailed(HRESULT hr)
{
#if defined _DEBUG
	if (FAILED(hr))
	{
		std::string message = std::system_category().message(hr);
		OutputDebugStringA(message.c_str());
		_CrtDbgBreak();
	}
#endif
}

inline void DebugAssert(bool success, const char* msg = nullptr)
{
#if defined _DEBUG
	if (!success)
	{
		if (msg)
		{
			OutputDebugStringA("\n*****\n");
			OutputDebugStringA(msg);
			OutputDebugStringA("\n*****\n");
		}

		_CrtDbgBreak();
	}
#endif
}

inline std::wstring GetFilepathW(const std::wstring& filename)
{
	for (auto& entry : std::filesystem::recursive_directory_iterator(CONTENT_DIR))
	{
		if (entry.is_regular_file() && entry.path().filename().wstring() == filename)
		{
			return entry.path().wstring();
		}
	}

	DebugAssert(false, "File not found");
	return {};
}

inline std::string GetFilepathA(const std::string& filename)
{
	for (auto& entry : std::filesystem::recursive_directory_iterator(CONTENT_DIR))
	{
		if (entry.is_regular_file() && entry.path().filename().string() == filename)
		{
			return entry.path().string();
		}
	}

	DebugAssert(false, "File not found");
	return {};
}