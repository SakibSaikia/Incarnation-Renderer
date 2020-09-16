#include <demo.h>
#include <d3d12layer.h>
#include <settings.h>
#include <ppltasks.h>
#include <sstream>

namespace
{
	auto renderJob = concurrency::create_task([]() -> FCommandList*
	{
		FCommandList* cmdList = Demo::D3D12::AcquireCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
		auto d3dCmdList = cmdList->Get<D3DGraphicsCommandList_t>();

		D3D12_VIEWPORT viewport{ 0.f, 0.f, Demo::Settings::k_screenWidth, Demo::Settings::k_screenHeight, 0.f, 1.f };
		D3D12_RECT screenRect{ 0.f, 0.f, Demo::Settings::k_screenWidth, Demo::Settings::k_screenHeight };
		d3dCmdList->RSSetViewports(1, &viewport);
		d3dCmdList->RSSetScissorRects(1, &screenRect);

		float clearColor[] = { .8f, .8f, 1.f, 0.f };
		d3dCmdList->ClearRenderTargetView(Demo::D3D12::GetBackBuffer(), clearColor, 0, nullptr);

		return cmdList;
	});
}

void Demo::Render()
{

}