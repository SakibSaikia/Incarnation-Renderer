#include <d3d12layer.h>
#include <settings.h>
#include <shadercompiler.h>
#include <ppltasks.h>
#include <assert.h>
#include <spookyhash_api.h>
#include <string>
#include <sstream>
#include <fstream>
#include <array>
#include <list>
#include <unordered_map>
#include <system_error>

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
struct std::hash<FGraphicsPipelineDesc>
{
	std::size_t operator()(const FGraphicsPipelineDesc& key) const
	{
		uint64_t seed1{}, seed2{};
		spookyhash_context context;
		spookyhash_context_init(&context, seed1, seed2);
		spookyhash_update(&context, key.m_rootsig.m_filename.data(), key.m_rootsig.m_filename.size());
		spookyhash_update(&context, key.m_rootsig.m_entrypoint.data(), key.m_rootsig.m_entrypoint.size());
		spookyhash_update(&context, key.m_vs.m_filename.data(), key.m_vs.m_filename.size());
		spookyhash_update(&context, key.m_vs.m_entrypoint.data(), key.m_vs.m_entrypoint.size());
		spookyhash_update(&context, key.m_vs.m_defines.data(), key.m_vs.m_defines.size());
		spookyhash_update(&context, key.m_ps.m_filename.data(), key.m_ps.m_filename.size());
		spookyhash_update(&context, key.m_ps.m_entrypoint.data(), key.m_ps.m_entrypoint.size());
		spookyhash_update(&context, key.m_ps.m_defines.data(), key.m_ps.m_defines.size());
		spookyhash_update(&context, &key.m_state.StreamOutput, sizeof(key.m_state.StreamOutput));
		spookyhash_update(&context, &key.m_state.BlendState, sizeof(key.m_state.BlendState));
		spookyhash_update(&context, &key.m_state.SampleMask, sizeof(key.m_state.SampleMask));
		spookyhash_update(&context, &key.m_state.RasterizerState, sizeof(key.m_state.RasterizerState));
		spookyhash_update(&context, &key.m_state.DepthStencilState, sizeof(key.m_state.DepthStencilState));
		spookyhash_update(&context, key.m_state.InputLayout.pInputElementDescs, key.m_state.InputLayout.NumElements * sizeof(D3D12_INPUT_LAYOUT_DESC));
		spookyhash_update(&context, &key.m_state.IBStripCutValue, sizeof(key.m_state.IBStripCutValue));
		spookyhash_update(&context, &key.m_state.PrimitiveTopologyType, sizeof(key.m_state.PrimitiveTopologyType));
		spookyhash_update(&context, &key.m_state.NumRenderTargets, sizeof(key.m_state.NumRenderTargets));
		spookyhash_update(&context, key.m_state.RTVFormats, sizeof(key.m_state.RTVFormats));
		spookyhash_update(&context, &key.m_state.DSVFormat, sizeof(key.m_state.DSVFormat));
		spookyhash_update(&context, &key.m_state.SampleDesc, sizeof(key.m_state.SampleDesc));
		spookyhash_update(&context, &key.m_state.NodeMask, sizeof(key.m_state.NodeMask));
		spookyhash_update(&context, &key.m_state.Flags, sizeof(key.m_state.Flags));
		spookyhash_final(&context, &seed1, &seed2);

		return seed1 ^ (seed2 << 1);
	}
};

template<>
struct std::hash<FComputePipelineDesc >
{
	std::size_t operator()(const FComputePipelineDesc& key) const
	{
		uint64_t seed1{}, seed2{};
		spookyhash_context context;
		spookyhash_context_init(&context, seed1, seed2);
		spookyhash_update(&context, key.m_rootsig.m_filename.data(), key.m_rootsig.m_filename.size());
		spookyhash_update(&context, key.m_rootsig.m_entrypoint.data(), key.m_rootsig.m_entrypoint.size());
		spookyhash_update(&context, key.m_cs.m_filename.data(), key.m_cs.m_filename.size());
		spookyhash_update(&context, key.m_cs.m_entrypoint.data(), key.m_cs.m_entrypoint.size());
		spookyhash_update(&context, key.m_cs.m_defines.data(), key.m_cs.m_defines.size());
		spookyhash_update(&context, &key.m_state.NodeMask, sizeof(key.m_state.NodeMask));
		spookyhash_update(&context, &key.m_state.Flags, sizeof(key.m_state.Flags));
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

bool operator==(const FGraphicsPipelineDesc& lhs, const FGraphicsPipelineDesc& rhs)
{
	return std::hash<FGraphicsPipelineDesc>{}(lhs) == std::hash<FGraphicsPipelineDesc>{}(rhs);
}

class FCommandListPool
{
public:
	FCommandList* GetOrCreate(const D3D12_COMMAND_LIST_TYPE type)
	{
		const std::lock_guard<std::mutex> lock(m_mutex);

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
			const std::lock_guard<std::mutex> lock(m_mutex);

			for (auto it = m_useList.begin(); it != m_useList.end();)
			{
				if (it->get() == cmdList)
				{
					m_freeList.push_back(std::move(*it));
					it = m_useList.erase(it);

					FCommandList* cl = m_freeList.back().get();
					cl->m_fenceValue = 0;
					cl->m_cmdAllocator->Reset();
					cl->m_cmdList->Reset(cl->m_cmdAllocator.Get(), nullptr);
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
	std::list<std::unique_ptr<FCommandList>> m_freeList;
	std::list<std::unique_ptr<FCommandList>> m_useList;
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

	std::mutex s_shaderCacheMutex;
	std::mutex s_rootsigCacheMutex;
	std::unordered_map<FShaderDesc, Microsoft::WRL::ComPtr<IDxcBlob>> s_shaderCache;
	std::unordered_map<FRootsigDesc, Microsoft::WRL::ComPtr<IDxcBlob>> s_rootsigCache;
	std::unordered_map<FGraphicsPipelineDesc, Microsoft::WRL::ComPtr<D3DPipelineState_t>> s_graphicsPSOPool;
	std::unordered_map<FComputePipelineDesc, Microsoft::WRL::ComPtr<D3DPipelineState_t>> s_computePSOPool;
}

namespace
{
	IDxcBlob* CacheRootsignature(const FRootsigDesc& rootsigDesc, const std::wstring& profile)
	{
		auto search = s_rootsigCache.find(rootsigDesc);
		if (search != s_rootsigCache.cend())
		{
			return search->second.Get();
		}
		else
		{
			auto& rsBlob = s_rootsigCache[rootsigDesc];
			AssertIfFailed(Demo::ShaderCompiler::CompileRootsignature(
				rootsigDesc.m_filename,
				rootsigDesc.m_entrypoint,
				profile,
				rsBlob.GetAddressOf()));

			return rsBlob.Get();
		}
	}

	IDxcBlob* CacheShader(const FShaderDesc& shaderDesc, const std::wstring& profile)
	{
		auto search = s_shaderCache.find(shaderDesc);
		if (search != s_shaderCache.cend())
		{
			return search->second.Get();
		}
		else
		{
			auto& shaderBlob = s_shaderCache[shaderDesc];
			AssertIfFailed(Demo::ShaderCompiler::CompileShader(
				shaderDesc.m_filename, 
				shaderDesc.m_entrypoint, 
				shaderDesc.m_defines,
				profile, 
				shaderBlob.GetAddressOf()));

			return shaderBlob.Get();
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
	s_graphicsQueue->SetName(L"Graphics Queue");

	cmdQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
	AssertIfFailed(s_d3dDevice->CreateCommandQueue(&cmdQueueDesc, IID_PPV_ARGS(s_computeQueue.GetAddressOf())));
	s_computeQueue->SetName(L"Compute Queue");

	cmdQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
	AssertIfFailed(s_d3dDevice->CreateCommandQueue(&cmdQueueDesc, IID_PPV_ARGS(s_copyQueue.GetAddressOf())));
	s_copyQueue->SetName(L"Copy Queue");

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
	s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV]->SetName(L"Bindless Descriptor Heap");

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
	s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV]->SetName(L"RTV Heap");

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
		s_backBuffers[bufferIdx]->SetName(L"Back Buffer");

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

FCommandList* Demo::D3D12::FetchCommandlist(const D3D12_COMMAND_LIST_TYPE type)
{
	return s_commandListPool.GetOrCreate(type);
}

D3DPipelineState_t* Demo::D3D12::FetchGraphicsPipelineState(
	const FRootsigDesc& rootsig,
	const FShaderDesc& vs,
	const FShaderDesc& ps,
	const D3D12_PRIMITIVE_TOPOLOGY_TYPE primitiveTopology,
	const DXGI_FORMAT dsvFormat,
	const uint32_t numRenderTargets,
	const std::initializer_list<DXGI_FORMAT>& rtvFormats,
	const std::initializer_list<D3D12_COLOR_WRITE_ENABLE>& colorWriteMask,
	const bool depthEnable,
	const D3D12_DEPTH_WRITE_MASK& depthWriteMask,
	const D3D12_COMPARISON_FUNC& depthFunc)
{
	FGraphicsPipelineDesc desc = {};

	// Shaders
	desc.m_vs = vs;
	desc.m_ps = ps;

	// Primitive Topology
	desc.m_state.PrimitiveTopologyType = primitiveTopology;

	// Rasterizer State
	desc.m_state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.m_state.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	desc.m_state.RasterizerState.FrontCounterClockwise = FALSE;
	desc.m_state.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
	desc.m_state.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
	desc.m_state.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	desc.m_state.RasterizerState.DepthClipEnable = TRUE;
	desc.m_state.RasterizerState.MultisampleEnable = FALSE;
	desc.m_state.RasterizerState.AntialiasedLineEnable = FALSE;
	desc.m_state.RasterizerState.ForcedSampleCount = 0;
	desc.m_state.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	// Blend State
	desc.m_state.BlendState.AlphaToCoverageEnable = FALSE;
	desc.m_state.BlendState.IndependentBlendEnable = FALSE;
	for (auto& rt : desc.m_state.BlendState.RenderTarget)
	{
		rt.BlendEnable = FALSE;
		rt.LogicOpEnable = FALSE;
		rt.RenderTargetWriteMask = 0;
	}

	// Depth Stencil State
	desc.m_state.DepthStencilState.DepthEnable = depthEnable;
	desc.m_state.DepthStencilState.DepthWriteMask = depthWriteMask;
	desc.m_state.DepthStencilState.DepthFunc = depthFunc;
	desc.m_state.DepthStencilState.StencilEnable = FALSE;
	desc.m_state.DSVFormat = dsvFormat;

	// Render Target(s) State
	assert(numRenderTargets == rtvFormats.size());
	desc.m_state.NumRenderTargets = numRenderTargets;
	int i = 0;
	for (auto& format : rtvFormats)
	{
		desc.m_state.RTVFormats[i++] = format;
	}

	// Multi Sampling State
	desc.m_state.SampleMask = UINT_MAX;
	desc.m_state.SampleDesc.Count = 1;

	// Create or Reuse
	auto search = s_graphicsPSOPool.find(desc);
	if (search != s_graphicsPSOPool.cend())
	{
		return search->second.Get();
	}
	else
	{
		IDxcBlob* rsBlob;
		{
			const std::lock_guard<std::mutex> lock(s_rootsigCacheMutex);
			rsBlob = CacheRootsignature(rootsig, L"rootsig_1_1");
		}

		IDxcBlob* vsBlob;
		IDxcBlob* psBlob;
		{
			const std::lock_guard<std::mutex> lock(s_shaderCacheMutex);
			vsBlob = CacheShader(vs, L"vs_6_4");
			psBlob = CacheShader(ps, L"ps_6_4");
		}

		Microsoft::WRL::ComPtr<D3DRootSignature_t> rs;
		s_d3dDevice->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(rs.GetAddressOf()));
		desc.m_state.pRootSignature = rs.Get();
	
		desc.m_state.VS.pShaderBytecode = vsBlob->GetBufferPointer();
		desc.m_state.VS.BytecodeLength = vsBlob->GetBufferSize();
		desc.m_state.PS.pShaderBytecode = psBlob->GetBufferPointer();
		desc.m_state.PS.BytecodeLength = psBlob->GetBufferSize();

		AssertIfFailed(s_d3dDevice->CreateGraphicsPipelineState(&desc.m_state, IID_PPV_ARGS(s_graphicsPSOPool[desc].GetAddressOf())));
		return s_graphicsPSOPool[desc].Get();
	}
}

D3DPipelineState_t* Demo::D3D12::FetchComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC  desc)
{
	const std::lock_guard<std::mutex> lock(s_shaderCacheMutex);
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

D3DFence_t* Demo::D3D12::ExecuteCommandlists(const D3D12_COMMAND_LIST_TYPE commandQueueType, std::vector<FCommandList*> commandLists)
{
	std::vector<ID3D12CommandList*> d3dCommandLists;
	size_t latestFenceValue = 0;
	D3DFence_t* latestFence = {};

	// Accumulate CLs and keep tab of the latest fence
	for (FCommandList* cl : commandLists)
	{
		D3DCommandList_t* d3dCL = cl->m_cmdList.Get();
		d3dCL->Close();
		d3dCommandLists.push_back(d3dCL);

		if (cl->m_fenceValue > latestFenceValue)
		{
			latestFenceValue = cl->m_fenceValue;
			latestFence = cl->m_fence.Get();
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
	for (FCommandList* cl : commandLists)
	{
		activeCommandQueue->Signal(cl->m_fence.Get(), cl->m_fenceValue);
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
	
	AssertIfFailed(GetDevice()->CreateCommandAllocator(type, IID_PPV_ARGS(m_cmdAllocator.GetAddressOf())));

	AssertIfFailed(
		GetDevice()->CreateCommandList(
			0,
			type,
			m_cmdAllocator.Get(),
			nullptr,
			IID_PPV_ARGS(m_cmdList.GetAddressOf()))
	);

	AssertIfFailed(
		GetDevice()->CreateFence(
			0,
			D3D12_FENCE_FLAG_NONE,
			IID_PPV_ARGS(m_fence.GetAddressOf()))
	);
}