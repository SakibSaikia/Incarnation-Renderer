#include <demo.h>
#include <d3d12layer.h>
#include <settings.h>
#include <sstream>

namespace Demo
{
	HWND s_windowHandle = {};

	// Mouse
	WPARAM s_mouseButtonState = {};
	POINT s_currentMousePos = { 0, 0 };
	POINT s_lastMousePos = { 0, 0 };
}

namespace
{
	void LockCursorToWindow(HWND& windowHandle)
	{
		RECT screenRect;
		GetWindowRect(windowHandle, &screenRect);
		ClipCursor(&screenRect);

		int windowCenterX = screenRect.left + (screenRect.right - screenRect.left) / 2;
		int windowCenterY = screenRect.top + (screenRect.bottom - screenRect.top) / 2;
		SetCursorPos(windowCenterX, windowCenterY);
	}

	LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		switch (msg)
		{
		case WM_KEYDOWN:
			if (wParam == VK_ESCAPE)
			{
				Demo::Teardown(Demo::s_windowHandle);
			}
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		case WM_MOUSEMOVE:
			Demo::OnMouseMove(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
			return 0;
		case WM_ACTIVATE:
			if (LOWORD(wParam) == WA_ACTIVE || LOWORD(wParam) == WA_CLICKACTIVE)
			{
				EnableWindow(hWnd, TRUE);
				LockCursorToWindow(Demo::s_windowHandle);
			}
			else if (LOWORD(wParam) == WA_INACTIVE)
			{
				EnableWindow(hWnd, FALSE);
			}
			return 0;
		}

		return DefWindowProc(hWnd, msg, wParam, lParam);
	}
}

bool Demo::InitializeWindow(HINSTANCE instanceHandle, HWND& windowHandle, const uint32_t windowId)
{
	std::wstringstream ss;
	ss << L"demo_window_" << windowId;
	std::wstring windowClassString = ss.str();

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
	desc.lpszClassName = _wcsdup(ss.str().c_str());

	if (!RegisterClass(&desc))
	{
		MessageBox(nullptr, TEXT("Failed to register window"), nullptr, 0);
		return false;
	}

	windowHandle = CreateWindowEx(
		WS_EX_APPWINDOW,
		_wcsdup(ss.str().c_str()),
		TEXT("demo"),
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		Demo::Settings::k_screenWidth,
		Demo::Settings::k_screenHeight,
		nullptr,
		nullptr,
		instanceHandle,
		nullptr
	);

	if (windowHandle == nullptr)
	{
		MessageBox(nullptr, TEXT("Failed to create window"), nullptr, 0);
		return false;
	}

	s_windowHandle = windowHandle;

	ShowWindow(windowHandle, SW_SHOW);
	UpdateWindow(windowHandle);

	ShowCursor(FALSE);

	LockCursorToWindow(windowHandle);
	return true;
}

bool Demo::Initialize(HINSTANCE instanceHandle, HWND& windowHandle, const uint32_t windowId)
{
	bool ok = InitializeWindow(instanceHandle, windowHandle, windowId);
	ok = ok && D3D12::Initialize(windowHandle);

	return ok;
}

void Demo::Tick(float dt)
{

}

void Demo::Teardown(HWND& windowHandle)
{
	if (windowHandle)
	{
		DestroyWindow(windowHandle);
		windowHandle = {};
	}
}

void Demo::OnMouseMove(WPARAM btnState, int x, int y)
{
	s_mouseButtonState = btnState;
	s_currentMousePos = { x, y };
}
