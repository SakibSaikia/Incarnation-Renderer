#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxcapi.h>
#include <wrl.h>
# include <string>

namespace Demo
{
	namespace ShaderCompiler
	{
		bool Initialize();
		void Teardown();

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
}

