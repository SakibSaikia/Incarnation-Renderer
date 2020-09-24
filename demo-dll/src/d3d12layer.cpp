#include <d3d12layer.h>
#include <settings.h>
#include <shadercompiler.h>
#include <ppltasks.h>
#include <concurrent_unordered_map.h>
#include <assert.h>
#include <spookyhash_api.h>
#include <string>
#include <sstream>
#include <fstream>
#include <array>
#include <list>
#include <unordered_map>
#include <system_error>
#include <utility>

// Constants
constexpr size_t k_bindlessSrvHeapSize = 1000;
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

bool operator==(const FILETIME& lhs, const FILETIME& rhs)
{
	return lhs.dwLowDateTime == rhs.dwLowDateTime &&
		lhs.dwHighDateTime == rhs.dwHighDateTime;
}

bool operator==(const FCommandList& lhs, const FCommandList& rhs)
{
	return lhs.m_cmdList.Get() == rhs.m_cmdList.Get();
}

bool operator==(const FResource& lhs, const FResource& rhs)
{
	return lhs.m_resource.Get() == rhs.m_resource.Get();
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
					cl.m_cmdList->Reset(cl.m_cmdAllocator.Get(), nullptr);
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

private:
	std::atomic_size_t m_fenceCounter{ 0 };
	std::mutex m_mutex;
	std::list<FCommandList> m_freeList;
	std::list<FCommandList> m_useList;
};

class FUploadBufferPool
{
public:
	FResource GetOrCreate(const std::wstring& name, const size_t sizeInBytes)
	{
		const std::lock_guard<std::mutex> lock(m_mutex);

		// Reuse buffer
		for (auto it = m_freeList.begin(); it != m_freeList.end(); ++it)
		{
			if (it->m_desc.Width >= sizeInBytes)
			{
				m_useList.push_back(*it);
				m_freeList.erase(it);

				FResource buffer = m_useList.back();
				buffer.m_resource->SetName(name.c_str());
				return buffer;
			}
		}

		// New buffer
		FResource newBuffer;

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
			IID_PPV_ARGS(newBuffer.m_resource.GetAddressOf())));

		newBuffer.m_resource->SetName(name.c_str());
		newBuffer.m_desc = resourceDesc;

		m_useList.push_back(newBuffer);
		return newBuffer;
	}

	void Retire(FResource buffer, FCommandList dependantCL)
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
				if (*it == buffer)
				{
					it->m_resource->Unmap(0, nullptr);
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

private:
	std::atomic_size_t m_fenceCounter{ 0 };
	std::mutex m_mutex;
	std::list<FResource> m_freeList;
	std::list<FResource> m_useList;
};

struct FTimestampedBlob
{
	FILETIME m_timestamp;
	Microsoft::WRL::ComPtr<IDxcBlob> m_blob;
};

class FResourceUploadContext
{
public:
	FResourceUploadContext() = delete;
	explicit FResourceUploadContext(const size_t uploadBufferSizeInBytes);

	std::pair<uint8_t*, size_t> Allocate(const size_t sizeInBytes);
	void CopyBuffer(D3DResource_t* destResource, const size_t srcOffset, const size_t size);
	D3DFence_t* SubmitUploads();

private:
	FResource m_uploadBuffer;
	FCommandList m_copyCommandlist;
	uint8_t* m_mappedPtr;
	size_t m_sizeInBytes;
	size_t m_currentOffset;
	size_t m_fenceValue;
	Microsoft::WRL::ComPtr<D3DFence_t> m_fence;
};


namespace Demo::D3D12
{
#if defined (_DEBUG)
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
	FUploadBufferPool s_uploadBufferPool;

	concurrency::concurrent_unordered_map<FShaderDesc, FTimestampedBlob> s_shaderCache;
	concurrency::concurrent_unordered_map<FRootsigDesc, FTimestampedBlob> s_rootsigCache;
	concurrency::concurrent_unordered_map<D3D12_GRAPHICS_PIPELINE_STATE_DESC, Microsoft::WRL::ComPtr<D3DPipelineState_t>> s_graphicsPSOPool;
	concurrency::concurrent_unordered_map<D3D12_COMPUTE_PIPELINE_STATE_DESC, Microsoft::WRL::ComPtr<D3DPipelineState_t>> s_computePSOPool;
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
					search->second.m_blob.GetAddressOf()))
			{
				search->second.m_timestamp = currentTimestamp;
				return search->second.m_blob.Get();
			}
			else
			{
				// Use pre-cached rootsig if it is cached and up-to-date or if the current changes fail to compile.
				// Update timestamp so that we don't retry compilation on failure every frame.
				search->second.m_timestamp = currentTimestamp;
				return search->second.m_blob.Get();
			}
		}
		else
		{
			FTimestampedBlob& rsBlob = s_rootsigCache[rootsigDesc];
			AssertIfFailed(Demo::ShaderCompiler::CompileRootsignature(
				rootsigDesc.m_filename,
				rootsigDesc.m_entrypoint,
				profile,
				rsBlob.m_blob.GetAddressOf()));

			rsBlob.m_timestamp = currentTimestamp;
			return rsBlob.m_blob.Get();
		}
	}

	IDxcBlob* CacheShader(const FShaderDesc& shaderDesc, const std::wstring& profile)
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
					search->second.m_blob.GetAddressOf()))
			{
				search->second.m_timestamp = currentTimestamp;
				return search->second.m_blob.Get();
			}
			else
			{
				// Use pre-cached shader if it is cached and up-to-date or if the current changes fail to compile.
				// Update timestamp so that we don't retry compilation on a failed shader every frame.
				search->second.m_timestamp = currentTimestamp;
				return search->second.m_blob.Get();
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
				shaderBlob.m_blob.GetAddressOf()));

			shaderBlob.m_timestamp = currentTimestamp;
			return shaderBlob.m_blob.Get();
		}
	}
}

bool Demo::D3D12::Initialize(HWND& windowHandle)
{
	UINT dxgiFactoryFlags = 0;

	// Debug layer
#if defined(_DEBUG)
	AssertIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(s_debugController.GetAddressOf())));
	s_debugController->EnableDebugLayer();
	dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

	// DXGI Factory
	AssertIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(s_dxgiFactory.GetAddressOf())));

	// Adapter
	Microsoft::WRL::ComPtr<DXGIAdapter_t> adapter = EnumerateAdapters(s_dxgiFactory.Get());

	// Device
	AssertIfFailed(
		D3D12CreateDevice(
			adapter.Get(),
			D3D_FEATURE_LEVEL_12_1,
			IID_PPV_ARGS(s_d3dDevice.GetAddressOf()))
	);

	// Cache descriptor sizes
	for (int typeId = 0; typeId < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++typeId)
	{
		s_descriptorSize[typeId] = s_d3dDevice->GetDescriptorHandleIncrementSize(static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(typeId));
	}

	// Command Queue(s)
	D3D12_COMMAND_QUEUE_DESC cmdQueueDesc = {};
	cmdQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	cmdQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	AssertIfFailed(s_d3dDevice->CreateCommandQueue(&cmdQueueDesc, IID_PPV_ARGS(s_graphicsQueue.GetAddressOf())));
	s_graphicsQueue->SetName(L"graphics_queue");

	cmdQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
	AssertIfFailed(s_d3dDevice->CreateCommandQueue(&cmdQueueDesc, IID_PPV_ARGS(s_computeQueue.GetAddressOf())));
	s_computeQueue->SetName(L"compute_queue");

	cmdQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
	AssertIfFailed(s_d3dDevice->CreateCommandQueue(&cmdQueueDesc, IID_PPV_ARGS(s_copyQueue.GetAddressOf())));
	s_copyQueue->SetName(L"copy_queue");

	// Bindless SRV heap
	D3D12_DESCRIPTOR_HEAP_DESC cbvSrvUavHeapDesc = {};
	cbvSrvUavHeapDesc.NumDescriptors = k_bindlessSrvHeapSize;
	cbvSrvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	cbvSrvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	AssertIfFailed(
		s_d3dDevice->CreateDescriptorHeap(
			&cbvSrvUavHeapDesc,
			IID_PPV_ARGS(s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].GetAddressOf()))
	);
	s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV]->SetName(L"bindless_descriptor_heap");

	// RTV heap
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = 20;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	AssertIfFailed(
		s_d3dDevice->CreateDescriptorHeap(
			&rtvHeapDesc, 
			IID_PPV_ARGS(s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].GetAddressOf()))
	);
	s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV]->SetName(L"rtv_heap");

	// Swap chain
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width = Demo::Settings::k_screenWidth;
	swapChainDesc.Height = Demo::Settings::k_screenHeight;
	swapChainDesc.Format = Demo::Settings::k_backBufferFormat;
	swapChainDesc.Scaling = DXGI_SCALING_NONE;
	swapChainDesc.SampleDesc.Quality = 0;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = k_backBufferCount;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
	AssertIfFailed(
		s_dxgiFactory->CreateSwapChainForHwnd(
			s_graphicsQueue.Get(),
			windowHandle,
			&swapChainDesc,
			nullptr,
			nullptr,
			swapChain.GetAddressOf())
	);

	AssertIfFailed(
		swapChain->QueryInterface(IID_PPV_ARGS(s_swapChain.GetAddressOf()))
	);

	// Back buffers
	for (size_t bufferIdx = 0; bufferIdx < k_backBufferCount; bufferIdx++)
	{
		AssertIfFailed(s_swapChain->GetBuffer(bufferIdx, IID_PPV_ARGS(s_backBuffers[bufferIdx].GetAddressOf())));

		std::wstringstream s;
		s << L"back_buffer_" << bufferIdx;
		s_backBuffers[bufferIdx]->SetName(s.str().c_str());

		D3D12_CPU_DESCRIPTOR_HANDLE rtvDescriptor;
		rtvDescriptor.ptr = s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV]->GetCPUDescriptorHandleForHeapStart().ptr +
			bufferIdx * s_descriptorSize[D3D12_DESCRIPTOR_HEAP_TYPE_RTV];

		s_d3dDevice->CreateRenderTargetView(s_backBuffers[bufferIdx].Get(), nullptr, rtvDescriptor);
	}

	s_currentBufferIndex = s_swapChain->GetCurrentBackBufferIndex();

	return true;
}

void Demo::D3D12::Teardown()
{
	Microsoft::WRL::ComPtr<D3DFence_t> flushFence;
	AssertIfFailed(s_d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(flushFence.GetAddressOf())));

	HANDLE flushEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);

	s_graphicsQueue->Signal(flushFence.Get(), 0xFF);
	flushFence->SetEventOnCompletion(0xFF, flushEvent);

	WaitForSingleObject(flushEvent, INFINITE);
}

D3DDevice_t* Demo::D3D12::GetDevice()
{
	return s_d3dDevice.Get();
}

FCommandList Demo::D3D12::FetchCommandlist(const D3D12_COMMAND_LIST_TYPE type)
{
	return s_commandListPool.GetOrCreate(type);
}

Microsoft::WRL::ComPtr<D3DRootSignature_t> Demo::D3D12::FetchGraphicsRootSignature(const FRootsigDesc& rootsig)
{
	IDxcBlob* rsBlob = CacheRootsignature(rootsig, L"rootsig_1_1");
	Microsoft::WRL::ComPtr<D3DRootSignature_t> rs;
	s_d3dDevice->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(rs.GetAddressOf()));
	return rs;
}

D3DPipelineState_t* Demo::D3D12::FetchGraphicsPipelineState(
	const FRootsigDesc& rootsig,
	const FShaderDesc& vs,
	const FShaderDesc& ps,
	const D3D12_PRIMITIVE_TOPOLOGY_TYPE primitiveTopology,
	const DXGI_FORMAT dsvFormat,
	const uint32_t numRenderTargets,
	const std::initializer_list<DXGI_FORMAT>& rtvFormats,
	const std::initializer_list<D3D12_COLOR_WRITE_ENABLE>& colorWriteMasks,
	const bool depthEnable,
	const D3D12_DEPTH_WRITE_MASK& depthWriteMask,
	const D3D12_COMPARISON_FUNC& depthFunc)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};

	// Shaders
	IDxcBlob* vsBlob = CacheShader(vs, L"vs_6_4");
	IDxcBlob* psBlob = CacheShader(ps, L"ps_6_4");
	desc.VS.pShaderBytecode = vsBlob->GetBufferPointer();
	desc.VS.BytecodeLength = vsBlob->GetBufferSize();
	desc.PS.pShaderBytecode = psBlob->GetBufferPointer();
	desc.PS.BytecodeLength = psBlob->GetBufferSize();

	// Primitive Topology
	desc.PrimitiveTopologyType = primitiveTopology;

	// Rasterizer State
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	desc.RasterizerState.FrontCounterClockwise = FALSE;
	desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
	desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
	desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.RasterizerState.MultisampleEnable = FALSE;
	desc.RasterizerState.AntialiasedLineEnable = FALSE;
	desc.RasterizerState.ForcedSampleCount = 0;
	desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	// Blend State
	desc.BlendState.AlphaToCoverageEnable = FALSE;
	desc.BlendState.IndependentBlendEnable = FALSE;

	assert(numRenderTargets == colorWriteMasks.size());
	int i = 0;
	for (auto& writeMask : colorWriteMasks)
	{
		desc.BlendState.RenderTarget[i].BlendEnable = FALSE;
		desc.BlendState.RenderTarget[i].LogicOpEnable = FALSE;
		desc.BlendState.RenderTarget[i].RenderTargetWriteMask = writeMask;

		i++;
	}

	// Depth Stencil State
	desc.DepthStencilState.DepthEnable = depthEnable;
	desc.DepthStencilState.DepthWriteMask = depthWriteMask;
	desc.DepthStencilState.DepthFunc = depthFunc;
	desc.DepthStencilState.StencilEnable = FALSE;
	desc.DSVFormat = dsvFormat;

	// Render Target(s) State
	assert(numRenderTargets == rtvFormats.size());
	desc.NumRenderTargets = numRenderTargets;
	i = 0;
	for (auto& format : rtvFormats)
	{
		desc.RTVFormats[i++] = format;
	}

	// Multi Sampling State
	desc.SampleMask = UINT_MAX;
	desc.SampleDesc.Count = 1;

	// Create or Reuse
	auto search = s_graphicsPSOPool.find(desc);
	if (search != s_graphicsPSOPool.cend())
	{
		return search->second.Get();
	}
	else
	{
		IDxcBlob* rsBlob = CacheRootsignature(rootsig, L"rootsig_1_1");
		Microsoft::WRL::ComPtr<D3DRootSignature_t> rs;
		s_d3dDevice->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(rs.GetAddressOf()));
		desc.pRootSignature = rs.Get();

		AssertIfFailed(s_d3dDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(s_graphicsPSOPool[desc].GetAddressOf())));
		return s_graphicsPSOPool[desc].Get();
	}
}

D3DPipelineState_t* Demo::D3D12::FetchComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC  desc)
{
	return {};
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
	return s_backBuffers[s_currentBufferIndex].Get();
}

D3DFence_t* Demo::D3D12::ExecuteCommandlists(const D3D12_COMMAND_LIST_TYPE commandQueueType, std::initializer_list<FCommandList> commandLists)
{
	std::vector<ID3D12CommandList*> d3dCommandLists;
	size_t latestFenceValue = 0;
	D3DFence_t* latestFence = {};

	// Accumulate CLs and keep tab of the latest fence
	for (const FCommandList& cl : commandLists)
	{
		D3DCommandList_t* d3dCL = cl.m_cmdList.Get();
		d3dCL->Close();
		d3dCommandLists.push_back(d3dCL);

		if (cl.m_fenceValue > latestFenceValue)
		{
			latestFenceValue = cl.m_fenceValue;
			latestFence = cl.m_fence.Get();
		}
	}

	// Figure out which command queue to use
	D3DCommandQueue_t* activeCommandQueue{};
	switch (commandQueueType)
	{
	case D3D12_COMMAND_LIST_TYPE_DIRECT:
		activeCommandQueue = s_graphicsQueue.Get();
		break;
	case D3D12_COMMAND_LIST_TYPE_COMPUTE:
		activeCommandQueue = s_computeQueue.Get();
		break;
	case D3D12_COMMAND_LIST_TYPE_COPY:
		activeCommandQueue = s_copyQueue.Get();
		break;
	}

	// Execute commands, signal the CL fences and retire the CLs
	activeCommandQueue->ExecuteCommandLists(d3dCommandLists.size(), d3dCommandLists.data());
	for (const FCommandList& cl : commandLists)
	{
		activeCommandQueue->Signal(cl.m_fence.Get(), cl.m_fenceValue);
		s_commandListPool.Retire(cl);
	}

	// Return the latest fence
	return latestFence;
}

void Demo::D3D12::PresentDisplay()
{
	s_swapChain->Present(1, 0);
	s_currentBufferIndex = (s_currentBufferIndex + 1) % k_backBufferCount;
}

FCommandList::FCommandList(const D3D12_COMMAND_LIST_TYPE type, const size_t  fenceValue) :
	m_type{ type },
	m_fenceValue{ fenceValue }
{
	
	AssertIfFailed(s_d3dDevice->CreateCommandAllocator(type, IID_PPV_ARGS(m_cmdAllocator.GetAddressOf())));

	AssertIfFailed(s_d3dDevice->CreateCommandList(
		0,
		type,
		m_cmdAllocator.Get(),
		nullptr,
		IID_PPV_ARGS(m_cmdList.GetAddressOf())));

	AssertIfFailed(s_d3dDevice->CreateFence(
		0,
		D3D12_FENCE_FLAG_NONE,
		IID_PPV_ARGS(m_fence.GetAddressOf())));
}

FResourceUploadContext::FResourceUploadContext(const size_t uploadBufferSizeInBytes) :
	m_currentOffset{ 0 }
{
	// Round up to power of 2
	DWORD n = _BitScanForward64(&n, uploadBufferSizeInBytes);
	m_sizeInBytes = (1 << n);

	m_copyCommandlist = FetchCommandlist(D3D12_COMMAND_LIST_TYPE_COPY);

	m_uploadBuffer = s_uploadBufferPool.GetOrCreate(L"upload_context_buffer", m_sizeInBytes);
	m_uploadBuffer.m_resource->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedPtr));
}

std::pair<uint8_t*, size_t> FResourceUploadContext::Allocate(const size_t sizeInBytes)
{
	size_t capacity = m_sizeInBytes - m_currentOffset;
	assert(sizeInBytes <= capacity && L"Upload buffer is too small!");

	uint8_t* pAlloc = m_mappedPtr + m_currentOffset;
	size_t offset = m_currentOffset;
	m_currentOffset += sizeInBytes;

	return std::make_pair(pAlloc, offset);
}

void FResourceUploadContext::CopyBuffer(D3DResource_t* destResource, const size_t srcOffset, const size_t size)
{
	m_copyCommandlist.m_cmdList->CopyBufferRegion(
		destResource,
		0,
		m_uploadBuffer.m_resource.Get(),
		srcOffset,
		size);
}

D3DFence_t* FResourceUploadContext::SubmitUploads()
{
	D3DFence_t* clFence = ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_COPY, { m_copyCommandlist });

	s_copyQueue->Signal(m_fence.Get(), m_fenceValue);
	s_uploadBufferPool.Retire(m_uploadBuffer, m_copyCommandlist);

	return clFence;
}

FResource Demo::D3D12::CreateUploadBuffer(
	const std::wstring& name,
	const size_t sizeInBytes,
	std::function<void(uint8_t*)> uploadFunc)
{
	DWORD n;
	_BitScanForward64(&n, sizeInBytes);
	const size_t powOf2Size = (1 << n);
	FResource buffer = s_uploadBufferPool.GetOrCreate(name, powOf2Size);

	uint8_t* pData;
	buffer.m_resource->Map(0, nullptr, reinterpret_cast<void**>(&pData));

	if (uploadFunc)
	{
		uploadFunc(pData);
	}

	return buffer;
}

FResource Demo::D3D12::CreateDefaultBuffer(
	const std::wstring& name,
	const size_t size,
	D3D12_RESOURCE_STATES state,
	FResourceUploadContext* uploadContext,
	std::function<void(uint8_t*)> uploadFunc)
{
	FResource buffer;

	bool requiresUpload = uploadContext && uploadFunc;

	// Create virtual resource
	buffer.m_desc = {};
	buffer.m_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer.m_desc.Width = size;
	buffer.m_desc.Height = 1;
	buffer.m_desc.DepthOrArraySize = 1;
	buffer.m_desc.MipLevels = 1;
	buffer.m_desc.Format = DXGI_FORMAT_UNKNOWN;
	buffer.m_desc.SampleDesc.Count = 1;
	buffer.m_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	AssertIfFailed(s_d3dDevice->CreateReservedResource(
		&buffer.m_desc,
		requiresUpload ? D3D12_RESOURCE_STATE_COMMON : state,
		nullptr,
		IID_PPV_ARGS(buffer.m_resource.GetAddressOf())));

	//// Allocate pages
	//uint32_t numPages;
	//s_d3dDevice->GetResourceTiling(buffer.m_resource.Get(), &numPages, nullptr, nullptr, nullptr, 0, nullptr);
	//buffer.m_physicalPages = s_resourceHeap.AllocatePages(numPages);

	//// Commit
	//UpdateTileMappings();

	//if (requiresUpload)
	//{
	//	auto&& [pDest, uploadBufferOffset] = uploadContext.Allocate(size);
	//	uploadFunc(pDest);
	//	uploadContext.CopyBuffer(buffer.m_resource.Get(), uploadBufferOffset, size);
	//}

	return buffer;
}