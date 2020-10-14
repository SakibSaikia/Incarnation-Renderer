#include <demo.h>
#include <profiling.h>
#include <backend-d3d12.h>
#include <shadercompiler.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <common.h>
#include <sstream>

namespace Demo
{
	// Mouse
	WPARAM s_mouseButtonState = {};
	POINT s_currentMousePos = { 0, 0 };
	POINT s_lastMousePos = { 0, 0 };
}

bool Demo::Initialize(const HWND& windowHandle, const uint32_t resX, const uint32_t resY)
{
	Profiling::Initialize();

	bool ok = RenderBackend12::Initialize(windowHandle, resX, resY);
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
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
		FResourceUploadContext uploader{ 32 * 1024 * 1024 };
		uint32_t fontSrvIndex = RenderBackend12::CacheTexture(L"imgui_fonts", &uploader);
		ImGui::GetIO().Fonts->TexID = (ImTextureID)fontSrvIndex;
		uploader.SubmitUploads(cmdList);
		RenderBackend12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { cmdList });

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
		RenderBackend12::Teardown();
		ShaderCompiler::Teardown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
	}
}

void Demo::OnMouseMove(WPARAM btnState, int x, int y)
{
	s_mouseButtonState = btnState;
	s_currentMousePos = { x, y };
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT Demo::WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
}
