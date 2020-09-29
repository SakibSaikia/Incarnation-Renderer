#include <demo.h>
#include <d3d12layer.h>
#include <shadercompiler.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
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

	return true;
}

bool Demo::Initialize(HINSTANCE instanceHandle, HWND& windowHandle, const uint32_t windowId)
{
	bool ok = InitializeWindow(instanceHandle, windowHandle, windowId);
	ok = ok && D3D12::Initialize(windowHandle);
	ok = ok && ShaderCompiler::Initialize();

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init(windowHandle);

	return ok;
}

void Demo::Tick(float dt)
{
	{
		FCommandList cmdList = Demo::D3D12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
		FResourceUploadContext uploader{ 32 * 1024 * 1024 };
		uint32_t fontSrvIndex = Demo::D3D12::CacheTexture(L"imgui_fonts", &uploader);
		ImGui::GetIO().Fonts->TexID = (ImTextureID)fontSrvIndex;
		uploader.SubmitUploads(&cmdList);
		Demo::D3D12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		static bool show_demo_window = true;
		static bool show_another_window = false;
		static ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

		static float f = 0.0f;
		static int counter = 0;

		ImGui::Begin("Hello, world!");                          // Create a window called "Hello, world!" and append into it.

		ImGui::Text("This is some useful text.");               // Display some text (you can use a format strings too)
		ImGui::Checkbox("Demo Window", &show_demo_window);      // Edit bools storing our window open/close state
		ImGui::Checkbox("Another Window", &show_another_window);

		ImGui::SliderFloat("float", &f, 0.0f, 1.0f);            // Edit 1 float using a slider from 0.0f to 1.0f
		ImGui::ColorEdit3("clear color", (float*)&clear_color); // Edit 3 floats representing a color

		if (ImGui::Button("Button"))                            // Buttons return true when clicked (most widgets return true when edited/activated)
			counter++;
		ImGui::SameLine();
		ImGui::Text("counter = %d", counter);

		ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
		ImGui::End();

		ImGui::EndFrame();
		ImGui::Render();
	}
}

void Demo::Teardown(HWND& windowHandle)
{
	if (windowHandle)
	{
		D3D12::Teardown();
		ShaderCompiler::Teardown();
		//ImGui_ImplWin32_Shutdown();
		//ImGui::DestroyContext();
		DestroyWindow(windowHandle);
		windowHandle = {};
	}
}

void Demo::OnMouseMove(WPARAM btnState, int x, int y)
{
	s_mouseButtonState = btnState;
	s_currentMousePos = { x, y };
}
