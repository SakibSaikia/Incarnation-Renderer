#define WIN32_LEAN_AND_MEAN
#include <d3d12layer.h>
#include <settings.h>
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl.h>
#include <cassert>
#include <string>
#include <sstream>
#include <array>

using DXGIFactory_t = IDXGIFactory4;
using DXGIAdapter_t = IDXGIAdapter;
using DXGISwapChain_t = IDXGISwapChain3;
using D3DDebug_t = ID3D12Debug;
using D3DDevice_t = ID3D12Device5;
using D3DCommandQueue_t = ID3D12CommandQueue;
using D3DCommandAllocator_t = ID3D12CommandAllocator;
using D3DDescriptorHeap_t = ID3D12DescriptorHeap;
using D3DResource_t = ID3D12Resource;

using namespace Demo::D3D12;

constexpr size_t k_bindlessSrvHeapSize = 1000;
constexpr size_t k_backBufferCount = 2;
constexpr DXGI_FORMAT k_backBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

namespace
{
	Microsoft::WRL::ComPtr<DXGIAdapter_t> EnumerateAdapters(DXGIFactory_t* dxgiFactory)
	{
		Microsoft::WRL::ComPtr<DXGIAdapter_t> bestAdapter;
		size_t maxVram = 0;

		Microsoft::WRL::ComPtr<DXGIAdapter_t> adapter;
		uint32_t i = 0;

		while (dxgiFactory->EnumAdapters(i++, adapter.GetAddressOf()) != DXGI_ERROR_NOT_FOUND)
		{
			DXGI_ADAPTER_DESC desc;
			adapter->GetDesc(&desc);

			if (desc.DedicatedVideoMemory > maxVram)
			{
				maxVram = desc.DedicatedVideoMemory;
				bestAdapter = adapter;
			}
		}

		DXGI_ADAPTER_DESC desc;
		bestAdapter->GetDesc(&desc);

		std::wstringstream out;
		out << L"*** Adapter : " << desc.Description << std::endl;
		OutputDebugString(out.str().c_str());

		return bestAdapter;
	}
}


namespace Demo
{
	namespace D3D12
	{
#if defined(DEBUG) || defined (_DEBUG)
		Microsoft::WRL::ComPtr<D3DDebug_t> s_debugController;
#endif

		Microsoft::WRL::ComPtr<DXGIFactory_t> s_dxgiFactory;
		Microsoft::WRL::ComPtr<D3DDevice_t> s_d3dDevice;

		Microsoft::WRL::ComPtr<DXGISwapChain_t> s_swapChain;
		std::array<Microsoft::WRL::ComPtr<D3DResource_t>, k_backBufferCount> s_backBuffers;
		uint32_t s_currentBufferIndex;

		std::array<uint32_t, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> s_descriptorSize;
		std::array< Microsoft::WRL::ComPtr<D3DDescriptorHeap_t>, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> s_descriptorHeaps;

		Microsoft::WRL::ComPtr<D3DCommandQueue_t> s_graphicsQueue;
		Microsoft::WRL::ComPtr<D3DCommandQueue_t> s_computeQueue;
		Microsoft::WRL::ComPtr<D3DCommandQueue_t> s_copyQueue;
	}
} 


bool Demo::D3D12::Initialize(HWND& windowHandle)
{
	// Debug layer
#if defined(DEBUG) || defined(_DEBUG)
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(s_debugController.GetAddressOf()))))
	{
		s_debugController->EnableDebugLayer();
	}
#endif

	// DXGI Factory
	assert(SUCCEEDED(
		CreateDXGIFactory1(IID_PPV_ARGS(s_dxgiFactory.GetAddressOf()))
	));

	// Adapter
	Microsoft::WRL::ComPtr<DXGIAdapter_t> adapter = EnumerateAdapters(s_dxgiFactory.Get());

	// Device
	assert(SUCCEEDED(
		D3D12CreateDevice(
			adapter.Get(),
			D3D_FEATURE_LEVEL_12_1,
			IID_PPV_ARGS(s_d3dDevice.GetAddressOf()))
	));

	// Cache descriptor sizes
	for (int typeId = 0; typeId < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++typeId)
	{
		s_descriptorSize[typeId] = s_d3dDevice->GetDescriptorHandleIncrementSize(static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(typeId));
	}

	// Command Queue(s)
	D3D12_COMMAND_QUEUE_DESC cmdQueueDesc = {};
	cmdQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	cmdQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	assert(SUCCEEDED(
		s_d3dDevice->CreateCommandQueue(
			&cmdQueueDesc,
			IID_PPV_ARGS(s_graphicsQueue.GetAddressOf()))
	));

	cmdQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
	assert(SUCCEEDED(
		s_d3dDevice->CreateCommandQueue(
			&cmdQueueDesc,
			IID_PPV_ARGS(s_computeQueue.GetAddressOf()))
	));

	cmdQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
	assert(SUCCEEDED(
		s_d3dDevice->CreateCommandQueue(
			&cmdQueueDesc,
			IID_PPV_ARGS(s_copyQueue.GetAddressOf()))
	));

	// Bindless SRV heap
	D3D12_DESCRIPTOR_HEAP_DESC cbvSrvUavHeapDesc = {};
	cbvSrvUavHeapDesc.NumDescriptors = k_bindlessSrvHeapSize;
	cbvSrvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	cbvSrvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	assert(SUCCEEDED(
		s_d3dDevice->CreateDescriptorHeap(
			&cbvSrvUavHeapDesc,
			IID_PPV_ARGS(s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetAddressOf()))
	));

	// RTV heap
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = 20;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	assert(SUCCEEDED(
		s_d3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetAddressOf()))
	));

	// Swap chain
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width = Demo::Settings::k_screenWidth;
	swapChainDesc.Height = Demo::Settings::k_screenHeight;
	swapChainDesc.Format = k_backBufferFormat;
	swapChainDesc.Scaling = DXGI_SCALING_NONE;
	swapChainDesc.SampleDesc.Quality = 0;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = k_backBufferCount;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
	assert(SUCCEEDED(
		s_dxgiFactory->CreateSwapChainForHwnd(
			s_graphicsQueue.Get(),
			windowHandle,
			&swapChainDesc,
			nullptr,
			nullptr,
			swapChain.GetAddressOf())
	));

	assert(SUCCEEDED(
		swapChain->QueryInterface(IID_PPV_ARGS(s_swapChain.GetAddressOf()))
	));

	// Back buffers
	for (auto bufferIdx = 0; bufferIdx < k_backBufferCount; bufferIdx++)
	{
		assert(SUCCEEDED(
			s_swapChain->GetBuffer(bufferIdx, IID_PPV_ARGS(s_backBuffers[bufferIdx].GetAddressOf()))
		));

		D3D12_CPU_DESCRIPTOR_HANDLE rtvDescriptor;
		rtvDescriptor.ptr = s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV]->GetCPUDescriptorHandleForHeapStart().ptr +
			bufferIdx * s_descriptorSize[D3D12_DESCRIPTOR_HEAP_TYPE_RTV];

		s_d3dDevice->CreateRenderTargetView(s_backBuffers[bufferIdx].Get(), nullptr, rtvDescriptor);
	}

	s_currentBufferIndex = s_swapChain->GetCurrentBackBufferIndex();

	return true;
}