#include <demo.h>
#include <d3d12layer.h>
#include <settings.h>
#include <ppltasks.h>
#include <sstream>

namespace
{
	concurrency::task<FCommandList*> PreRenderJob()
	{
		return concurrency::create_task([]
		{
			FCommandList* cmdList = Demo::D3D12::AcquireCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			auto d3dCmdList = cmdList->m_cmdList;

			D3D12_RESOURCE_BARRIER barrierDesc = {};
			barrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrierDesc.Transition.pResource = Demo::D3D12::GetBackBufferResource();
			barrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			barrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrierDesc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			d3dCmdList->ResourceBarrier(1, &barrierDesc);

			return cmdList;
		});
	}

	concurrency::task<FCommandList*> RenderJob()
	{
		return concurrency::create_task([]
		{
			FCommandList* cmdList = Demo::D3D12::AcquireCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			auto d3dCmdList = cmdList->m_cmdList;

			D3D12_VIEWPORT viewport{ 0.f, 0.f, Demo::Settings::k_screenWidth, Demo::Settings::k_screenHeight, 0.f, 1.f };
			D3D12_RECT screenRect{ 0.f, 0.f, Demo::Settings::k_screenWidth, Demo::Settings::k_screenHeight };
			d3dCmdList->RSSetViewports(1, &viewport);
			d3dCmdList->RSSetScissorRects(1, &screenRect);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { Demo::D3D12::GetBackBufferDescriptor() };
			d3dCmdList->OMSetRenderTargets(1, rtvs, FALSE, nullptr);

			float clearColor[] = { .8f, .8f, 1.f, 0.f };
			d3dCmdList->ClearRenderTargetView(Demo::D3D12::GetBackBufferDescriptor(), clearColor, 0, nullptr);

			return cmdList;
		});
	}

	concurrency::task<FCommandList*> PresentJob()
	{
		return concurrency::create_task([]
		{
			FCommandList* cmdList = Demo::D3D12::AcquireCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			auto d3dCmdList = cmdList->m_cmdList;

			D3D12_RESOURCE_BARRIER barrierDesc = {};
			barrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrierDesc.Transition.pResource = Demo::D3D12::GetBackBufferResource();
			barrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
			barrierDesc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			d3dCmdList->ResourceBarrier(1, &barrierDesc);

			return cmdList;
		});
	}
}

void Demo::Render()
{
	auto preRenderCL = PreRenderJob().get();
	auto renderCL = RenderJob().get();
	auto presentCL = PresentJob().get();

	Demo::D3D12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { preRenderCL, renderCL, presentCL });
	Demo::D3D12::PresentDisplay();
}