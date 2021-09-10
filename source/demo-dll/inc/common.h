#pragma once
#include <filesystem>
#include <locale>
#include <codecvt>

struct Config
{
	static inline const DXGI_FORMAT g_backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	static inline std::wstring g_modelFilename = L"DamagedHelmet.gltf";
	static inline std::wstring g_environmentFilename = L"lilienstein_2k.hdr";
	static inline bool g_useContentCache = true;
	static inline float g_fov = 0.25f * DirectX::XM_PI;
	static inline float g_exposure = 13.f;
	static inline float g_cameraSpeed = 5.f;
	static inline bool g_lightingOnlyView = false;
	static inline bool g_enableDirectLighting = true;
	static inline bool g_enableDiffuseIBL = true;
	static inline bool g_enableSpecularIBL = true;
};

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

inline std::wstring GetFilepathW(const std::wstring& filename, bool includeCache = false)
{
	for (auto& entry : std::filesystem::recursive_directory_iterator(CONTENT_DIR))
	{
		if (entry.is_regular_file() && entry.path().filename().wstring() == filename)
		{
			if (includeCache)
			{
				return entry.path().wstring();
			}
			else
			{
				// If cache directories are to be excluded in the search, look for the '.' in 
				// the path which is the prefix for all cache directories
				std::string parentPath = entry.path().parent_path().string();
				if (parentPath.rfind('.') == std::string::npos)
				{
					return entry.path().wstring();
				}
			}
		}
	}

	DebugAssert(false, "File not found");
	return {};
}

inline std::string GetFilepathA(const std::string& filename, bool includeCache = false)
{
	for (auto& entry : std::filesystem::recursive_directory_iterator(CONTENT_DIR))
	{
		if (entry.is_regular_file() && entry.path().filename().string() == filename)
		{
			if (includeCache)
			{
				return entry.path().string();
			}
			else
			{
				// If cache directories are to be excluded in the search, look for the '.' in 
				// the path which is the prefix for all cache directories
				std::string parentPath = entry.path().parent_path().string();
				if (parentPath.rfind('.') == std::string::npos)
				{
					return entry.path().string();
				}
			}
		}
	}

	DebugAssert(false, "File not found");
	return {};
}

// https://stackoverflow.com/questions/4804298/how-to-convert-wstring-into-string
inline std::wstring s2ws(const std::string& str)
{
	using convert_typeX = std::codecvt_utf8<wchar_t>;
	std::wstring_convert<convert_typeX, wchar_t> converterX;

	return converterX.from_bytes(str);
}

// https://stackoverflow.com/questions/4804298/how-to-convert-wstring-into-string
inline std::string ws2s(const std::wstring& wstr)
{
	using convert_typeX = std::codecvt_utf8<wchar_t>;
	std::wstring_convert<convert_typeX, wchar_t> converterX;

	return converterX.to_bytes(wstr);
}