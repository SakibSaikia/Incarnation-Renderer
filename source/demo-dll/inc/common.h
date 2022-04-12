#pragma once
#include <filesystem>
#include <locale>
#include <codecvt>

enum class Viewmode
{
	Normal			= 0,
	LightingOnly	= 1,
	Roughness		= 2,
	Metallic		= 3,
	BaseColor		= 4,
	Emissive		= 5,
	NanCheck		= 6,
	Reflections		= 7,
	ObjectIds		= 8,
	TriangleIds		= 9,
	Normalmap		= 10
};

struct FConfig
{
	DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	std::wstring ModelFilename = L"DamagedHelmet.gltf";
	std::wstring EnvironmentFilename = L"lilienstein_2k.hdr";
	bool UseContentCache = true;
	float Fov = 0.25f * DirectX::XM_PI;
	float Exposure = 13.f;
	float CameraSpeed = 5.f;
	int Viewmode = 0;
	bool EnableDirectLighting = true;
	bool EnableDiffuseIBL = true;
	bool EnableSpecularIBL = true;
	bool PathTrace = false;
	bool EnableTAA = true;
	uint32_t MaxSampleCount = 256;
	float Pathtracing_CameraAperture = 0.01f;
	float Pathtracing_CameraFocalLength = 7.f;
};

inline void AssertIfFailed(HRESULT hr)
{
#if defined _DEBUG
	if (FAILED(hr))
	{
		std::stringstream message;
		message << "ASSERTION FAILURE - " << std::system_category().message(hr) << std::endl;
		OutputDebugStringA(message.str().c_str());
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

inline void AbortOnFailure(bool success, const char* msg, const HWND& windowHandle)
{
	if (!success)
	{
		MessageBoxA(windowHandle, msg, "Fatal Error", MB_OK);
		ExitProcess(-1);
	}
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

constexpr size_t GetAlignedSize(const size_t alignment, const size_t size)
{
	return (size + (alignment - 1)) & ~(alignment - 1);
}