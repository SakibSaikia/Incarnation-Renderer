#pragma once
#include <filesystem>
#include <locale>
#include <codecvt>

struct Config
{
	static inline const DXGI_FORMAT g_backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	static inline std::wstring g_modelFilename = L"MetalRoughSpheres.gltf";
	static inline std::wstring g_environmentFilename = L"lilienstein_2k.hdr";
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