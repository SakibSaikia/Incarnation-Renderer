#include <shadercompiler.h>
#include <system_error>
#include <vector>

namespace Settings
{
#if defined (_DEBUG)
	std::vector<LPCWSTR> k_compilerArguments = { L"-Zpr", L"-Zi", L"-WX" };
#else
	std::vector<LPCWSTR> k_compilerArguments = { L"-Zpr" };
#endif
}

namespace
{

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
}

HRESULT Demo::ShaderCompiler::CompileShader(
	const std::wstring& filename, 
	const std::wstring& entrypoint, 
	const std::wstring& arguments,
	const std::wstring& profile,
	IDxcBlob** compiledBytecode)
{
	Microsoft::WRL::ComPtr<IDxcLibrary> library;
	AssertIfFailed(DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(library.GetAddressOf())));

	Microsoft::WRL::ComPtr<IDxcBlobEncoding> source;
	AssertIfFailed(library->CreateBlobFromFile(filename.c_str(), nullptr, source.GetAddressOf()));

	Microsoft::WRL::ComPtr<IDxcIncludeHandler> includeHandler;
	AssertIfFailed(library->CreateIncludeHandler(includeHandler.GetAddressOf()));

	Microsoft::WRL::ComPtr<IDxcCompiler> compiler;
	AssertIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.GetAddressOf())));

	Microsoft::WRL::ComPtr<IDxcOperationResult> result;
	AssertIfFailed(compiler->Compile(
		source.Get(),
		filename.c_str(),
		entrypoint.c_str(),
		profile.c_str(),
		Settings::k_compilerArguments.data(), (UINT)Settings::k_compilerArguments.size(),
		nullptr, 0, 
		includeHandler.Get(),
		result.GetAddressOf()));

	HRESULT hr;
	result->GetStatus(&hr);
	if (SUCCEEDED(hr)) 
	{
		Microsoft::WRL::ComPtr<IDxcBlob> bytecode;
		return result->GetResult(compiledBytecode);
	}
	else
	{
		Microsoft::WRL::ComPtr<IDxcBlobEncoding> error;
		result->GetErrorBuffer(error.GetAddressOf());
		return hr;
	}
}