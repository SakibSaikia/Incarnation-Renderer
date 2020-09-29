#include <Demo.h>
#include <time.h>
#include <strsafe.h>
#include <sstream>
#include <filesystem>
#include <cassert>

struct ModuleProcs
{
	decltype(Demo::Initialize)* init;
	decltype(Demo::Teardown)* teardown;
	decltype(Demo::Tick)* tick;
	decltype(Demo::Render)* render;
};

bool LoadModule(LPCWSTR modulePath, LPCWSTR moduleName, HMODULE& moduleHnd, ModuleProcs& exportedProcs)
{
	// Copy module to unique path before loading it
	std::wstringstream uniqueName;
	uniqueName << moduleName << L"_" << time(nullptr) << L".dll";

	std::wstringstream originalPath;
	originalPath << modulePath << L"/" << moduleName << L".dll";

	std::wstringstream copyPath;
	copyPath << modulePath << L"/" << uniqueName.str();

	if (!CopyFile(originalPath.str().c_str(), copyPath.str().c_str(), FALSE))
	{
		return false;
	}

	HMODULE newModuleHnd = LoadLibraryEx(uniqueName.str().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (!newModuleHnd)
	{
		return false;
	}

	if (moduleHnd)
	{
		FreeLibrary(moduleHnd);
	}

	moduleHnd = newModuleHnd;
	exportedProcs.init = reinterpret_cast<decltype(Demo::Initialize)*>(GetProcAddress(moduleHnd, "Initialize"));
	exportedProcs.teardown = reinterpret_cast<decltype(Demo::Teardown)*>(GetProcAddress(moduleHnd, "Teardown"));
	exportedProcs.tick = reinterpret_cast<decltype(Demo::Tick)*>(GetProcAddress(moduleHnd, "Tick"));
	exportedProcs.render = reinterpret_cast<decltype(Demo::Render)*>(GetProcAddress(moduleHnd, "Render"));

	return true;

}

// https://docs.microsoft.com/en-us/windows/win32/debug/retrieving-the-last-error-code
void ErrorExit(LPTSTR lpszFunction)
{
	// Retrieve the system error message for the last-error code

	LPVOID lpMsgBuf;
	LPVOID lpDisplayBuf;
	DWORD dw = GetLastError();

	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		dw,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf,
		0, NULL);

	// Display the error message and exit the process

	lpDisplayBuf = (LPVOID)LocalAlloc(LMEM_ZEROINIT,
		(lstrlen((LPCTSTR)lpMsgBuf) + lstrlen((LPCTSTR)lpszFunction) + 40) * sizeof(TCHAR));
	StringCchPrintf((LPTSTR)lpDisplayBuf,
		LocalSize(lpDisplayBuf) / sizeof(TCHAR),
		TEXT("%s failed with error %d: %s"),
		lpszFunction, dw, lpMsgBuf);
	MessageBox(NULL, (LPCTSTR)lpDisplayBuf, TEXT("Error"), MB_OK);

	LocalFree(lpMsgBuf);
	LocalFree(lpDisplayBuf);
	ExitProcess(dw);
}

bool GetFileLastWriteTime(LPCWSTR filePath, FILETIME& writeTime)
{
	_WIN32_FILE_ATTRIBUTE_DATA fileAttributeData{};
	bool ok = GetFileAttributesExW(filePath, GetFileExInfoStandard, &fileAttributeData);
	writeTime = fileAttributeData.ftLastWriteTime;
	return ok;
}

void CleanTempFiles(LPCWSTR path, LPCWSTR namePrefex)
{
	for (auto& dirEntry : std::filesystem::directory_iterator(path))
	{
		if (dirEntry.is_regular_file() && dirEntry.path().filename().wstring()._Starts_with(namePrefex))
		{
			DeleteFile(dirEntry.path().wstring().c_str());
		}
	}
}

int WINAPI main(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nShowCmd)
{
	AddDllDirectory(LIB_DEMO_DIR);
	AddDllDirectory(SPOOKYHASH_BIN_DIR);
	AddDllDirectory(DXC_BIN_DIR);
	AddDllDirectory(PIX_BIN_DIR);

	HMODULE demoDll{};
	ModuleProcs demoProcs{};

	FILETIME lastWriteTimestamp{};

	MSG msg = {};

	uint32_t windowId = 0;
	HWND windowHandle = {};

	CleanTempFiles(LIB_DEMO_DIR, LIB_DEMO_NAME L"_");

	while (msg.message != WM_QUIT)
	{
		FILETIME currentWriteTimestamp{};
		if (GetFileLastWriteTime(LIB_DEMO_DIR L"\\" LIB_DEMO_NAME L".dll", currentWriteTimestamp))
		{
			if (currentWriteTimestamp.dwLowDateTime != lastWriteTimestamp.dwLowDateTime ||
				currentWriteTimestamp.dwHighDateTime != lastWriteTimestamp.dwHighDateTime)
			{
				if (demoProcs.teardown)
				{
					demoProcs.teardown(windowHandle);
				}

				if (!LoadModule(LIB_DEMO_DIR, LIB_DEMO_NAME, demoDll, demoProcs))
				{
					ErrorExit(L"LoadModule");
				}
				
				demoProcs.init(hInstance, windowHandle, windowId++);
				lastWriteTimestamp = currentWriteTimestamp;
			}
		}

		if (!windowHandle)
		{
			continue;
		}

		if (PeekMessage(&msg, windowHandle, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else
		{
			demoProcs.tick(0.0166f);
			demoProcs.render();
		}
	}

	FreeLibrary(demoDll);

	return static_cast<int>(msg.wParam);
}
