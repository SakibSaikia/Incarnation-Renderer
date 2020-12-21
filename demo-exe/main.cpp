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
	decltype(Demo::OnMouseMove)* mouseMove;
	decltype(Demo::WndProcHandler)* wndProc;
};

static ModuleProcs s_demoProcs = {};

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

	moduleHnd = newModuleHnd;
	exportedProcs.init = reinterpret_cast<decltype(Demo::Initialize)*>(GetProcAddress(moduleHnd, "Initialize"));
	exportedProcs.teardown = reinterpret_cast<decltype(Demo::Teardown)*>(GetProcAddress(moduleHnd, "Teardown"));
	exportedProcs.tick = reinterpret_cast<decltype(Demo::Tick)*>(GetProcAddress(moduleHnd, "Tick"));
	exportedProcs.render = reinterpret_cast<decltype(Demo::Render)*>(GetProcAddress(moduleHnd, "Render"));
	exportedProcs.mouseMove = reinterpret_cast<decltype(Demo::OnMouseMove)*>(GetProcAddress(moduleHnd, "OnMouseMove"));
	exportedProcs.wndProc = reinterpret_cast<decltype(Demo::WndProcHandler)*>(GetProcAddress(moduleHnd, "WndProcHandler"));

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

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if(s_demoProcs.wndProc &&
		s_demoProcs.wndProc(hWnd, msg, wParam, lParam))
	{
		return true;
	}

	switch (msg)
	{
	case WM_KEYDOWN:
		if (wParam == VK_ESCAPE)
		{
			if (s_demoProcs.teardown)
			{
				s_demoProcs.teardown(hWnd);
			}
		}
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_MOUSEMOVE:
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
		if (s_demoProcs.mouseMove)
		{
			s_demoProcs.mouseMove(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		}
		return 0;
	case WM_ACTIVATE:
		if (LOWORD(wParam) == WA_ACTIVE || LOWORD(wParam) == WA_CLICKACTIVE)
		{
			EnableWindow(hWnd, TRUE);
		}
		else if (LOWORD(wParam) == WA_INACTIVE)
		{
			EnableWindow(hWnd, FALSE);
		}
		return 0;
	}

	return DefWindowProc(hWnd, msg, wParam, lParam);
}

bool InitializeWindow(HINSTANCE instanceHandle, HWND& windowHandle, const uint32_t resX, const uint32_t resY)
{
	WNDCLASS desc;
	desc.style = CS_HREDRAW | CS_VREDRAW;
	desc.lpfnWndProc = WndProc;
	desc.cbClsExtra = 0;
	desc.cbWndExtra = 0;
	desc.hInstance = instanceHandle;
	desc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
	desc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	desc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW);
	desc.lpszMenuName = nullptr;
	desc.lpszClassName = L"demo_window";

	if (!RegisterClass(&desc))
	{
		MessageBox(nullptr, TEXT("Failed to register window"), nullptr, 0);
		return false;
	}

	windowHandle = CreateWindowEx(
		WS_EX_APPWINDOW,
		L"demo_window",
		TEXT("demo"),
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		resX,
		resY,
		nullptr,
		nullptr,
		instanceHandle,
		nullptr
	);

	if (!windowHandle)
	{
		MessageBox(nullptr, TEXT("Failed to create window"), nullptr, 0);
		return false;
	}
	else
	{
		ShowWindow(windowHandle, SW_SHOW);
		UpdateWindow(windowHandle);
		return true;
	}
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nShowCmd)
{
	AddDllDirectory(LIB_DEMO_DIR);
	AddDllDirectory(SPOOKYHASH_BIN_DIR);
	AddDllDirectory(DXC_BIN_DIR);
	AddDllDirectory(PIX_BIN_DIR);

	const uint32_t windowWidth = 1280;
	const uint32_t windowHeight = 720;

	HMODULE demoDll = {};
	FILETIME lastWriteTimestamp = {};
	MSG msg = {};
	HWND windowHandle = {};

	LARGE_INTEGER counterFrequency, startTime, endTime;
	float elapsedSeconds = 0.016;
	QueryPerformanceFrequency(&counterFrequency);

	InitializeWindow(hInstance, windowHandle, windowWidth, windowHeight);
	CleanTempFiles(LIB_DEMO_DIR, LIB_DEMO_NAME L"_");

	while (msg.wParam != VK_ESCAPE)
	{
		FILETIME currentWriteTimestamp{};
		if (GetFileLastWriteTime(LIB_DEMO_DIR L"\\" LIB_DEMO_NAME L".dll", currentWriteTimestamp))
		{
			if (currentWriteTimestamp.dwLowDateTime != lastWriteTimestamp.dwLowDateTime ||
				currentWriteTimestamp.dwHighDateTime != lastWriteTimestamp.dwHighDateTime)
			{
				if (s_demoProcs.teardown)
				{
					s_demoProcs.teardown(windowHandle);
				}

				if (!LoadModule(LIB_DEMO_DIR, LIB_DEMO_NAME, demoDll, s_demoProcs))
				{
					ErrorExit((LPTSTR)TEXT("LoadModule"));
				}
				
				s_demoProcs.init(windowHandle, windowWidth, windowHeight);
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
			QueryPerformanceCounter(&startTime);
			s_demoProcs.tick(elapsedSeconds);
			s_demoProcs.render(windowWidth, windowHeight);
			QueryPerformanceCounter(&endTime);
			elapsedSeconds = (endTime.QuadPart - startTime.QuadPart) / (float) counterFrequency.QuadPart;
		}
	}

	DestroyWindow(windowHandle);

	return static_cast<int>(msg.wParam);
}
