#include <d3d12layer.h>
#include <settings.h>
#include <ppltasks.h>
#include <cassert>
#include <string>
#include <sstream>
#include <array>
#include <list>

// Constants
constexpr size_t k_bindlessSrvHeapSize = 1000;
constexpr size_t k_backBufferCount = 2;
constexpr DXGI_FORMAT k_backBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

using namespace Demo::D3D12;

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

class FCommandListPool
{
public:
	FCommandList* GetOrCreate(const D3D12_COMMAND_LIST_TYPE type)
	{
		// Reuse CL
		for (auto it = m_freeList.begin(); it != m_freeList.end(); ++it)
		{
			if ((*it)->m_type == type)
			{
				m_useList.push_back(std::move(*it));
				m_freeList.erase(it);
				
				FCommandList* cl = m_useList.back().get();
				cl->m_fenceValue = ++m_fenceCounter;
				return cl;
			}
		}

		// New CL
		m_useList.emplace_back(std::make_unique<FCommandList>(type, ++m_fenceCounter));
		return m_useList.back().get();
	}

	void Retire(FCommandList* cmdList)
	{
		auto waitForFenceTask = concurrency::create_task([cmdList, this]()
		{
			HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
			if (event)
			{
				assert(cmdList->m_fenceValue != 0);
				cmdList->m_fence->SetEventOnCompletion(cmdList->m_fenceValue, event);
				WaitForSingleObject(event, INFINITE);
			}
		});

		auto addToFreePool = [cmdList, this]()
		{
			for (auto it = m_useList.begin(); it != m_useList.end(); ++it)
			{
				if (it->get() == cmdList)
				{
					m_freeList.push_back(std::move(*it));
					m_useList.erase(it);

					FCommandList* cl = m_freeList.back().get();
					cl->m_fenceValue = 0;
					cl->m_cmdAllocator.Reset();
					break;
				}
			}
		};

		waitForFenceTask.then(addToFreePool);
	}

private:
	std::atomic_size_t m_fenceCounter{ 0 };
	std::list<std::unique_ptr<FCommandList>> m_freeList;
	std::list<std::unique_ptr<FCommandList>> m_useList;
};


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

		FCommandListPool s_commandListPool;
	}
} 


bool Demo::D3D12::Initialize(HWND& windowHandle)
{
	UINT dxgiFactoryFlags = 0;

	// Debug layer
#if defined(DEBUG) || defined(_DEBUG)
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(s_debugController.GetAddressOf()))))
	{
		s_debugController->EnableDebugLayer();
		dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
	}
#endif

	// DXGI Factory
	assert(SUCCEEDED(
		CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(s_dxgiFactory.GetAddressOf()))
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
	for (size_t bufferIdx = 0; bufferIdx < k_backBufferCount; bufferIdx++)
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

D3DDevice_t* Demo::D3D12::GetDevice()
{
	return s_d3dDevice.Get();
}

FCommandList* Demo::D3D12::AcquireCommandlist(const D3D12_COMMAND_LIST_TYPE type)
{
	return s_commandListPool.GetOrCreate(type);
}

void Demo::D3D12::ReleaseCommandList(FCommandList* cmdList)
{
	s_commandListPool.Retire(cmdList);
}

D3D12_CPU_DESCRIPTOR_HANDLE Demo::D3D12::GetBackBuffer()
{
	D3D12_CPU_DESCRIPTOR_HANDLE descriptor;
	descriptor.ptr = s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV]->GetCPUDescriptorHandleForHeapStart().ptr +
		s_currentBufferIndex * s_descriptorSize[D3D12_DESCRIPTOR_HEAP_TYPE_RTV];

	return descriptor;
}

FCommandList::FCommandList(const D3D12_COMMAND_LIST_TYPE type, const size_t  fenceValue) :
	m_type{ type },
	m_fenceValue{ fenceValue }
{
	assert(SUCCEEDED(
		GetDevice()->CreateCommandAllocator(
			type,
			IID_PPV_ARGS(m_cmdAllocator.GetAddressOf()))
	));

	assert(SUCCEEDED(
		GetDevice()->CreateCommandList(
			0,
			type,
			m_cmdAllocator.Get(),
			nullptr,
			IID_PPV_ARGS(m_cmdList.GetAddressOf()))
	));
}