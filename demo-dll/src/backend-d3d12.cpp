#include <backend-d3d12.h>
#include <common.h>
#include <shadercompiler.h>
#include <ppltasks.h>
#include <concurrent_unordered_map.h>
#include <concurrent_queue.h>
#include <common.h>
#include <spookyhash_api.h>
#include <microprofile.h>
#include <imgui.h>
#include <dxgidebug.h>
#include <string>
#include <sstream>
#include <fstream>
#include <list>
#include <unordered_map>
#include <system_error>
#include <utility>

using namespace RenderBackend12;

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Constants
//-----------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t k_backBufferCount = 2;
constexpr size_t k_rtvHeapSize = 32;
constexpr size_t k_renderTextureMemory = 32 * 1024 * 1024;

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Forward Declarations
//-----------------------------------------------------------------------------------------------------------------------------------------------
class FUploadBufferPool;
class FRenderTexturePool;

namespace
{
	D3DDevice_t* GetDevice();
	D3DCommandQueue_t* GetGraphicsQueue();
	D3DCommandQueue_t* GetComputeQueue();
	D3DCommandQueue_t* GetCopyQueue();
	FUploadBufferPool* GetUploadBufferPool();
	FRenderTexturePool* GetRenderTexturePool();
	concurrency::concurrent_queue<uint32_t>& GetBindlessIndexPool();
	concurrency::concurrent_queue<uint32_t>& GetRTVIndexPool();
}

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Common
//-----------------------------------------------------------------------------------------------------------------------------------------------

namespace
{
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

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														User defined overrides
//-----------------------------------------------------------------------------------------------------------------------------------------------

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

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Command Lists
//-----------------------------------------------------------------------------------------------------------------------------------------------

class FCommandListPool
{
public:
	FCommandList* GetOrCreate(const D3D12_COMMAND_LIST_TYPE type)
	{
		const std::lock_guard<std::mutex> lock(m_mutex);

		// Reuse CL
		for (auto it = m_freeList.begin(); it != m_freeList.end(); ++it)
		{
			if (it->get()->m_type == type)
			{
				m_useList.push_back(std::move(*it));
				m_freeList.erase(it);
				
				FCommandList* cl = m_useList.back().get();
				cl->m_fenceValue = ++m_fenceCounter;
				return cl;
			}
		}

		// New CL
		m_useList.push_back(std::make_unique<FCommandList>(type, ++m_fenceCounter));
		return m_useList.back().get();
	}

	void Retire(FCommandList* cmdList)
	{
		auto waitForFenceTask = concurrency::create_task([cmdList, this]()
		{
			HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
			if (event)
			{
				DebugAssert(cmdList->m_fenceValue != 0);
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
					cl->m_cmdList->Reset(cl->m_cmdAllocator.get(), nullptr);
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
		DebugAssert(m_useList.empty(), "All CLs should be retired at this point");
		m_freeList.clear();
	}

private:
	std::atomic_size_t m_fenceCounter{ 0 };
	std::mutex m_mutex;
	std::list<std::unique_ptr<FCommandList>> m_freeList;
	std::list<std::unique_ptr<FCommandList>> m_useList;
};

FCommandList::FCommandList(const D3D12_COMMAND_LIST_TYPE type, const size_t  fenceValue) :
	m_type{ type },
	m_fenceValue{ fenceValue }
{

	AssertIfFailed(GetDevice()->CreateCommandAllocator(type, IID_PPV_ARGS(m_cmdAllocator.put())));

	AssertIfFailed(GetDevice()->CreateCommandList(
		0,
		type,
		m_cmdAllocator.get(),
		nullptr,
		IID_PPV_ARGS(m_cmdList.put())));

	AssertIfFailed(GetDevice()->CreateFence(
		0,
		D3D12_FENCE_FLAG_NONE,
		IID_PPV_ARGS(m_fence.put())));
}

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Resource Upload
//-----------------------------------------------------------------------------------------------------------------------------------------------

class FUploadBufferPool
{
public:
	FResource* GetOrCreate(const std::wstring& name, const size_t sizeInBytes)
	{
		const std::lock_guard<std::mutex> lock(m_mutex);

		// Reuse buffer
		for (auto it = m_freeList.begin(); it != m_freeList.end(); ++it)
		{
			if ((*it)->m_d3dResource->GetDesc().Width >= sizeInBytes)
			{
				m_useList.push_back(std::move(*it));
				m_freeList.erase(it);

				FResource* buffer = m_useList.back().get();
				buffer->SetName(name);
				return buffer;
			}
		}

		// New buffer
		auto newBuffer = std::make_unique<FResource>();

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

		AssertIfFailed(newBuffer->InitCommittedResource(name, heapDesc, resourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ));

		m_useList.push_back(std::move(newBuffer));
		return m_useList.back().get();
	}

	void Retire(const FResource* buffer, const FCommandList* dependantCL)
	{
		auto waitForFenceTask = concurrency::create_task([dependantCL, this]()
		{
			HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
			if (event)
			{		
				dependantCL->m_fence->SetEventOnCompletion(dependantCL->m_fenceValue, event);
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
					m_freeList.push_back(std::move(*it));
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
		DebugAssert(m_useList.empty(), "All buffers should be retired at this point");
		m_freeList.clear();
	}

private:
	std::atomic_size_t m_fenceCounter{ 0 };
	std::mutex m_mutex;
	std::list<std::unique_ptr<FResource>> m_freeList;
	std::list<std::unique_ptr<FResource>> m_useList;
};

FResourceUploadContext::FResourceUploadContext(const size_t uploadBufferSizeInBytes) :
	m_currentOffset{ 0 }
{
	DebugAssert(uploadBufferSizeInBytes != 0);

	// Round up to power of 2
	unsigned long n;
	_BitScanReverse64(&n, uploadBufferSizeInBytes);
	m_sizeInBytes = (1 << (n + 1));

	m_copyCommandlist = FetchCommandlist(D3D12_COMMAND_LIST_TYPE_COPY);

	m_uploadBuffer = GetUploadBufferPool()->GetOrCreate(L"upload_context_buffer", m_sizeInBytes);
	m_uploadBuffer->m_d3dResource->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedPtr));
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
	DebugAssert(totalBytes <= capacity, "Upload buffer is too small!");

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
		m_copyCommandlist->m_cmdList->CopyBufferRegion(
			destinationResource,
			0,
			m_uploadBuffer->m_d3dResource,
			layouts[0].Offset,
			layouts[0].Footprint.Width);
	}
	else
	{
		for (UINT i = 0; i < numSubresources; ++i)
		{
			D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
			srcLocation.pResource = m_uploadBuffer->m_d3dResource;
			srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			srcLocation.PlacedFootprint = layouts[i];

			D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
			dstLocation.pResource = destinationResource;
			dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dstLocation.SubresourceIndex = i + firstSubresource;

			m_copyCommandlist->m_cmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
		}
	}

	m_currentOffset += totalBytes;
	m_pendingTransitions.push_back(transition);
}

D3DFence_t* FResourceUploadContext::SubmitUploads(FCommandList* owningCL)
{
	D3DFence_t* clFence = ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_COPY, { m_copyCommandlist });

	GetUploadBufferPool()->Retire(m_uploadBuffer, m_copyCommandlist);

	// Transition all the destination resources on the owning/direct CL since the copy CLs have limited transition capabilities
	for (auto& transitionCallback : m_pendingTransitions)
	{
		transitionCallback(owningCL);
	}

	return clFence;
}

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Generic Resources
//-----------------------------------------------------------------------------------------------------------------------------------------------

FResource::~FResource()
{
	if (m_d3dResource)
	{
		m_d3dResource->Release();
		m_d3dResource = nullptr;
	}
}

void FResource::SetName(const std::wstring& name)
{
	m_name = name;
	m_d3dResource->SetName(name.c_str());
}

HRESULT FResource::InitCommittedResource(const std::wstring& name, const D3D12_HEAP_PROPERTIES& heapProperties, const D3D12_RESOURCE_DESC& resourceDesc, const D3D12_RESOURCE_STATES initialState)
{
	HRESULT hr = GetDevice()->CreateCommittedResource(
		&heapProperties, 
		D3D12_HEAP_FLAG_NONE, 
		&resourceDesc,
		initialState, 
		nullptr,
		IID_PPV_ARGS(&m_d3dResource));

	SetName(name);

	m_subresourceStates.clear();
	for (int i = 0; i < resourceDesc.MipLevels; ++i)
	{
		m_subresourceStates.push_back(initialState);
	}

	return hr;
}

HRESULT FResource::InitReservedResource(const std::wstring& name, const D3D12_RESOURCE_DESC& resourceDesc, const D3D12_RESOURCE_STATES initialState)
{
	HRESULT hr = GetDevice()->CreateReservedResource(
		&resourceDesc,
		initialState,
		nullptr,
		IID_PPV_ARGS(&m_d3dResource));

	SetName(name);

	m_subresourceStates.clear();
	for (int i = 0; i < resourceDesc.MipLevels; ++i)
	{
		m_subresourceStates.push_back(initialState);
	}

	return hr;
}

void FResource::Transition(FCommandList* cmdList, const uint32_t subresourceIndex, const D3D12_RESOURCE_STATES destState)
{
	uint32_t subId = (subresourceIndex == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? 0 : subresourceIndex);
	D3D12_RESOURCE_STATES beforeState = m_subresourceStates[subId];

	D3D12_RESOURCE_BARRIER barrierDesc = {};
	barrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrierDesc.Transition.pResource = m_d3dResource;
	barrierDesc.Transition.StateBefore = beforeState;
	barrierDesc.Transition.StateAfter = destState;
	barrierDesc.Transition.Subresource = subresourceIndex;
	cmdList->m_cmdList->ResourceBarrier(1, &barrierDesc);

	cmdList->m_postExecuteCallbacks.push_back(
		[this, subresourceIndex, destState]()
		{
			if (subresourceIndex == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
			{
				for (auto& state : m_subresourceStates)
				{
					state = destState;
				}
			}
			else
			{
				m_subresourceStates[subresourceIndex] = destState;
			}
		});
}

FTransientBuffer::~FTransientBuffer()
{
	GetUploadBufferPool()->Retire(m_resource, m_dependentCmdlist);
	m_dependentCmdlist = nullptr;
}

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Render Textures
//-----------------------------------------------------------------------------------------------------------------------------------------------

class FRenderTexturePool
{
public:
	void Initialize(size_t sizeInBytes)
	{
		constexpr size_t k_tileSize = 64 * 1024;

		// align to 64k
		sizeInBytes = (sizeInBytes + k_tileSize - 1) & ~(k_tileSize - 1);

		D3D12_HEAP_DESC desc
		{
			.SizeInBytes = sizeInBytes,
			.Properties
			{
				.Type = D3D12_HEAP_TYPE_DEFAULT,
				.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
				.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
			},
			.Alignment = 0,
			.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES
		};

		AssertIfFailed(GetDevice()->CreateHeap(&desc, IID_PPV_ARGS(m_heap.put())));

		size_t numTiles = sizeInBytes / k_tileSize;
		for (int i = 0; i < numTiles; ++i)
		{
			m_tilePool.push(i);
		}

		AssertIfFailed(GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.put())));
	}

	FResource* GetOrCreate(
		const std::wstring& name, 
		const DXGI_FORMAT format,
		const size_t width,
		const size_t height,
		const size_t mipLevels,
		const size_t depth)
	{
		const std::lock_guard<std::mutex> lock(m_mutex);

		// Reuse buffer
		for (auto it = m_freeList.begin(); it != m_freeList.end(); ++it)
		{
			const D3D12_RESOURCE_DESC& desc = (*it)->m_d3dResource->GetDesc();

			if (desc.Format == format &&
				desc.Width == width &&
				desc.Height == height &&
				desc.MipLevels == mipLevels &&
				desc.DepthOrArraySize == depth)
			{
				m_useList.push_back(std::move(*it));
				m_freeList.erase(it);

				FResource* rt = m_useList.back().get();
				rt->SetName(name.c_str());
				return rt;
			}
		}

		// New render texture
		auto newRt = std::make_unique<FResource>();

		D3D12_RESOURCE_DESC rtDesc = {};
		rtDesc.Dimension = depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		rtDesc.Width = width;
		rtDesc.Height = (UINT)height;
		rtDesc.DepthOrArraySize = (UINT16)depth;
		rtDesc.MipLevels = (UINT16)mipLevels;
		rtDesc.Format = format;
		rtDesc.SampleDesc.Count = 1;
		rtDesc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
		rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		AssertIfFailed(newRt->InitReservedResource(name, rtDesc, D3D12_RESOURCE_STATE_RENDER_TARGET));

		m_useList.push_back(std::move(newRt));
		return m_useList.back().get();
	}

	void Retire(const FResource* rt)
	{
		auto waitForFenceTask = concurrency::create_task([this]() mutable
		{
			GetGraphicsQueue()->Signal(m_fence.get(), ++m_fenceValue);
			HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
			if (event)
			{
				m_fence->SetEventOnCompletion(m_fenceValue, event);
				WaitForSingleObject(event, INFINITE);
			}
		});

		auto addToFreePool = [rt, this]()
		{
			const std::lock_guard<std::mutex> lock(m_mutex);

			for (auto it = m_useList.begin(); it != m_useList.end();)
			{
				if (it->get() == rt)
				{
					m_freeList.push_back(std::move(*it));
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

	std::vector<uint32_t> AllocateTiles(const size_t numTiles)
	{
		std::vector<uint32_t> tileList;
		size_t remainingAllocation = numTiles;
		while (remainingAllocation > 0)
		{
			uint32_t tileIndex;
			bool result = m_tilePool.try_pop(tileIndex);
			DebugAssert(result, "Ran out of tiles");
			tileList.push_back(tileIndex);
			remainingAllocation--;
		}

		return tileList;
	}

	void ReturnTiles(std::vector<uint32_t>&& tileList)
	{
		for (uint32_t tileIndex : tileList)
		{
			m_tilePool.push(tileIndex);
		}

		tileList.clear();
	}

	D3DHeap_t* GetHeap()
	{
		return m_heap.get();
	}

	void Clear()
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		DebugAssert(m_useList.empty(), "All render textures should be retired at this point");
		m_freeList.clear();
		m_tilePool.clear();
	}

private:
	std::mutex m_mutex;
	winrt::com_ptr<D3DHeap_t> m_heap;
	winrt::com_ptr<D3DFence_t> m_fence;
	size_t m_fenceValue = 0;
	std::list<std::unique_ptr<FResource>> m_freeList;
	std::list<std::unique_ptr<FResource>> m_useList;
	concurrency::concurrent_queue<uint32_t> m_tilePool;
};

FRenderTexture::~FRenderTexture()
{
	const size_t numTiles = m_tileList.size();
	std::vector<D3D12_TILE_RANGE_FLAGS> rangeFlags(numTiles, D3D12_TILE_RANGE_FLAG_NULL);
	std::vector<UINT> rangeTileCounts(numTiles, 1);

	GetGraphicsQueue()->UpdateTileMappings(
		m_resource->m_d3dResource,
		1,
		nullptr,
		nullptr,
		GetRenderTexturePool()->GetHeap(),
		numTiles,
		rangeFlags.data(),
		m_tileList.data(),
		rangeTileCounts.data(),
		D3D12_TILE_MAPPING_FLAG_NONE);

	GetRenderTexturePool()->ReturnTiles(std::move(m_tileList));
	GetRenderTexturePool()->Retire(m_resource);
	//s_bindlessIndexPool.push(m_srvIndex);
	for (uint32_t rtvIndex : m_rtvIndices)
	{
		GetRTVIndexPool().push(rtvIndex);
	}
}

struct FTimestampedBlob
{
	FILETIME m_timestamp;
	winrt::com_ptr<IDxcBlob> m_blob;
};

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														RenderBackend12
//-----------------------------------------------------------------------------------------------------------------------------------------------

namespace RenderBackend12
{
#if defined (_DEBUG)
	winrt::com_ptr<D3DDebug_t> s_debugController;
#endif

	winrt::com_ptr<DXGIFactory_t> s_dxgiFactory;
	winrt::com_ptr<D3DDevice_t> s_d3dDevice;

	winrt::com_ptr<DXGISwapChain_t> s_swapChain;
	std::unique_ptr<FResource> s_backBuffers[k_backBufferCount];
	uint32_t s_currentBufferIndex;

	winrt::com_ptr<D3DFence_t> s_frameFence;
	uint64_t s_frameFenceValues[k_backBufferCount];

	uint32_t s_descriptorSize[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES];
	winrt::com_ptr<D3DDescriptorHeap_t> s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES];

	winrt::com_ptr<D3DCommandQueue_t> s_graphicsQueue;
	winrt::com_ptr<D3DCommandQueue_t> s_computeQueue;
	winrt::com_ptr<D3DCommandQueue_t> s_copyQueue;

	FCommandListPool s_commandListPool;
	FUploadBufferPool s_uploadBufferPool;
	FRenderTexturePool s_renderTexturePool;

	concurrency::concurrent_unordered_map<FShaderDesc, FTimestampedBlob> s_shaderCache;
	concurrency::concurrent_unordered_map<FRootsigDesc, FTimestampedBlob> s_rootsigCache;
	concurrency::concurrent_unordered_map<std::wstring, FBitmapTexture> s_textureCache;
	concurrency::concurrent_unordered_map<D3D12_GRAPHICS_PIPELINE_STATE_DESC, winrt::com_ptr<D3DPipelineState_t>> s_graphicsPSOPool;
	concurrency::concurrent_unordered_map<D3D12_COMPUTE_PIPELINE_STATE_DESC, winrt::com_ptr<D3DPipelineState_t>> s_computePSOPool;
	concurrency::concurrent_queue<uint32_t> s_bindlessIndexPool;
	concurrency::concurrent_queue<uint32_t> s_rtvIndexPool;
}

namespace 
{
	D3DDevice_t* GetDevice()
	{
		return RenderBackend12::s_d3dDevice.get();
	}

	D3DCommandQueue_t* GetGraphicsQueue()
	{
		return RenderBackend12::s_graphicsQueue.get();
	}

	D3DCommandQueue_t* GetComputeQueue()
	{
		return RenderBackend12::s_computeQueue.get();
	}

	D3DCommandQueue_t* GetCopyQueue()
	{
		return RenderBackend12::s_copyQueue.get();
	}

	FUploadBufferPool* GetUploadBufferPool()
	{
		return &RenderBackend12::s_uploadBufferPool;
	}

	FRenderTexturePool* GetRenderTexturePool()
	{
		return &RenderBackend12::s_renderTexturePool;
	}

	concurrency::concurrent_queue<uint32_t>& GetBindlessIndexPool()
	{
		return RenderBackend12::s_bindlessIndexPool;
	}

	concurrency::concurrent_queue<uint32_t>& GetRTVIndexPool()
	{
		return RenderBackend12::s_rtvIndexPool;
	}
}

bool RenderBackend12::Initialize(const HWND& windowHandle, const uint32_t resX, const uint32_t resY)
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
	{
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
		rtvHeapDesc.NumDescriptors = k_rtvHeapSize;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		AssertIfFailed(
			s_d3dDevice->CreateDescriptorHeap(
				&rtvHeapDesc,
				IID_PPV_ARGS(s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].put()))
		);
		s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV]->SetName(L"rtv_heap");

		// Cache available indices
		for (int i = 0; i < k_rtvHeapSize; ++i)
		{
			s_rtvIndexPool.push(i);
		}
	}

	// Swap chain
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width = resX;
	swapChainDesc.Height = resY;
	swapChainDesc.Format = Settings::k_backBufferFormat;
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
		s_backBuffers[bufferIdx] = std::make_unique<FResource>();
		AssertIfFailed(s_swapChain->GetBuffer(bufferIdx, IID_PPV_ARGS(&s_backBuffers[bufferIdx]->m_d3dResource)));

		std::wstringstream s;
		s << L"back_buffer_" << bufferIdx;
		s_backBuffers[bufferIdx]->SetName(s.str().c_str());

		s_backBuffers[bufferIdx]->m_subresourceStates.push_back(D3D12_RESOURCE_STATE_PRESENT);

		uint32_t rtvIndex;
		bool ok = s_rtvIndexPool.try_pop(rtvIndex);
		DebugAssert(ok, "Ran out of RTV descriptors");

		D3D12_CPU_DESCRIPTOR_HANDLE rtvDescriptor;
		rtvDescriptor.ptr = s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV]->GetCPUDescriptorHandleForHeapStart().ptr +
			rtvIndex * s_descriptorSize[D3D12_DESCRIPTOR_HEAP_TYPE_RTV];

		s_d3dDevice->CreateRenderTargetView(s_backBuffers[bufferIdx]->m_d3dResource, nullptr, rtvDescriptor);
	}

	s_currentBufferIndex = s_swapChain->GetCurrentBackBufferIndex();

	// Render texture memory
	s_renderTexturePool.Initialize(k_renderTextureMemory);

	// Frame sync
	AssertIfFailed(s_d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(s_frameFence.put())));

	for (auto& val : s_frameFenceValues)
	{
		val = 0;
	}

	void* cmdQueues[] = { s_graphicsQueue.get() };
	MicroProfileGpuInitD3D12(GetDevice(), 1, cmdQueues);
	MicroProfileSetCurrentNodeD3D12(0);

	return true;
}

void RenderBackend12::Teardown()
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

	MicroProfileGpuShutdown();

	s_commandListPool.Clear();
	s_uploadBufferPool.Clear();
	s_renderTexturePool.Clear();

	s_shaderCache.clear();
	s_rootsigCache.clear();
	s_textureCache.clear();
	s_graphicsPSOPool.clear();
	s_computePSOPool.clear();
	s_bindlessIndexPool.clear();
	s_rtvIndexPool.clear();

	s_frameFence.get()->Release();

	for (auto& descriptorHeap : s_descriptorHeaps)
	{
		if (descriptorHeap)
		{
			descriptorHeap.get()->Release();
		}
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

FCommandList* RenderBackend12::FetchCommandlist(const D3D12_COMMAND_LIST_TYPE type)
{
	return s_commandListPool.GetOrCreate(type);
}

IDxcBlob* RenderBackend12::CacheShader(const FShaderDesc& shaderDesc, const std::wstring& profile)
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

IDxcBlob* RenderBackend12::CacheRootsignature(const FRootsigDesc& rootsigDesc, const std::wstring& profile)
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

winrt::com_ptr<D3DRootSignature_t> RenderBackend12::FetchGraphicsRootSignature(const FRootsigDesc& rootsig)
{
	IDxcBlob* rsBlob = CacheRootsignature(rootsig, L"rootsig_1_1");
	winrt::com_ptr<D3DRootSignature_t> rs;
	s_d3dDevice->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(rs.put()));
	return rs;
}

D3DPipelineState_t* RenderBackend12::FetchGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
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

D3DPipelineState_t* RenderBackend12::FetchComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc)
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

D3D12_CPU_DESCRIPTOR_HANDLE RenderBackend12::GetBackBufferDescriptor()
{
	D3D12_CPU_DESCRIPTOR_HANDLE descriptor;
	descriptor.ptr = s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV]->GetCPUDescriptorHandleForHeapStart().ptr +
		s_currentBufferIndex * s_descriptorSize[D3D12_DESCRIPTOR_HEAP_TYPE_RTV];

	return descriptor;
}

FResource* RenderBackend12::GetBackBufferResource()
{
	return s_backBuffers[s_currentBufferIndex].get();
}

D3DFence_t* RenderBackend12::ExecuteCommandlists(const D3D12_COMMAND_LIST_TYPE commandQueueType, std::initializer_list<FCommandList*> commandLists)
{
	std::vector<ID3D12CommandList*> d3dCommandLists;
	size_t latestFenceValue = 0;
	D3DFence_t* latestFence = {};

	// Accumulate CLs and keep tab of the latest fence
	for (const FCommandList* cl : commandLists)
	{
		D3DCommandList_t* d3dCL = cl->m_cmdList.get();
		d3dCL->Close();
		d3dCommandLists.push_back(d3dCL);

		if (cl->m_fenceValue > latestFenceValue)
		{
			latestFenceValue = cl->m_fenceValue;
			latestFence = cl->m_fence.get();
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
	for (FCommandList* cl : commandLists)
	{
		for (auto& callbackProc : cl->m_postExecuteCallbacks)
		{
			callbackProc();
		}

		cl->m_postExecuteCallbacks.clear();

		activeCommandQueue->Signal(cl->m_fence.get(), cl->m_fenceValue);
		s_commandListPool.Retire(cl);
	}

	// Return the latest fence
	return latestFence;
}

void RenderBackend12::PresentDisplay()
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

D3DDescriptorHeap_t* RenderBackend12::GetBindlessShaderResourceHeap()
{
	return s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].get();
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t descriptorIndex)
{
	D3D12_CPU_DESCRIPTOR_HANDLE descriptor;
	descriptor.ptr = s_descriptorHeaps[type]->GetCPUDescriptorHandleForHeapStart().ptr +
		descriptorIndex * s_descriptorSize[type];

	return descriptor;
}

D3D12_GPU_DESCRIPTOR_HANDLE RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t descriptorIndex)
{
	D3D12_GPU_DESCRIPTOR_HANDLE descriptor;
	descriptor.ptr = s_descriptorHeaps[type]->GetGPUDescriptorHandleForHeapStart().ptr +
		descriptorIndex * s_descriptorSize[type];

	return descriptor;
}

FTransientBuffer RenderBackend12::CreateTransientBuffer(
	const std::wstring& name,
	const size_t sizeInBytes,
	const FCommandList* dependentCL,
	std::function<void(uint8_t*)> uploadFunc)
{
	DebugAssert(sizeInBytes != 0);

	DWORD n;
	_BitScanReverse64(&n, sizeInBytes);
	const size_t powOf2Size = (1 << (n + 1));
	FResource* buffer = s_uploadBufferPool.GetOrCreate(name, powOf2Size);

	uint8_t* pData;
	buffer->m_d3dResource->Map(0, nullptr, reinterpret_cast<void**>(&pData));

	if (uploadFunc)
	{
		uploadFunc(pData);
		buffer->m_d3dResource->Unmap(0, nullptr);
	}

	FTransientBuffer tempBuffer = {};
	tempBuffer.m_resource = buffer;
	tempBuffer.m_dependentCmdlist = dependentCL;

	return tempBuffer;
}

FRenderTexture RenderBackend12::CreateRenderTexture(
	const std::wstring& name,
	const DXGI_FORMAT format,
	const size_t width,
	const size_t height,
	const size_t mipLevels,
	const size_t depth)
{

	FResource* rtResource = s_renderTexturePool.GetOrCreate(name, format, width, height, mipLevels, depth);

	uint32_t numTiles;
	GetDevice()->GetResourceTiling(rtResource->m_d3dResource, &numTiles, nullptr, nullptr, nullptr, 0, nullptr);
	std::vector<uint32_t> tileList = s_renderTexturePool.AllocateTiles(numTiles);

	std::vector<D3D12_TILE_RANGE_FLAGS> rangeFlags(numTiles, D3D12_TILE_RANGE_FLAG_NONE);
	std::vector<UINT> rangeTileCounts(numTiles, 1);

	s_graphicsQueue->UpdateTileMappings(
		rtResource->m_d3dResource,
		1,
		nullptr,
		nullptr,
		s_renderTexturePool.GetHeap(),
		numTiles,
		rangeFlags.data(),
		tileList.data(),
		rangeTileCounts.data(),
		D3D12_TILE_MAPPING_FLAG_NONE);

	// RTV Descriptor
	std::vector<uint32_t> rtvIndices;
	for(int mip = 0; mip < mipLevels; ++mip)
	{
		uint32_t rtvIndex;
		bool ok = s_rtvIndexPool.try_pop(rtvIndex);
		DebugAssert(ok, "Ran out of RTV descriptors");
		D3D12_CPU_DESCRIPTOR_HANDLE rtv;
		rtv.ptr = s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV]->GetCPUDescriptorHandleForHeapStart().ptr +
			rtvIndex * s_descriptorSize[D3D12_DESCRIPTOR_HEAP_TYPE_RTV];

		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = format;
		if (depth == 1)
		{
			rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			rtvDesc.Texture2D.MipSlice = mip;
		}
		else
		{
			rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
			rtvDesc.Texture3D.MipSlice = mip;
		}

		GetDevice()->CreateRenderTargetView(rtResource->m_d3dResource, &rtvDesc, rtv);
		rtvIndices.push_back(rtvIndex);
	}

	// Cache SRV Description
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	{
		srvDesc.Format = format;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		if (depth == 1)
		{
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = mipLevels;
			srvDesc.Texture2D.MostDetailedMip = 0;
		}
		else
		{
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
			srvDesc.Texture3D.MipLevels = mipLevels;
			srvDesc.Texture3D.MostDetailedMip = 0;
		}
	}

	FRenderTexture rt = {};
	rt.m_resource = rtResource;
	rt.m_tileList = std::move(tileList);
	rt.m_rtvIndices = std::move(rtvIndices);
	rt.m_srvDesc = std::move(srvDesc);
	//rt.m_srvIndex = srvIndex;

	return rt;
}

uint32_t RenderBackend12::CacheTexture(const std::wstring& name, FResourceUploadContext* uploadContext)
{
	auto search = s_textureCache.find(name);
	if (search != s_textureCache.cend())
	{
		return search->second.m_srvIndex;
	}
	else
	{
		FBitmapTexture& newTexture = s_textureCache[name];

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

			AssertIfFailed(newTexture.m_resource.InitCommittedResource(name, props, desc, D3D12_RESOURCE_STATE_COPY_DEST));
		}

		// Upload texture data
		{
			D3D12_SUBRESOURCE_DATA srcData;
			srcData.pData = pixels;
			srcData.RowPitch = width * bpp;
			srcData.SlicePitch = height * srcData.RowPitch;
			uploadContext->UpdateSubresources(
				newTexture.m_resource.m_d3dResource, 
				0, 
				1, 
				&srcData,
				[&newTexture](FCommandList* cmdList)
				{
					newTexture.m_resource.Transition(cmdList, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				});
		}

		// Descriptor
		{
			bool ok = s_bindlessIndexPool.try_pop(newTexture.m_srvIndex);
			DebugAssert(ok, "Ran out of bindless descriptors");
			D3D12_CPU_DESCRIPTOR_HANDLE srv;
			srv.ptr = s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV]->GetCPUDescriptorHandleForHeapStart().ptr +
				newTexture.m_srvIndex * s_descriptorSize[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV];

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = format;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = mipLevels;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			GetDevice()->CreateShaderResourceView(newTexture.m_resource.m_d3dResource, &srvDesc, srv);
		}

		return newTexture.m_srvIndex;
	}
}