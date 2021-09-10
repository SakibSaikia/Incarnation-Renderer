#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <WindowsX.h>

namespace Demo
{
	extern "C" __declspec(dllexport) bool WINAPI Initialize(const HWND& windowHandle, const uint32_t resX, const uint32_t resY);
	extern "C" __declspec(dllexport) void WINAPI Teardown(HWND & windowHandle);
	extern "C" __declspec(dllexport) void WINAPI Tick(float dt);
	extern "C" __declspec(dllexport) void WINAPI Render(const uint32_t resX, const uint32_t resY);
	extern "C" __declspec(dllexport) void WINAPI OnMouseMove(WPARAM btnState, int x, int y);
	extern "C" __declspec(dllexport) LRESULT WINAPI WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
}
