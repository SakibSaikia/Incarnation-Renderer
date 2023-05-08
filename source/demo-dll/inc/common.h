#pragma once
#include <filesystem>
#include <locale>
#include <codecvt>

struct FConfig
{
	DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	bool UseGpuBasedValidation = false;
	std::wstring ModelFilename = L"DamagedHelmet.gltf";
	std::wstring HDRIFilename = L"lilienstein_2k.hdr";
	bool UseContentCache = true;
	float Fov = 0.25f * DirectX::XM_PI;
	float Exposure = 13.f;
	float CameraSpeed = 5.f;
	float CameraNearPlane = 1.f;
	int Viewmode = 0;
	int EnvSkyMode = 1;
	float SkyBrightness = 25000.f;
	bool EnableDirectLighting = true;
	bool EnableDiffuseIBL = true;
	bool EnableSpecularIBL = true;
	bool PathTrace = false;
	bool EnableTAA = true;
	bool FreezeCulling = false;
	bool ShowLightBounds = false;
	int LightClusterDimX = 16;
	int LightClusterDimY = 9;
	int LightClusterDimZ = 24;
	int MaxLightsPerCluster = 64;
	float ClusterDepthExtent = 200.f;
	uint32_t MaxSampleCount = 256;
	float Pathtracing_CameraAperture = 0.01f;
	float Pathtracing_CameraFocalLength = 7.f;
	float Turbidity = 2.f;
	bool ToD_Enable = true;
	float ToD_DecimalHours = 11.f;
	int ToD_JulianDate = 200;
	float ToD_Latitude = 42.5;
	int EnvmapResolution = 256;
};

template<class T>
inline std::basic_string<T> GetFormattedString(const T* formatString, va_list params)
{
	std::basic_stringstream<T> output;
	for (int i = 0; formatString[i] != '\0'; ++i)
	{
		if (formatString[i] == '%')
		{
			switch (formatString[++i])
			{
			case 'c':
				output << va_arg(params, T);
				break;
			case 's':
				output << va_arg(params, T*);
				break;
			case 'd':
				output << va_arg(params, int);
				break;
			case 'u':
				output << va_arg(params, uint32_t);
				break;
			case 'f':
				output << va_arg(params, float);
				break;
			case 'x':
				output << std::hex << va_arg(params, uint32_t);
				break;
			}
		}
		else
		{
			output << formatString[i];
		}
	}

	return output.str();
}

template<class T>
inline std::basic_string<T> PrintString(const T* formatString, ...)
{
	va_list params;
	va_start(params, formatString);
	return GetFormattedString(formatString, params);
}

// Logging utility that can work with both char* and wchar_t*
template<class T>
requires std::is_same<T, char>::value || std::is_same<T, wchar_t>::value
inline void Print(const T* formatString, ...)
{
	va_list params;
	va_start(params, formatString);
	std::basic_string<T> output = GetFormattedString(formatString, params);
	output += '\n';

	if constexpr (std::is_same<T, wchar_t>::value)
		OutputDebugStringW(output.c_str());
	else 
		OutputDebugStringA(output.c_str());
}

inline void AssertIfFailed(HRESULT hr)
{
#if defined _DEBUG
	if (FAILED(hr))
	{
		Print("ASSERTION FAILURE - %s", std::system_category().message(hr).c_str());
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
			Print("\n*****\n");
			Print(msg);
			Print("\n*****\n");
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