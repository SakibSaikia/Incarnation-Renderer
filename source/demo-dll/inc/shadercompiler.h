#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxcapi.h>
# include <string>

namespace ShaderCompiler
{
	bool Initialize();
	void Teardown();

	FILETIME GetLastModifiedTime(const std::wstring& filename);

	HRESULT Preprocess(
		const std::wstring& filename,
		const std::wstring& arguments,
		IDxcBlob** preprocessBlob);

	HRESULT CompileShader(
		const std::wstring& filename,
		const std::wstring& entrypoint,
		const std::wstring& arguments,
		const std::wstring& profile,
		IDxcBlob** compiledBlob);

	HRESULT CompileRootsignature(
		const std::wstring& filename,
		const std::wstring& define,
		const std::wstring& profile,
		IDxcBlob** compiledBlob);
}

