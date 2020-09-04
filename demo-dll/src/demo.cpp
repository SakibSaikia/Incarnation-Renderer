#include "demo.h"
#include "settings.h"

namespace Demo
{
	HWND s_windowHandle;

	// Mouse
	WPARAM s_mouseButtonState = {};
	POINT s_currentMousePos = { 0, 0 };
	POINT s_lastMousePos = { 0, 0 };
}

namespace
{
	void LockCursorToWindow()
	{
		RECT screenRect;
		GetWindowRect(Demo::s_windowHandle, &screenRect);
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
				Demo::Teardown();
				DestroyWindow(Demo::s_windowHandle);
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
				LockCursorToWindow();
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

bool Demo::InitializeWindows(HINSTANCE instanceHandle)
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
	desc.lpszClassName = TEXT("demo_window");

	if (!RegisterClass(&desc))
	{
		MessageBox(nullptr, TEXT("Failed to register window"), nullptr, 0);
		return false;
	}

	s_windowHandle = CreateWindow(
		TEXT("demo_window"),
		TEXT("demo"),
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		Settings::k_screenWidth,
		Settings::k_screenHeight,
		nullptr,
		nullptr,
		instanceHandle,
		nullptr
	);

	if (s_windowHandle == nullptr)
	{
		MessageBox(nullptr, TEXT("Failed to create window"), nullptr, 0);
		return false;
	}

	ShowWindow(s_windowHandle, SW_SHOW);
	UpdateWindow(s_windowHandle);

	ShowCursor(FALSE);

	LockCursorToWindow();
	return true;
}

bool Demo::Initialize(HINSTANCE instanceHandle)
{
	return InitializeWindows(instanceHandle);
}

void Demo::Tick(float dt)
{

}

void Demo::Render()
{

}

void Demo::Teardown()
{

}

void Demo::OnMouseMove(WPARAM btnState, int x, int y)
{
	s_mouseButtonState = btnState;
	s_currentMousePos = { x, y };
}
