#pragma once

#include <windows.h>
#include <string>
#include <WindowsX.h>

namespace Demo
{
	extern "C" __declspec(dllexport) bool WINAPI Initialize(HINSTANCE instanceHandle, HWND& windowHandle, const uint32_t windowId);
	extern "C" __declspec(dllexport) void WINAPI Teardown(HWND & windowHandle);
	extern "C" __declspec(dllexport) void WINAPI Tick(float dt);
	extern "C" __declspec(dllexport) void WINAPI Render();

	bool InitializeWindow(HINSTANCE instanceHandle, HWND& windowHandle, const uint32_t windowId);
	void OnMouseMove(WPARAM btnState, int x, int y);
};
