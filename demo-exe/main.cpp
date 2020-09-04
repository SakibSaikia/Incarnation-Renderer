#include "Demo.h"
#include <time.h>
#include <sstream>

struct ModuleProcs
{
	decltype(Demo::Initialize)* init;
	decltype(Demo::Initialize)* teardown;
	decltype(Demo::Tick)* tick;
	decltype(Demo::Render)* render;
};

ModuleProcs LoadModule(LPCWSTR modulePath, LPCWSTR moduleName, HMODULE& moduleHnd)
{
	// Copy module to unique path before loading it
	std::wstringstream uniqueName;
	uniqueName << moduleName << L"_" << time(nullptr) << L".dll";

	std::wstringstream originalPath;
	originalPath << modulePath << L"/" << moduleName << L".dll";

	std::wstringstream copyPath;
	copyPath << modulePath << L"/" << uniqueName.str();

	CopyFile(originalPath.str().c_str(), copyPath.str().c_str(), FALSE);

	if (moduleHnd)
	{
		FreeLibrary(moduleHnd);
	}

	moduleHnd = LoadLibrary(uniqueName.str().c_str());

	ModuleProcs exportedProcs;
	exportedProcs.init = reinterpret_cast<decltype(Demo::Initialize)*>(GetProcAddress(moduleHnd, "Initialize"));
	exportedProcs.teardown = reinterpret_cast<decltype(Demo::Initialize)*>(GetProcAddress(moduleHnd, "Teardown"));
	exportedProcs.tick = reinterpret_cast<decltype(Demo::Tick)*>(GetProcAddress(moduleHnd, "Tick"));
	exportedProcs.render = reinterpret_cast<decltype(Demo::Render)*>(GetProcAddress(moduleHnd, "Render"));

	return exportedProcs;

}

bool GetFileLastWriteTime(LPCWSTR filePath, FILETIME& writeTime)
{
	_WIN32_FILE_ATTRIBUTE_DATA fileAttributeData{};
	bool ok = GetFileAttributesExW(filePath, GetFileExInfoStandard, &fileAttributeData);
	writeTime = fileAttributeData.ftLastWriteTime;
	return ok;
}

int WINAPI main(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nShowCmd)
{
	SetDllDirectory(LIB_DEMO_DIR);
	HMODULE demoDll{};
	ModuleProcs demoProcs{};

	FILETIME lastWriteTimestamp{};

	MSG msg = {};
	while (msg.message != WM_QUIT)
	{
		FILETIME currentWriteTimestamp{};
		if (GetFileLastWriteTime(LIB_DEMO_DIR L"\\libdemo.dll", currentWriteTimestamp))
		{
			if (currentWriteTimestamp.dwLowDateTime != lastWriteTimestamp.dwLowDateTime ||
				currentWriteTimestamp.dwHighDateTime != lastWriteTimestamp.dwHighDateTime)
			{
				demoProcs = LoadModule(LIB_DEMO_DIR, TEXT("libdemo"), demoDll);
				demoProcs.init(hInstance);
				lastWriteTimestamp = currentWriteTimestamp;
			}
		}

		if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
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