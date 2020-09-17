#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl.h>
#include <vector>

// Aliased types
using DXGIFactory_t = IDXGIFactory4;
using DXGIAdapter_t = IDXGIAdapter;
using DXGISwapChain_t = IDXGISwapChain3;
using D3DDebug_t = ID3D12Debug;
using D3DDevice_t = ID3D12Device5;
using D3DCommandQueue_t = ID3D12CommandQueue;
using D3DCommandAllocator_t = ID3D12CommandAllocator;
using D3DDescriptorHeap_t = ID3D12DescriptorHeap;
using D3DResource_t = ID3D12Resource;
using D3DCommandList_t = ID3D12GraphicsCommandList4;
using D3DFence_t = ID3D12Fence1;

struct FCommandList
{
	FCommandList() = delete;
	FCommandList(const D3D12_COMMAND_LIST_TYPE type, const size_t  fenceValue);

	D3D12_COMMAND_LIST_TYPE m_type;
	size_t m_fenceValue;
	Microsoft::WRL::ComPtr<D3DCommandList_t> m_cmdList;
	Microsoft::WRL::ComPtr<D3DCommandAllocator_t> m_cmdAllocator;
	Microsoft::WRL::ComPtr<D3DFence_t> m_fence;
};

namespace Demo
{
	namespace D3D12
	{
		bool Initialize(HWND& windowHandle);
		void Teardown();

		D3DDevice_t* GetDevice();

		FCommandList* AcquireCommandlist(const D3D12_COMMAND_LIST_TYPE type);
		D3DFence_t* ExecuteCommandlists(const D3D12_COMMAND_LIST_TYPE commandQueueType, std::vector<FCommandList*> commandLists);

		D3D12_CPU_DESCRIPTOR_HANDLE GetBackBufferDescriptor();
		D3DResource_t* GetBackBufferResource();

		void PresentDisplay();
	}
}