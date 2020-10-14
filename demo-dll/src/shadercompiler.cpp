#include <shadercompiler.h>
#include <winrt/base.h>
#include <common.h>
#include <system_error>
#include <vector>
#include <filesystem>
#include <sstream>

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
	DebugAssert(s_validationModule, "Failed to load dxil.dll. Shaders will not be signed!");
	
	return s_validationModule != nullptr;
}

void Demo::ShaderCompiler::Teardown()
{
	FreeLibrary(s_validationModule);
}

FILETIME Demo::ShaderCompiler::GetLastModifiedTime(const std::wstring& filename)
{
	const std::filesystem::path filepath = SearchShaderDir(filename);
	DebugAssert(!filepath.empty(), "Shader source file not found");

	_WIN32_FILE_ATTRIBUTE_DATA fileAttributeData{};
	GetFileAttributesExW(filepath.wstring().c_str(), GetFileExInfoStandard, &fileAttributeData);
	return fileAttributeData.ftLastWriteTime;
}

HRESULT Demo::ShaderCompiler::CompileShader(
	const std::wstring& filename, 
	const std::wstring& entrypoint, 
	const std::wstring& arguments,
	const std::wstring& profile,
	IDxcBlob** compiledBlob)
{
	const std::filesystem::path filepath = SearchShaderDir(filename);
	DebugAssert(!filepath.empty(), "Shader source file not found");

	winrt::com_ptr<IDxcLibrary> library;
	AssertIfFailed(DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(library.put())));

	winrt::com_ptr<IDxcBlobEncoding> source;
	AssertIfFailed(library->CreateBlobFromFile(filepath.wstring().c_str(), nullptr, source.put()));

	winrt::com_ptr<IDxcIncludeHandler> includeHandler;
	AssertIfFailed(library->CreateIncludeHandler(includeHandler.put()));

	winrt::com_ptr<IDxcCompiler> compiler;
	AssertIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.put())));

	winrt::com_ptr<IDxcOperationResult> result;
	AssertIfFailed(compiler->Compile(
		source.get(),
		filename.c_str(),
		entrypoint.c_str(),
		profile.c_str(),
		Settings::k_compilerArguments.data(), (UINT)Settings::k_compilerArguments.size(),
		nullptr, 0, 
		includeHandler.get(),
		result.put()));

	HRESULT hr;
	result->GetStatus(&hr);
	if (SUCCEEDED(hr)) 
	{
		// Compilation result
		result->GetResult(compiledBlob);

		// Validation
		DxcCreateInstanceProc dxil_create_func = (DxcCreateInstanceProc)GetProcAddress(s_validationModule, "DxcCreateInstance");
		winrt::com_ptr<IDxcValidator> validator;
		AssertIfFailed(dxil_create_func(CLSID_DxcValidator, IID_PPV_ARGS(validator.put())));

		winrt::com_ptr<IDxcOperationResult> signResult;
		AssertIfFailed(validator->Validate(*compiledBlob, DxcValidatorFlags_InPlaceEdit, signResult.put()));

		signResult->GetStatus(&hr);
		if (SUCCEEDED(hr))
		{
			return signResult->GetResult(compiledBlob);
		}
		else
		{
			winrt::com_ptr<IDxcBlobEncoding> error;
			signResult->GetErrorBuffer(error.put());
			return hr;
		}
	}
	else
	{
		winrt::com_ptr<IDxcBlobEncoding> error;
		result->GetErrorBuffer(error.put());

		winrt::com_ptr<IDxcBlobEncoding> errorMessage;
		library->GetBlobAsUtf16(error.get(), errorMessage.put());
		OutputDebugString((LPCWSTR)errorMessage->GetBufferPointer());

		return hr;
	}
}

HRESULT Demo::ShaderCompiler::CompileRootsignature(
	const std::wstring& filename,
	const std::wstring& entrypoint,
	const std::wstring& profile,
	IDxcBlob** compiledBlob)
{
	const std::filesystem::path filepath = SearchShaderDir(filename);
	DebugAssert(!filepath.empty(), "Rootsignature source file not found");

	winrt::com_ptr<IDxcLibrary> library;
	AssertIfFailed(DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(library.put())));

	winrt::com_ptr<IDxcBlobEncoding> source;
	AssertIfFailed(library->CreateBlobFromFile(filepath.wstring().c_str(), nullptr, source.put()));

	winrt::com_ptr<IDxcIncludeHandler> includeHandler;
	AssertIfFailed(library->CreateIncludeHandler(includeHandler.put()));

	winrt::com_ptr<IDxcCompiler> compiler;
	AssertIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.put())));

	std::wstringstream s;
	s << k_bindlessSrvHeapSize;
	DxcDefine defines[] = 
	{ 
		{L"BINDLESS_DESCRIPTOR_COUNT", s.str().c_str()} 
	};

	winrt::com_ptr<IDxcOperationResult> result;
	AssertIfFailed(compiler->Compile(
		source.get(),
		filename.c_str(),
		entrypoint.c_str(),
		profile.c_str(),
		nullptr, 0,
		defines, std::size(defines),
		includeHandler.get(),
		result.put()));

	HRESULT hr;
	result->GetStatus(&hr);
	if (SUCCEEDED(hr))
	{
		// Compilation result
		return result->GetResult(compiledBlob);
	}
	else
	{
		winrt::com_ptr<IDxcBlobEncoding> error;
		result->GetErrorBuffer(error.put());

		winrt::com_ptr<IDxcBlobEncoding> errorMessage;
		library->GetBlobAsUtf16(error.get(), errorMessage.put());
		OutputDebugString((LPCWSTR)errorMessage->GetBufferPointer());

		return hr;
	}
}