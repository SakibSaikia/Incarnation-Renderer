#include <shadercompiler.h>
#include <winrt/base.h>
#include <common.h>
#include <system_error>
#include <vector>
#include <filesystem>
#include <sstream>

namespace
{
#if defined (_DEBUG)
	std::vector<LPCWSTR> k_compilerArguments = { L"-Zpr", L"-Zi", L"-Od", L"-WX", L"-I", SHADER_DIR, L"-I", SRC_INCLUDE_DIR};
#else
	std::vector<LPCWSTR> k_compilerArguments = { L"-Zpr", L"-I", SHADER_DIR, L"-I", SRC_INCLUDE_DIR };
#endif

	std::vector<LPCWSTR> k_rootsigArguments = { L"-I", SHADER_DIR, L"-I", SRC_INCLUDE_DIR };
}

namespace ShaderCompiler
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

bool ShaderCompiler::Initialize()
{
	s_validationModule = LoadLibraryEx(L"dxil.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	DebugAssert(s_validationModule, "Failed to load dxil.dll. Shaders will not be signed!");
	
	return s_validationModule != nullptr;
}

void ShaderCompiler::Teardown()
{
	FreeLibrary(s_validationModule);
}

FILETIME ShaderCompiler::GetLastModifiedTime(const std::wstring& relativepath)
{
	const std::wstring filepath = SHADER_DIR L"/" + relativepath;
	DebugAssert(std::filesystem::exists(std::filesystem::path(filepath)));

	_WIN32_FILE_ATTRIBUTE_DATA fileAttributeData{};
	GetFileAttributesExW(filepath.c_str(), GetFileExInfoStandard, &fileAttributeData);
	return fileAttributeData.ftLastWriteTime;
}

HRESULT ShaderCompiler::Preprocess(
	const std::wstring& relativepath,
	const std::wstring& defineStr,
	IDxcBlob** preprocessBlob)
{
	const std::wstring filepath = SHADER_DIR L"/" + relativepath;
	DebugAssert(std::filesystem::exists(std::filesystem::path(filepath)));

	winrt::com_ptr<IDxcUtils> utils;
	AssertIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(utils.put())));

	winrt::com_ptr<IDxcBlobEncoding> source;
	AssertIfFailed(utils->LoadFile(filepath.c_str(), nullptr, source.put()));

	winrt::com_ptr<IDxcIncludeHandler> includeHandler;
	AssertIfFailed(utils->CreateDefaultIncludeHandler(includeHandler.put()));

	winrt::com_ptr<IDxcCompiler> compiler;
	AssertIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.put())));

	std::vector<DxcDefine> defines{ DxcDefine{_wcsdup(L"__HLSL"), _wcsdup(L"1")} };
	std::wstring str = defineStr;
	size_t index = str.find_first_of(' ');
	while (!str.empty())
	{
		DxcDefine macro = {};

		std::wstring subString = str.substr(0, index);
		size_t subIndex = subString.find_first_of('=');

		if (subIndex != std::wstring::npos)
		{
			macro.Name = _wcsdup(subString.substr(0, subIndex).c_str());
			macro.Value = _wcsdup(subString.substr(subIndex + 1, subString.length()).c_str());
		}
		else
		{
			macro.Name = _wcsdup(subString.c_str());
		}

		defines.push_back(macro);

		str.erase(0, index + 1);
		index = str.find_first_of(' ');
		if (index == std::wstring::npos)
		{
			index = str.length();
		}
	}

	winrt::com_ptr<IDxcOperationResult> result;
	AssertIfFailed(compiler->Preprocess(
		source.get(),
		filepath.c_str(),
		k_compilerArguments.data(), (UINT)k_compilerArguments.size(),
		defines.data(), defines.size(),
		includeHandler.get(),
		result.put()));

	for (auto& def : defines)
	{
		delete def.Name;
		delete def.Value;
	}

	HRESULT hr;
	if (result && SUCCEEDED(result->GetStatus(&hr)))
	{
		if (SUCCEEDED(hr))
		{
			result->GetResult(preprocessBlob);
			return hr;
		}
		else
		{
			winrt::com_ptr<IDxcBlobEncoding> error;
			result->GetErrorBuffer(error.put());

			winrt::com_ptr<IDxcBlobUtf16> errorMessage;
			utils->GetBlobAsUtf16(error.get(), errorMessage.put());
			OutputDebugString((LPCWSTR)errorMessage->GetBufferPointer());

			return hr;
		}
	}
	else
	{
		OutputDebugStringA("Shader preprocessing failed");
		return E_FAIL;
	}
}

HRESULT ShaderCompiler::CompileShader(
	const std::wstring& relativepath, 
	const std::wstring& entrypoint, 
	const std::wstring& defineStr,
	const std::wstring& profile,
	IDxcBlob** compiledBlob)
{
	const std::wstring filepath = SHADER_DIR L"/" + relativepath;
	DebugAssert(std::filesystem::exists(std::filesystem::path(filepath)));

	winrt::com_ptr<IDxcUtils> utils;
	AssertIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(utils.put())));

	winrt::com_ptr<IDxcBlobEncoding> source;
	AssertIfFailed(utils->LoadFile(filepath.c_str(), nullptr, source.put()));

	winrt::com_ptr<IDxcIncludeHandler> includeHandler;
	AssertIfFailed(utils->CreateDefaultIncludeHandler(includeHandler.put()));

	winrt::com_ptr<IDxcCompiler> compiler;
	AssertIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.put())));

	std::vector<DxcDefine> defines{ DxcDefine{_wcsdup(L"__HLSL"), _wcsdup(L"1")} };
	std::wstring str = defineStr;
	size_t index = str.find_first_of(' ');
	while(!str.empty())
	{
		DxcDefine macro = {};

		std::wstring subString = str.substr(0, index);
		size_t subIndex = subString.find_first_of('=');

		if (subIndex != std::wstring::npos)
		{
			macro.Name = _wcsdup(subString.substr(0, subIndex).c_str());
			macro.Value = _wcsdup(subString.substr(subIndex + 1, subString.length()).c_str());
		}
		else
		{
			macro.Name = _wcsdup(subString.c_str());
		}

		defines.push_back(macro);

		str.erase(0, index + 1);
		index = str.find_first_of(' ');
		if (index == std::wstring::npos)
		{
			index = str.length();
		}
	}

	winrt::com_ptr<IDxcOperationResult> result;
	AssertIfFailed(compiler->Compile(
		source.get(),
		filepath.c_str(),
		entrypoint.c_str(),
		profile.c_str(),
		k_compilerArguments.data(), (UINT)k_compilerArguments.size(),
		defines.data(), defines.size(), 
		includeHandler.get(),
		result.put()));

	for (auto& def : defines)
	{
		delete def.Name;
		delete def.Value;
	}

	HRESULT hr;
	if (result && SUCCEEDED(result->GetStatus(&hr)))
	{
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

			winrt::com_ptr<IDxcBlobUtf16> errorMessage;
			utils->GetBlobAsUtf16(error.get(), errorMessage.put());
			OutputDebugString((LPCWSTR)errorMessage->GetBufferPointer());

			return hr;
		}
	}
	else
	{
		OutputDebugStringA("Shader compilation failed");
		return E_FAIL;
	}
}

HRESULT ShaderCompiler::CompileRootsignature(
	const std::wstring& relativepath,
	const std::wstring& entrypoint,
	const std::wstring& profile,
	IDxcBlob** compiledBlob)
{
	const std::wstring filepath = SHADER_DIR L"/" + relativepath;
	DebugAssert(std::filesystem::exists(std::filesystem::path(filepath)));

	winrt::com_ptr<IDxcUtils> utils;
	AssertIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(utils.put())));

	winrt::com_ptr<IDxcBlobEncoding> source;
	AssertIfFailed(utils->LoadFile(filepath.c_str(), nullptr, source.put()));

	winrt::com_ptr<IDxcIncludeHandler> includeHandler;
	AssertIfFailed(utils->CreateDefaultIncludeHandler(includeHandler.put()));

	winrt::com_ptr<IDxcCompiler> compiler;
	AssertIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.put())));

	winrt::com_ptr<IDxcOperationResult> result;
	AssertIfFailed(compiler->Compile(
		source.get(),
		filepath.c_str(),
		entrypoint.c_str(),
		profile.c_str(),
		k_rootsigArguments.data(), (UINT)k_rootsigArguments.size(),
		nullptr, 0,
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

		winrt::com_ptr<IDxcBlobUtf16> errorMessage;
		utils->GetBlobAsUtf16(error.get(), errorMessage.put());
		OutputDebugString((LPCWSTR)errorMessage->GetBufferPointer());

		return hr;
	}
}