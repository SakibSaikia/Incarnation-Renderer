#include <demo.h>
#include <d3d12layer.h>
#include <settings.h>
#include <ppltasks.h>
#include <sstream>
#include <assert.h>

namespace Jobs
{
	concurrency::task<FCommandList*> PreRender()
	{
		return concurrency::create_task([]
		{
			FCommandList* cmdList = Demo::D3D12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			D3DCommandList_t* d3dCmdList = cmdList->m_cmdList.Get();
			d3dCmdList->SetName(L"PreRenderJob CL");

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

	concurrency::task<FCommandList*> Render()
	{
		return concurrency::create_task([]
		{
			FCommandList* cmdList = Demo::D3D12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			D3DCommandList_t* d3dCmdList = cmdList->m_cmdList.Get();
			d3dCmdList->SetName(L"RenderJob CL");

			Microsoft::WRL::ComPtr<D3DRootSignature_t> rootsig = Demo::D3D12::FetchGraphicsRootSignature({ L"rootsig.hlsl", L"graphics_rootsig_main" });
			d3dCmdList->SetGraphicsRootSignature(rootsig.Get());

			D3DPipelineState_t* pso = Demo::D3D12::FetchGraphicsPipelineState(
				{ L"rootsig.hlsl", L"graphics_rootsig_main"},
				{ L"hello.hlsl", L"vs_main", L"" },
				{ L"hello.hlsl", L"ps_main", L"" },
				D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
				DXGI_FORMAT_UNKNOWN,
				1,
				{ Demo::Settings::k_backBufferFormat },
				{ D3D12_COLOR_WRITE_ENABLE_ALL },
				false
			);

			d3dCmdList->SetPipelineState(pso);

			D3D12_VIEWPORT viewport{ 0.f, 0.f, Demo::Settings::k_screenWidth, Demo::Settings::k_screenHeight, 0.f, 1.f };
			D3D12_RECT screenRect{ 0.f, 0.f, Demo::Settings::k_screenWidth, Demo::Settings::k_screenHeight };
			d3dCmdList->RSSetViewports(1, &viewport);
			d3dCmdList->RSSetScissorRects(1, &screenRect);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { Demo::D3D12::GetBackBufferDescriptor() };
			d3dCmdList->OMSetRenderTargets(1, rtvs, FALSE, nullptr);

			float clearColor[] = { .8f, .8f, 1.f, 0.f };
			d3dCmdList->ClearRenderTargetView(Demo::D3D12::GetBackBufferDescriptor(), clearColor, 0, nullptr);

			d3dCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			d3dCmdList->DrawInstanced(3, 1, 0, 0);

			return cmdList;
		});
	}

	concurrency::task<FCommandList*> Present()
	{
		return concurrency::create_task([]
		{
			FCommandList* cmdList = Demo::D3D12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			D3DCommandList_t* d3dCmdList = cmdList->m_cmdList.Get();
			d3dCmdList->SetName(L"PresentJob CL");

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
	auto preRenderCL = Jobs::PreRender().get();
	auto renderCL = Jobs::Render().get();
	auto presentCL = Jobs::Present().get();

	Demo::D3D12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { preRenderCL, renderCL, presentCL });
	Demo::D3D12::PresentDisplay();
}