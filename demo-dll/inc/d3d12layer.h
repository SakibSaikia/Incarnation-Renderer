#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <string>
#include <functional>
#include <optional>

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
using D3DRootSignature_t = ID3D12RootSignature;
using PhysicalAlloc_t = std::vector<uint32_t>;

class FResourceUploadContext;
struct IDxcBlob;

struct FCommandList
{
	FCommandList() = default;
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

struct FRootsigDesc
{
	std::wstring m_filename;
	std::wstring m_entrypoint;
};

struct FResource
{
	Microsoft::WRL::ComPtr<D3DResource_t> m_resource;
	D3D12_RESOURCE_DESC m_desc;
	std::optional<PhysicalAlloc_t> m_physicalPages;
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
		FCommandList FetchCommandlist(const D3D12_COMMAND_LIST_TYPE type);
		D3DFence_t* ExecuteCommandlists(const D3D12_COMMAND_LIST_TYPE commandQueueType, std::initializer_list<FCommandList> commandLists);

		// Root Signatures
		Microsoft::WRL::ComPtr<D3DRootSignature_t> FetchGraphicsRootSignature(const FRootsigDesc& rootsig);

		// Pipeline States
		D3DPipelineState_t* FetchGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);
		D3DPipelineState_t* FetchComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC&  desc);

		// Swap chain and back buffers
		D3D12_CPU_DESCRIPTOR_HANDLE GetBackBufferDescriptor();
		D3DResource_t* GetBackBufferResource();
		void PresentDisplay();

		// Shaders
		IDxcBlob* CacheShader(const FShaderDesc& shaderDesc, const std::wstring& profile);

		// Resource Management
		FResource CreateUploadBuffer(
			const std::wstring& name,
			const size_t size,
			std::function<void(uint8_t*)> uploadFunc = nullptr);

		FResource CreateDefaultBuffer(
			const std::wstring& name,
			const size_t size,
			D3D12_RESOURCE_STATES state,
			FResourceUploadContext* uploadContext,
			std::function<void(uint8_t*)> uploadFunc = nullptr);
	}
}