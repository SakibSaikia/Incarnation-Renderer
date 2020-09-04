#pragma once

#include <windows.h>
#include <string>
#include <WindowsX.h>

namespace Demo
{
	extern "C" __declspec(dllexport) bool WINAPI Initialize(HINSTANCE instanceHandle);
	extern "C" __declspec(dllexport) void WINAPI Teardown();
	extern "C" __declspec(dllexport) void WINAPI Tick(float dt);
	extern "C" __declspec(dllexport) void WINAPI Render();

	bool InitializeWindows(HINSTANCE instanceHandle);
	void OnMouseMove(WPARAM btnState, int x, int y);
};