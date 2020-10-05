#include <d3d12layer.h>
#include <settings.h>
#include <shadercompiler.h>
#include <ppltasks.h>
#include <concurrent_unordered_map.h>
#include <concurrent_queue.h>
#include <assert.h>
#include <spookyhash_api.h>
#include <imgui.h>
#include <dxgidebug.h>
#include <string>
#include <sstream>
#include <fstream>
#include <array>
#include <list>
#include <unordered_map>
#include <system_error>
#include <utility>

// Constants
constexpr uint32_t k_backBufferCount = 2;

using namespace Demo::D3D12;

namespace
{
	inline void AssertIfFailed(HRESULT hr)
	{
	#if defined _DEBUG
		if (FAILED(hr))
		{
			std::string message = std::system_category().message(hr);
			OutputDebugStringA(message.c_str());
			_CrtDbgBreak();
		}
	#endif
	}

	winrt::com_ptr<DXGIAdapter_t> EnumerateAdapters(DXGIFactory_t* dxgiFactory)
	{
		winrt::com_ptr<DXGIAdapter_t> bestAdapter;
		size_t maxVram = 0;

		winrt::com_ptr<DXGIAdapter_t> adapter;
		uint32_t i = 0;

		while (dxgiFactory->EnumAdapters(i++, adapter.put()) != DXGI_ERROR_NOT_FOUND)
		{
			DXGI_ADAPTER_DESC desc;
			adapter->GetDesc(&desc);

			if (desc.DedicatedVideoMemory > maxVram)
			{
				maxVram = desc.DedicatedVideoMemory;

				bestAdapter = nullptr;
				bestAdapter = adapter;
			}

			adapter = nullptr;
		}

		DXGI_ADAPTER_DESC desc;
		bestAdapter->GetDesc(&desc);

		std::wstringstream out;
		out << L"*** Adapter : " << desc.Description << std::endl;
		OutputDebugString(out.str().c_str());

		return bestAdapter;
	}
}

template<>
struct std::hash<D3D12_GRAPHICS_PIPELINE_STATE_DESC>
{
	std::size_t operator()(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& key) const
	{
		uint64_t seed1{}, seed2{};
		spookyhash_context context;
		spookyhash_context_init(&context, seed1, seed2);
		spookyhash_update(&context, key.VS.pShaderBytecode, key.VS.BytecodeLength);
		spookyhash_update(&context, key.PS.pShaderBytecode, key.PS.BytecodeLength);
		spookyhash_update(&context, &key.StreamOutput, sizeof(key.StreamOutput));
		spookyhash_update(&context, &key.BlendState, sizeof(key.BlendState));
		spookyhash_update(&context, &key.SampleMask, sizeof(key.SampleMask));
		spookyhash_update(&context, &key.RasterizerState, sizeof(key.RasterizerState));
		spookyhash_update(&context, &key.DepthStencilState, sizeof(key.DepthStencilState));
		spookyhash_update(&context, key.InputLayout.pInputElementDescs, key.InputLayout.NumElements * sizeof(D3D12_INPUT_LAYOUT_DESC));
		spookyhash_update(&context, &key.IBStripCutValue, sizeof(key.IBStripCutValue));
		spookyhash_update(&context, &key.PrimitiveTopologyType, sizeof(key.PrimitiveTopologyType));
		spookyhash_update(&context, &key.NumRenderTargets, sizeof(key.NumRenderTargets));
		spookyhash_update(&context, key.RTVFormats, sizeof(key.RTVFormats));
		spookyhash_update(&context, &key.DSVFormat, sizeof(key.DSVFormat));
		spookyhash_update(&context, &key.SampleDesc, sizeof(key.SampleDesc));
		spookyhash_update(&context, &key.NodeMask, sizeof(key.NodeMask));
		spookyhash_update(&context, &key.Flags, sizeof(key.Flags));
		spookyhash_final(&context, &seed1, &seed2);

		return seed1 ^ (seed2 << 1);
	}
};

template<>
struct std::hash<D3D12_COMPUTE_PIPELINE_STATE_DESC >
{
	std::size_t operator()(const D3D12_COMPUTE_PIPELINE_STATE_DESC& key) const
	{
		uint64_t seed1{}, seed2{};
		spookyhash_context context;
		spookyhash_context_init(&context, seed1, seed2);
		spookyhash_update(&context, key.CS.pShaderBytecode, key.CS.BytecodeLength);
		spookyhash_update(&context, &key.NodeMask, sizeof(key.NodeMask));
		spookyhash_update(&context, &key.Flags, sizeof(key.Flags));
		spookyhash_final(&context, &seed1, &seed2);

		return seed1 ^ (seed2 << 1);
	}
};

template<>
struct std::hash<FShaderDesc>
{
	std::size_t operator()(const FShaderDesc& key) const
	{
		uint64_t seed1{}, seed2{};
		spookyhash_context context;
		spookyhash_context_init(&context, seed1, seed2);
		spookyhash_update(&context, key.m_filename.c_str(), key.m_filename.size());
		spookyhash_update(&context, key.m_entrypoint.c_str(), key.m_entrypoint.size());
		spookyhash_update(&context, key.m_defines.c_str(), key.m_defines.size());
		spookyhash_final(&context, &seed1, &seed2);

		return seed1 ^ (seed2 << 1);
	}
};

template<>
struct std::hash<FRootsigDesc>
{
	std::size_t operator()(const FRootsigDesc& key) const
	{
		uint64_t seed1{}, seed2{};
		spookyhash_context context;
		spookyhash_context_init(&context, seed1, seed2);
		spookyhash_update(&context, key.m_filename.c_str(), key.m_filename.size());
		spookyhash_update(&context, key.m_entrypoint.c_str(), key.m_entrypoint.size());
		spookyhash_final(&context, &seed1, &seed2);

		return seed1 ^ (seed2 << 1);
	}
};

bool operator==(const FShaderDesc& lhs, const FShaderDesc& rhs)
{
	return lhs.m_filename == rhs.m_filename &&
		lhs.m_entrypoint == rhs.m_entrypoint &&
		lhs.m_defines == rhs.m_defines;
}

bool operator==(const FRootsigDesc& lhs, const FRootsigDesc& rhs)
{
	return lhs.m_filename == rhs.m_filename &&
		lhs.m_entrypoint == rhs.m_entrypoint;
}

bool operator==(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& lhs, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& rhs)
{
	return std::hash<D3D12_GRAPHICS_PIPELINE_STATE_DESC>{}(lhs) == std::hash<D3D12_GRAPHICS_PIPELINE_STATE_DESC>{}(rhs);
}

bool operator==(const D3D12_COMPUTE_PIPELINE_STATE_DESC& lhs, const D3D12_COMPUTE_PIPELINE_STATE_DESC& rhs)
{
	return std::hash<D3D12_COMPUTE_PIPELINE_STATE_DESC>{}(lhs) == std::hash<D3D12_COMPUTE_PIPELINE_STATE_DESC>{}(rhs);
}

bool operator==(const FILETIME& lhs, const FILETIME& rhs)
{
	return lhs.dwLowDateTime == rhs.dwLowDateTime &&
		lhs.dwHighDateTime == rhs.dwHighDateTime;
}

bool operator==(const FCommandList& lhs, const FCommandList& rhs)
{
	return lhs.m_cmdList.get() == rhs.m_cmdList.get();
}

bool operator==(const FResource& lhs, const FResource& rhs)
{
	return lhs.m_resource.get() == rhs.m_resource.get();
}

class FCommandListPool
{
public:
	FCommandList GetOrCreate(const D3D12_COMMAND_LIST_TYPE type)
	{
		const std::lock_guard<std::mutex> lock(m_mutex);

		// Reuse CL
		for (auto it = m_freeList.begin(); it != m_freeList.end(); ++it)
		{
			if (it->m_type == type)
			{
				m_useList.push_back(*it);
				m_freeList.erase(it);
				
				FCommandList cl = m_useList.back();
				cl.m_fenceValue = ++m_fenceCounter;
				return cl;
			}
		}

		// New CL
		m_useList.emplace_back(type, ++m_fenceCounter);
		return m_useList.back();
	}

	void Retire(FCommandList cmdList)
	{
		auto waitForFenceTask = concurrency::create_task([cmdList, this]()
		{
			HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
			if (event)
			{
				assert(cmdList.m_fenceValue != 0);
				cmdList.m_fence->SetEventOnCompletion(cmdList.m_fenceValue, event);
				WaitForSingleObject(event, INFINITE);
			}
		});

		auto addToFreePool = [cmdList, this]()
		{
			const std::lock_guard<std::mutex> lock(m_mutex);

			for (auto it = m_useList.begin(); it != m_useList.end();)
			{
				if (*it == cmdList)
				{
					m_freeList.push_back(*it);
					it = m_useList.erase(it);

					FCommandList cl = m_freeList.back();
					cl.m_fenceValue = 0;
					cl.m_cmdAllocator->Reset();
					cl.m_cmdList->Reset(cl.m_cmdAllocator.get(), nullptr);
					break;
				}
				else
				{
					++it;
				}
			}
		};

		waitForFenceTask.then(addToFreePool);
	}

	void Clear()
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		assert(m_useList.empty() && "All CLs should be retired at this point");
		m_freeList.clear();
	}

private:
	std::atomic_size_t m_fenceCounter{ 0 };
	std::mutex m_mutex;
	std::list<FCommandList> m_freeList;
	std::list<FCommandList> m_useList;
};

class FUploadBufferPool
{
public:
	D3DResource_t* GetOrCreate(const std::wstring& name, const size_t sizeInBytes)
	{
		const std::lock_guard<std::mutex> lock(m_mutex);

		// Reuse buffer
		for (auto it = m_freeList.begin(); it != m_freeList.end(); ++it)
		{
			if ((*it)->GetDesc().Width >= sizeInBytes)
			{
				m_useList.push_back(*it);
				m_freeList.erase(it);

				D3DResource_t* buffer = m_useList.back().get();
				buffer->SetName(name.c_str());
				return buffer;
			}
		}

		// New buffer
		winrt::com_ptr<D3DResource_t> newBuffer;

		D3D12_HEAP_PROPERTIES heapDesc = {};
		heapDesc.Type = D3D12_HEAP_TYPE_UPLOAD;
		heapDesc.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC resourceDesc = {};
		resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		resourceDesc.Width = sizeInBytes;
		resourceDesc.Height = 1;
		resourceDesc.DepthOrArraySize = 1;
		resourceDesc.MipLevels = 1;
		resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
		resourceDesc.SampleDesc.Count = 1;
		resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		AssertIfFailed(GetDevice()->CreateCommittedResource(
			&heapDesc,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(newBuffer.put())));

		newBuffer->SetName(name.c_str());

		m_useList.push_back(newBuffer);
		return newBuffer.get();
	}

	void Retire(D3DResource_t* buffer, FCommandList dependantCL)
	{
		auto waitForFenceTask = concurrency::create_task([buffer, dependantCL, this]()
		{
			HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
			if (event)
			{
				assert(dependantCL.m_fenceValue != 0);
				dependantCL.m_fence->SetEventOnCompletion(dependantCL.m_fenceValue, event);
				WaitForSingleObject(event, INFINITE);
			}
		});

		auto addToFreePool = [buffer, this]()
		{
			const std::lock_guard<std::mutex> lock(m_mutex);

			for (auto it = m_useList.begin(); it != m_useList.end();)
			{
				if (it->get() == buffer)
				{
					m_freeList.push_back(*it);
					it = m_useList.erase(it);
					break;
				}
				else
				{
					++it;
				}
			}
		};

		waitForFenceTask.then(addToFreePool);
	}

	void Clear()
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		assert(m_useList.empty() && "All buffers should be retired at this point");
		m_freeList.clear();
	}

private:
	std::atomic_size_t m_fenceCounter{ 0 };
	std::mutex m_mutex;
	std::list<winrt::com_ptr<D3DResource_t>> m_freeList;
	std::list<winrt::com_ptr<D3DResource_t>> m_useList;
};

struct FTimestampedBlob
{
	FILETIME m_timestamp;
	winrt::com_ptr<IDxcBlob> m_blob;
};

namespace Demo::D3D12
{
#if defined (_DEBUG)
	winrt::com_ptr<D3DDebug_t> s_debugController;
#endif

	winrt::com_ptr<DXGIFactory_t> s_dxgiFactory;
	winrt::com_ptr<D3DDevice_t> s_d3dDevice;

	winrt::com_ptr<DXGISwapChain_t> s_swapChain;
	std::array<winrt::com_ptr<D3DResource_t>, k_backBufferCount> s_backBuffers;
	uint32_t s_currentBufferIndex;

	winrt::com_ptr<D3DFence_t> s_frameFence;
	std::array<uint64_t, k_backBufferCount> s_frameFenceValues;

	std::array<uint32_t, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> s_descriptorSize;
	std::array< winrt::com_ptr<D3DDescriptorHeap_t>, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> s_descriptorHeaps;

	winrt::com_ptr<D3DCommandQueue_t> s_graphicsQueue;
	winrt::com_ptr<D3DCommandQueue_t> s_computeQueue;
	winrt::com_ptr<D3DCommandQueue_t> s_copyQueue;

	FCommandListPool s_commandListPool;
	FUploadBufferPool s_uploadBufferPool;

	concurrency::concurrent_unordered_map<FShaderDesc, FTimestampedBlob> s_shaderCache;
	concurrency::concurrent_unordered_map<FRootsigDesc, FTimestampedBlob> s_rootsigCache;
	concurrency::concurrent_unordered_map<std::wstring, FBindlessShaderResource> s_textureCache;
	concurrency::concurrent_unordered_map<D3D12_GRAPHICS_PIPELINE_STATE_DESC, winrt::com_ptr<D3DPipelineState_t>> s_graphicsPSOPool;
	concurrency::concurrent_unordered_map<D3D12_COMPUTE_PIPELINE_STATE_DESC, winrt::com_ptr<D3DPipelineState_t>> s_computePSOPool;
	concurrency::concurrent_queue<uint32_t> s_bindlessIndexPool;
}

namespace
{
	IDxcBlob* CacheRootsignature(const FRootsigDesc& rootsigDesc, const std::wstring& profile)
	{
		FILETIME currentTimestamp = Demo::ShaderCompiler::GetLastModifiedTime(rootsigDesc.m_filename);

		auto search = s_rootsigCache.find(rootsigDesc);
		if (search != s_rootsigCache.cend())
		{
			if (search->second.m_timestamp != currentTimestamp &&
				Demo::ShaderCompiler::CompileRootsignature(
					rootsigDesc.m_filename,
					rootsigDesc.m_entrypoint,
					profile,
					search->second.m_blob.put()))
			{
				search->second.m_timestamp = currentTimestamp;
				return search->second.m_blob.get();
			}
			else
			{
				// Use pre-cached rootsig if it is cached and up-to-date or if the current changes fail to compile.
				// Update timestamp so that we don't retry compilation on failure every frame.
				search->second.m_timestamp = currentTimestamp;
				return search->second.m_blob.get();
			}
		}
		else
		{
			FTimestampedBlob& rsBlob = s_rootsigCache[rootsigDesc];
			AssertIfFailed(Demo::ShaderCompiler::CompileRootsignature(
				rootsigDesc.m_filename,
				rootsigDesc.m_entrypoint,
				profile,
				rsBlob.m_blob.put()));

			rsBlob.m_timestamp = currentTimestamp;
			return rsBlob.m_blob.get();
		}
	}
}

bool Demo::D3D12::Initialize(const HWND& windowHandle, const uint32_t resX, const uint32_t resY)
{
	UINT dxgiFactoryFlags = 0;

	// Debug layer
#if defined(_DEBUG)
	AssertIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(s_debugController.put())));
	s_debugController->EnableDebugLayer();
	dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

	// DXGI Factory
	AssertIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(s_dxgiFactory.put())));

	// Adapter
	winrt::com_ptr<DXGIAdapter_t> adapter = EnumerateAdapters(s_dxgiFactory.get());

	// Device
	AssertIfFailed(D3D12CreateDevice(
		adapter.get(),
		D3D_FEATURE_LEVEL_12_1,
		IID_PPV_ARGS(s_d3dDevice.put())));

	// Cache descriptor sizes
	for (int typeId = 0; typeId < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++typeId)
	{
		s_descriptorSize[typeId] = s_d3dDevice->GetDescriptorHandleIncrementSize(static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(typeId));
	}

	// Command Queue(s)
	D3D12_COMMAND_QUEUE_DESC cmdQueueDesc = {};
	cmdQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	cmdQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	AssertIfFailed(s_d3dDevice->CreateCommandQueue(&cmdQueueDesc, IID_PPV_ARGS(s_graphicsQueue.put())));
	s_graphicsQueue->SetName(L"graphics_queue");

	cmdQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
	AssertIfFailed(s_d3dDevice->CreateCommandQueue(&cmdQueueDesc, IID_PPV_ARGS(s_computeQueue.put())));
	s_computeQueue->SetName(L"compute_queue");

	cmdQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
	AssertIfFailed(s_d3dDevice->CreateCommandQueue(&cmdQueueDesc, IID_PPV_ARGS(s_copyQueue.put())));
	s_copyQueue->SetName(L"copy_queue");

	// Bindless SRV heap
	{
		D3D12_DESCRIPTOR_HEAP_DESC cbvSrvUavHeapDesc = {};
		cbvSrvUavHeapDesc.NumDescriptors = k_bindlessSrvHeapSize;
		cbvSrvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		cbvSrvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		AssertIfFailed(
			s_d3dDevice->CreateDescriptorHeap(
				&cbvSrvUavHeapDesc,
				IID_PPV_ARGS(s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].put()))
		);
		s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV]->SetName(L"bindless_descriptor_heap");

		// Initialize with null descriptors
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		for (int i = 0; i < k_bindlessSrvHeapSize; ++i)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE srv;
			srv.ptr = s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV]->GetCPUDescriptorHandleForHeapStart().ptr + 
				i * s_descriptorSize[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV];
			GetDevice()->CreateShaderResourceView(nullptr, &srvDesc, srv);
		}

		// Cache available indices
		for (int i = 0; i < k_bindlessSrvHeapSize; ++i)
		{
			s_bindlessIndexPool.push(i);
		}
	}

	// RTV heap
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = 20;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	AssertIfFailed(
		s_d3dDevice->CreateDescriptorHeap(
			&rtvHeapDesc, 
			IID_PPV_ARGS(s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].put()))
	);
	s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV]->SetName(L"rtv_heap");

	// Swap chain
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width = resX;
	swapChainDesc.Height = resY;
	swapChainDesc.Format = Demo::Settings::k_backBufferFormat;
	swapChainDesc.Scaling = DXGI_SCALING_NONE;
	swapChainDesc.SampleDesc.Quality = 0;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = k_backBufferCount;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	winrt::com_ptr<IDXGISwapChain1> swapChain;
	AssertIfFailed(
		s_dxgiFactory->CreateSwapChainForHwnd(
			s_graphicsQueue.get(),
			windowHandle,
			&swapChainDesc,
			nullptr,
			nullptr,
			swapChain.put())
	);

	AssertIfFailed(
		swapChain->QueryInterface(IID_PPV_ARGS(s_swapChain.put()))
	);

	// Back buffers
	for (size_t bufferIdx = 0; bufferIdx < k_backBufferCount; bufferIdx++)
	{
		AssertIfFailed(s_swapChain->GetBuffer(bufferIdx, IID_PPV_ARGS(s_backBuffers[bufferIdx].put())));

		std::wstringstream s;
		s << L"back_buffer_" << bufferIdx;
		s_backBuffers[bufferIdx]->SetName(s.str().c_str());

		D3D12_CPU_DESCRIPTOR_HANDLE rtvDescriptor;
		rtvDescriptor.ptr = s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV]->GetCPUDescriptorHandleForHeapStart().ptr +
			bufferIdx * s_descriptorSize[D3D12_DESCRIPTOR_HEAP_TYPE_RTV];

		s_d3dDevice->CreateRenderTargetView(s_backBuffers[bufferIdx].get(), nullptr, rtvDescriptor);
	}

	s_currentBufferIndex = s_swapChain->GetCurrentBackBufferIndex();

	// Frame sync
	AssertIfFailed(s_d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(s_frameFence.put())));
	s_frameFenceValues = { 0, 0 };

	return true;
}

void Demo::D3D12::Teardown()
{
	winrt::com_ptr<D3DFence_t> flushFence;
	AssertIfFailed(s_d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(flushFence.put())));

	HANDLE flushEvents[3];
	flushEvents[0] = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
	flushEvents[1] = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
	flushEvents[2] = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);

	s_copyQueue->Signal(flushFence.get(), 0xFD);
	flushFence->SetEventOnCompletion(0xFD, flushEvents[0]);
	s_computeQueue->Signal(flushFence.get(), 0xFE);
	flushFence->SetEventOnCompletion(0xFE, flushEvents[1]);
	s_graphicsQueue->Signal(flushFence.get(), 0xFF);
	flushFence->SetEventOnCompletion(0xFF, flushEvents[2]);
	WaitForMultipleObjects(3, flushEvents, TRUE, INFINITE);

	s_commandListPool.Clear();
	s_uploadBufferPool.Clear();

	s_shaderCache.clear();
	s_rootsigCache.clear();
	s_textureCache.clear();
	s_graphicsPSOPool.clear();
	s_computePSOPool.clear();
	s_bindlessIndexPool.clear();

	s_frameFence.get()->Release();

	for (auto& descriptorHeap : s_descriptorHeaps)
	{
		if (descriptorHeap)
		{
			descriptorHeap.get()->Release();
		}
	}

	for (auto& backBuffer : s_backBuffers)
	{
		backBuffer.get()->Release();
	}

	s_swapChain.get()->Release();
	s_dxgiFactory.get()->Release();
	s_d3dDevice.get()->Release();

#if _DEBUG
	HMODULE dxgiDebugDll = GetModuleHandle(L"Dxgidebug.dll");
	auto DXGIGetDebugInterfaceProc = reinterpret_cast<decltype(DXGIGetDebugInterface)*>(GetProcAddress(dxgiDebugDll, "DXGIGetDebugInterface"));

	winrt::com_ptr<IDXGIDebug> dxgiDebug;
	DXGIGetDebugInterfaceProc(IID_PPV_ARGS(dxgiDebug.put()));
	AssertIfFailed(dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL));
#endif
}

D3DDevice_t* Demo::D3D12::GetDevice()
{
	return s_d3dDevice.get();
}

FCommandList Demo::D3D12::FetchCommandlist(const D3D12_COMMAND_LIST_TYPE type)
{
	return s_commandListPool.GetOrCreate(type);
}

IDxcBlob* Demo::D3D12::CacheShader(const FShaderDesc& shaderDesc, const std::wstring& profile)
{
	FILETIME currentTimestamp = Demo::ShaderCompiler::GetLastModifiedTime(shaderDesc.m_filename);

	auto search = s_shaderCache.find(shaderDesc);
	if (search != s_shaderCache.cend())
	{
		if (search->second.m_timestamp != currentTimestamp &&
			Demo::ShaderCompiler::CompileShader(
				shaderDesc.m_filename,
				shaderDesc.m_entrypoint,
				shaderDesc.m_defines,
				profile,
				search->second.m_blob.put()))
		{
			search->second.m_timestamp = currentTimestamp;
			return search->second.m_blob.get();
		}
		else
		{
			// Use pre-cached shader if it is cached and up-to-date or if the current changes fail to compile.
			// Update timestamp so that we don't retry compilation on a failed shader every frame.
			search->second.m_timestamp = currentTimestamp;
			return search->second.m_blob.get();
		}
	}
	else
	{
		FTimestampedBlob& shaderBlob = s_shaderCache[shaderDesc];
		AssertIfFailed(Demo::ShaderCompiler::CompileShader(
			shaderDesc.m_filename,
			shaderDesc.m_entrypoint,
			shaderDesc.m_defines,
			profile,
			shaderBlob.m_blob.put()));

		shaderBlob.m_timestamp = currentTimestamp;
		return shaderBlob.m_blob.get();
	}
}

winrt::com_ptr<D3DRootSignature_t> Demo::D3D12::FetchGraphicsRootSignature(const FRootsigDesc& rootsig)
{
	IDxcBlob* rsBlob = CacheRootsignature(rootsig, L"rootsig_1_1");
	winrt::com_ptr<D3DRootSignature_t> rs;
	s_d3dDevice->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(rs.put()));
	return rs;
}

D3DPipelineState_t* Demo::D3D12::FetchGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
	auto search = s_graphicsPSOPool.find(desc);
	if (search != s_graphicsPSOPool.cend())
	{
		return search->second.get();
	}
	else
	{
		AssertIfFailed(s_d3dDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(s_graphicsPSOPool[desc].put())));
		return s_graphicsPSOPool[desc].get();
	}
}

D3DPipelineState_t* Demo::D3D12::FetchComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc)
{
	auto search = s_computePSOPool.find(desc);
	if (search != s_computePSOPool.cend())
	{
		return search->second.get();
	}
	else
	{
		AssertIfFailed(s_d3dDevice->CreateComputePipelineState(&desc, IID_PPV_ARGS(s_computePSOPool[desc].put())));
		return s_computePSOPool[desc].get();
	}
}

D3D12_CPU_DESCRIPTOR_HANDLE Demo::D3D12::GetBackBufferDescriptor()
{
	D3D12_CPU_DESCRIPTOR_HANDLE descriptor;
	descriptor.ptr = s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV]->GetCPUDescriptorHandleForHeapStart().ptr +
		s_currentBufferIndex * s_descriptorSize[D3D12_DESCRIPTOR_HEAP_TYPE_RTV];

	return descriptor;
}

D3DResource_t* Demo::D3D12::GetBackBufferResource()
{
	return s_backBuffers[s_currentBufferIndex].get();
}

D3DFence_t* Demo::D3D12::ExecuteCommandlists(const D3D12_COMMAND_LIST_TYPE commandQueueType, std::initializer_list<FCommandList> commandLists)
{
	std::vector<ID3D12CommandList*> d3dCommandLists;
	size_t latestFenceValue = 0;
	D3DFence_t* latestFence = {};

	// Accumulate CLs and keep tab of the latest fence
	for (const FCommandList& cl : commandLists)
	{
		D3DCommandList_t* d3dCL = cl.m_cmdList.get();
		d3dCL->Close();
		d3dCommandLists.push_back(d3dCL);

		if (cl.m_fenceValue > latestFenceValue)
		{
			latestFenceValue = cl.m_fenceValue;
			latestFence = cl.m_fence.get();
		}
	}

	// Figure out which command queue to use
	D3DCommandQueue_t* activeCommandQueue{};
	switch (commandQueueType)
	{
	case D3D12_COMMAND_LIST_TYPE_DIRECT:
		activeCommandQueue = s_graphicsQueue.get();
		break;
	case D3D12_COMMAND_LIST_TYPE_COMPUTE:
		activeCommandQueue = s_computeQueue.get();
		break;
	case D3D12_COMMAND_LIST_TYPE_COPY:
		activeCommandQueue = s_copyQueue.get();
		break;
	}

	// Execute commands, signal the CL fences and retire the CLs
	activeCommandQueue->ExecuteCommandLists(d3dCommandLists.size(), d3dCommandLists.data());
	for (const FCommandList& cl : commandLists)
	{
		activeCommandQueue->Signal(cl.m_fence.get(), cl.m_fenceValue);
		s_commandListPool.Retire(cl);
	}

	// Return the latest fence
	return latestFence;
}

void Demo::D3D12::PresentDisplay()
{
	s_swapChain->Present(1, 0);

	// Signal current frame is done
	auto currentFenceValue = s_frameFenceValues[s_currentBufferIndex];
	s_graphicsQueue->Signal(s_frameFence.get(), currentFenceValue);

	// Cycle to next buffer index
	s_currentBufferIndex = (s_currentBufferIndex + 1) % k_backBufferCount;

	// If the buffer that was swapped in hasn't finished rendering on the GPU (from a previous submit), then wait!
	if (s_frameFence->GetCompletedValue() < s_frameFenceValues[s_currentBufferIndex])
	{
		PIXBeginEvent(0, L"wait_on_previous_frame");
		HANDLE frameWaitEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
		s_frameFence->SetEventOnCompletion(s_frameFenceValues[s_currentBufferIndex], frameWaitEvent);
		WaitForSingleObjectEx(frameWaitEvent, INFINITE, FALSE);
		PIXEndEvent();
	}

	// Update fence value for the next frame
	s_frameFenceValues[s_currentBufferIndex] = currentFenceValue + 1;
}

FCommandList::FCommandList(const D3D12_COMMAND_LIST_TYPE type, const size_t  fenceValue) :
	m_type{ type },
	m_fenceValue{ fenceValue }
{
	
	AssertIfFailed(s_d3dDevice->CreateCommandAllocator(type, IID_PPV_ARGS(m_cmdAllocator.put())));

	AssertIfFailed(s_d3dDevice->CreateCommandList(
		0,
		type,
		m_cmdAllocator.get(),
		nullptr,
		IID_PPV_ARGS(m_cmdList.put())));

	AssertIfFailed(s_d3dDevice->CreateFence(
		0,
		D3D12_FENCE_FLAG_NONE,
		IID_PPV_ARGS(m_fence.put())));
}

FResourceUploadContext::FResourceUploadContext(const size_t uploadBufferSizeInBytes) :
	m_currentOffset{ 0 }
{
	assert(uploadBufferSizeInBytes != 0);

	// Round up to power of 2
	unsigned long n;
	_BitScanReverse64(&n, uploadBufferSizeInBytes);
	m_sizeInBytes = (1 << (n + 1));

	m_copyCommandlist = FetchCommandlist(D3D12_COMMAND_LIST_TYPE_COPY);

	m_uploadBuffer = s_uploadBufferPool.GetOrCreate(L"upload_context_buffer", m_sizeInBytes);
	m_uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedPtr));
}

void FResourceUploadContext::UpdateSubresources(
	D3DResource_t* destinationResource, 
	const uint32_t firstSubresource,
	const uint32_t numSubresources,
	D3D12_SUBRESOURCE_DATA* srcData,
	std::function<void(FCommandList*)> transition)
{
	// NOTE layout.Footprint.RowPitch is the D3D12 aligned pitch whereas rowSizeInBytes is the unaligned pitch
	UINT64 totalBytes = 0;
	auto layouts = std::make_unique<D3D12_PLACED_SUBRESOURCE_FOOTPRINT[]>(numSubresources);
	auto rowSizeInBytes = std::make_unique<UINT64[]>(numSubresources);
	auto numRows = std::make_unique<UINT[]>(numSubresources);

	D3D12_RESOURCE_DESC destinationDesc = destinationResource->GetDesc();
	GetDevice()->GetCopyableFootprints(&destinationDesc, firstSubresource, numSubresources, m_currentOffset, layouts.get(), numRows.get(), rowSizeInBytes.get(), &totalBytes);

	size_t capacity = m_sizeInBytes - m_currentOffset;
	assert(totalBytes <= capacity && L"Upload buffer is too small!");

	uint8_t* pAlloc = m_mappedPtr + m_currentOffset;

	for (UINT i = 0; i < numSubresources; ++i)
	{
		D3D12_SUBRESOURCE_DATA src;
		src.pData = srcData[i].pData;
		src.RowPitch = srcData[i].RowPitch;
		src.SlicePitch = srcData[i].SlicePitch;

		D3D12_MEMCPY_DEST dest;
		dest.pData = pAlloc + layouts[i].Offset;
		dest.RowPitch = layouts[i].Footprint.RowPitch;
		dest.SlicePitch = layouts[i].Footprint.RowPitch * numRows[i];

		UINT numSlices = layouts[i].Footprint.Depth;

		for (UINT z = 0; z < numSlices; ++z)
		{
			BYTE* pDestSlice = reinterpret_cast<BYTE*>(dest.pData) + dest.SlicePitch * z;
			const BYTE* pSrcSlice = reinterpret_cast<const BYTE*>(src.pData) + src.SlicePitch * z;

			for (UINT y = 0; y < numRows[i]; ++y)
			{
				memcpy(pDestSlice + dest.RowPitch * y,
					pSrcSlice + src.RowPitch * y,
					rowSizeInBytes[i]);
			}
		}
	}

	if (destinationDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
	{
		m_copyCommandlist.m_cmdList->CopyBufferRegion(
			destinationResource, 
			0, 
			m_uploadBuffer, 
			layouts[0].Offset, 
			layouts[0].Footprint.Width);
	}
	else
	{
		for (UINT i = 0; i < numSubresources; ++i)
		{
			D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
			srcLocation.pResource = m_uploadBuffer;
			srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			srcLocation.PlacedFootprint = layouts[i];

			D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
			dstLocation.pResource = destinationResource;
			dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dstLocation.SubresourceIndex = i + firstSubresource;

			m_copyCommandlist.m_cmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
		}
	}

	m_currentOffset += totalBytes;
	m_pendingTransitions.push_back(transition);
}

D3DFence_t* FResourceUploadContext::SubmitUploads(FCommandList* owningCL)
{
	D3DFence_t* clFence = ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_COPY, { m_copyCommandlist });

	s_uploadBufferPool.Retire(m_uploadBuffer, m_copyCommandlist);

	// Transition all the destination resources on the owning/direct CL since the copy CLs have limited transition capabilities
	for (auto& transitionCallback : m_pendingTransitions)
	{
		transitionCallback(owningCL);
	}

	return clFence;
}

D3DDescriptorHeap_t* Demo::D3D12::GetBindlessShaderResourceHeap()
{
	return s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].get();
}

D3D12_GPU_DESCRIPTOR_HANDLE Demo::D3D12::GetBindlessShaderResourceHeapHandle()
{
	return s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV]->GetGPUDescriptorHandleForHeapStart();
}

FTransientBuffer Demo::D3D12::CreateTransientBuffer(
	const std::wstring& name,
	const size_t sizeInBytes,
	const FCommandList dependentCL,
	std::function<void(uint8_t*)> uploadFunc)
{
	assert(sizeInBytes != 0);

	DWORD n;
	_BitScanReverse64(&n, sizeInBytes);
	const size_t powOf2Size = (1 << (n + 1));
	D3DResource_t* buffer = s_uploadBufferPool.GetOrCreate(name, powOf2Size);

	uint8_t* pData;
	buffer->Map(0, nullptr, reinterpret_cast<void**>(&pData));

	if (uploadFunc)
	{
		uploadFunc(pData);
		buffer->Unmap(0, nullptr);
	}

	FTransientBuffer tempBuffer;
	tempBuffer.m_resource.copy_from(buffer);
	tempBuffer.m_dependentCmdlist = dependentCL;

	return tempBuffer;
}

FResource Demo::D3D12::CreateTemporaryDefaultBuffer(
	const std::wstring& name,
	const size_t size,
	D3D12_RESOURCE_STATES state)
{
	FResource buffer;

	// Create virtual resource
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	AssertIfFailed(s_d3dDevice->CreateReservedResource(
		&desc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(buffer.m_resource.put())));

	//// Allocate pages
	//uint32_t numPages;
	//s_d3dDevice->GetResourceTiling(buffer.m_resource.get(), &numPages, nullptr, nullptr, nullptr, 0, nullptr);
	//buffer.m_physicalPages = s_resourceHeap.AllocatePages(numPages);

	//// Commit
	//UpdateTileMappings();

	return buffer;
}

uint32_t Demo::D3D12::CacheTexture(const std::wstring& name, FResourceUploadContext* uploadContext)
{
	auto search = s_textureCache.find(name);
	if (search != s_textureCache.cend())
	{
		return search->second.m_bindlessDescriptorIndex;
	}
	else
	{
		FBindlessShaderResource& newTexture = s_textureCache[name];

		uint8_t* pixels;
		int width, height, bpp;
		uint16_t mipLevels;
		DXGI_FORMAT format;

		if (name == L"imgui_fonts")
		{
			ImGuiIO& io = ImGui::GetIO();
			io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height, &bpp);
			format = DXGI_FORMAT_R8G8B8A8_UNORM;
			mipLevels = 1;
		}

		// Create resource
		{
			D3D12_HEAP_PROPERTIES props = {};
			props.Type = D3D12_HEAP_TYPE_DEFAULT;
			props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Alignment = 0;
			desc.Width = width;
			desc.Height = height;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = mipLevels;
			desc.Format = format;
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			desc.Flags = D3D12_RESOURCE_FLAG_NONE;

			GetDevice()->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(newTexture.m_resource.put()));
		}

		// Upload texture data
		{
			D3D12_SUBRESOURCE_DATA srcData;
			srcData.pData = pixels;
			srcData.RowPitch = width * bpp;
			srcData.SlicePitch = height * srcData.RowPitch;
			uploadContext->UpdateSubresources(
				newTexture.m_resource.get(), 
				0, 
				1, 
				&srcData,
				[resource = newTexture.m_resource.get()](FCommandList* cmdList)
				{
					D3D12_RESOURCE_BARRIER barrier = {};
					barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
					barrier.Transition.pResource = resource;
					barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
					barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
					barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
					cmdList->m_cmdList->ResourceBarrier(1, &barrier);
				});
		}

		// Descriptor
		{
			bool ok = s_bindlessIndexPool.try_pop(newTexture.m_bindlessDescriptorIndex);
			assert(ok && "Ran out of bindless descriptors");
			D3D12_CPU_DESCRIPTOR_HANDLE srv;
			srv.ptr = s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV]->GetCPUDescriptorHandleForHeapStart().ptr +
				newTexture.m_bindlessDescriptorIndex * s_descriptorSize[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV];

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = format;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = mipLevels;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			GetDevice()->CreateShaderResourceView(newTexture.m_resource.get(), &srvDesc, srv);
		}

		return newTexture.m_bindlessDescriptorIndex;
	}
}

FTransientBuffer::~FTransientBuffer()
{
	s_uploadBufferPool.Retire(m_resource.get(), m_dependentCmdlist);
}