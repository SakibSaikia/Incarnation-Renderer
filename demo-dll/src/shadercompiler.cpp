#include <shadercompiler.h>
#include <assert.h>
#include <system_error>
#include <vector>
#include <filesystem>

namespace Settings
{
#if defined (_DEBUG)
	std::vector<LPCWSTR> k_compilerArguments = { L"-Zpr", L"-Zi", L"-WX" };
#else
	std::vector<LPCWSTR> k_compilerArguments = { L"-Zpr" };
#endif
}

namespace Demo::ShaderCompiler
{
	HMODULE s_validationModule;
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

	std::filesystem::path SearchShaderDir(const std::wstring& filename)
	{
		for (auto& it : std::filesystem::recursive_directory_iterator(SHADER_DIR))
		{
			if (it.is_regular_file() && 
				it.path().filename().wstring() == filename)
			{
				return it.path().wstring();
			}
		}

		return {};
	}
}

bool Demo::ShaderCompiler::Initialize()
{
	s_validationModule = LoadLibraryEx(L"dxil.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	assert(s_validationModule && "Failed to load dxil.dll. Shaders will not be signed!");
	
	return s_validationModule != nullptr;
}

void Demo::ShaderCompiler::Teardown()
{
	FreeLibrary(s_validationModule);
}

HRESULT Demo::ShaderCompiler::CompileShader(
	const std::wstring& filename, 
	const std::wstring& entrypoint, 
	const std::wstring& arguments,
	const std::wstring& profile,
	IDxcBlob** compiledBlob)
{
	const std::filesystem::path filepath = SearchShaderDir(filename);
	assert(!filepath.empty() && "Shader source file not found");

	Microsoft::WRL::ComPtr<IDxcLibrary> library;
	AssertIfFailed(DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(library.GetAddressOf())));

	Microsoft::WRL::ComPtr<IDxcBlobEncoding> source;
	AssertIfFailed(library->CreateBlobFromFile(filepath.wstring().c_str(), nullptr, source.GetAddressOf()));

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
		// Compilation result
		result->GetResult(compiledBlob);

		// Validation
		DxcCreateInstanceProc dxil_create_func = (DxcCreateInstanceProc)GetProcAddress(s_validationModule, "DxcCreateInstance");
		Microsoft::WRL::ComPtr<IDxcValidator> validator;
		AssertIfFailed(dxil_create_func(CLSID_DxcValidator, IID_PPV_ARGS(validator.GetAddressOf())));

		Microsoft::WRL::ComPtr<IDxcOperationResult> signResult;
		AssertIfFailed(validator->Validate(*compiledBlob, DxcValidatorFlags_InPlaceEdit, signResult.GetAddressOf()));

		signResult->GetStatus(&hr);
		if (SUCCEEDED(hr))
		{
			return signResult->GetResult(compiledBlob);
		}
		else
		{
			Microsoft::WRL::ComPtr<IDxcBlobEncoding> error;
			signResult->GetErrorBuffer(error.GetAddressOf());
			return hr;
		}
	}
	else
	{
		Microsoft::WRL::ComPtr<IDxcBlobEncoding> error;
		result->GetErrorBuffer(error.GetAddressOf());
		return hr;
	}
}