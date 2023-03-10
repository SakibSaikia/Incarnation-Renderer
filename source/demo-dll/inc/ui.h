#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <imgui.h>

namespace Demo { struct App;  }

namespace UI
{
	void Initialize(const HWND& windowHandle);
	void Teardown();
	void Update(Demo::App* demoApp, const float deltaTime);
	bool HasFocus();
}