#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <string>

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
using D3DPipelineState_t = ID3D12PipelineState;

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

struct FShaderDesc
{
	std::wstring m_filename;
	std::wstring m_entrypoint;
	std::wstring m_defines;
};

struct FGraphicsPipelineDesc
{
	FShaderDesc m_vs;
	FShaderDesc m_ps;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC m_state;
};

struct FComputePipelineDesc
{
	FShaderDesc m_cs;
	D3D12_COMPUTE_PIPELINE_STATE_DESC m_state;
};

namespace Demo
{
	namespace D3D12
	{
		bool Initialize(HWND& windowHandle);
		void Teardown();

		// Device
		D3DDevice_t* GetDevice();

		// Command Lists
		FCommandList* FetchCommandlist(const D3D12_COMMAND_LIST_TYPE type);
		D3DFence_t* ExecuteCommandlists(const D3D12_COMMAND_LIST_TYPE commandQueueType, std::vector<FCommandList*> commandLists);

		// Pipeline States
		D3DPipelineState_t* FetchGraphicsPipelineState(
			const FShaderDesc& vs,
			const FShaderDesc& ps,
			const D3D12_PRIMITIVE_TOPOLOGY_TYPE primitiveTopology,
			const DXGI_FORMAT dsvFormat,
			const uint32_t numRenderTargets,
			const std::initializer_list<DXGI_FORMAT>& rtvFormats,
			const std::initializer_list<D3D12_COLOR_WRITE_ENABLE>& colorWriteMask = { D3D12_COLOR_WRITE_ENABLE_ALL, D3D12_COLOR_WRITE_ENABLE_ALL, D3D12_COLOR_WRITE_ENABLE_ALL, D3D12_COLOR_WRITE_ENABLE_ALL, D3D12_COLOR_WRITE_ENABLE_ALL, D3D12_COLOR_WRITE_ENABLE_ALL, D3D12_COLOR_WRITE_ENABLE_ALL, D3D12_COLOR_WRITE_ENABLE_ALL },
			const bool depthEnable = true,
			const D3D12_DEPTH_WRITE_MASK& depthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL,
			const D3D12_COMPARISON_FUNC& depthFunc = D3D12_COMPARISON_FUNC_LESS);

		D3DPipelineState_t* FetchComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC  desc);

		// Swap chain and back buffers
		D3D12_CPU_DESCRIPTOR_HANDLE GetBackBufferDescriptor();
		D3DResource_t* GetBackBufferResource();
		void PresentDisplay();
	}
}