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
constexpr size_t k_dsvHeapSize = 8;
constexpr size_t k_renderTextureMemory = 32 * 1024 * 1024;

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Forward Declarations
//-----------------------------------------------------------------------------------------------------------------------------------------------
class FUploadBufferPool;
class FRenderTexturePool;
class FBindlessIndexPool;

namespace
{
	D3DDevice_t* GetDevice();
	D3DCommandQueue_t* GetGraphicsQueue();
	D3DCommandQueue_t* GetComputeQueue();
	D3DCommandQueue_t* GetCopyQueue();
	D3DDescriptorHeap_t* GetDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_TYPE type);
	uint32_t GetDescriptorSize(const D3D12_DESCRIPTOR_HEAP_TYPE type);
	FUploadBufferPool* GetUploadBufferPool();
	FRenderTexturePool* GetRenderTexturePool();
	FBindlessIndexPool* GetBindlessPool();
	concurrency::concurrent_queue<uint32_t>& GetRTVIndexPool();
	concurrency::concurrent_queue<uint32_t>& GetDSVIndexPool();
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

	bool IsShaderResource(D3D12_RESOURCE_STATES state)
	{
		return state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE || 
			state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC GetNullSRVDesc(D3D12_SRV_DIMENSION viewDimension)
	{
		DXGI_FORMAT format = {};
		switch (viewDimension)
		{
		case D3D12_SRV_DIMENSION_BUFFER: 
			format = DXGI_FORMAT_R32_FLOAT; 
			break;
		case D3D12_SRV_DIMENSION_TEXTURE2D: 
		case D3D12_SRV_DIMENSION_TEXTURECUBE:
			format = DXGI_FORMAT_R8G8B8A8_UNORM; 
			break;
		default:
			DebugAssert(false, "Unsupported");
		}

		D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
		desc.Format = format;
		desc.ViewDimension = viewDimension;
		desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		return desc;
	}

	D3D12_UNORDERED_ACCESS_VIEW_DESC GetNullUavDesc(D3D12_UAV_DIMENSION viewDimension)
	{
		DXGI_FORMAT format = {};
		switch (viewDimension)
		{
		case D3D12_UAV_DIMENSION_TEXTURE2D:
		case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
			format = DXGI_FORMAT_R8G8B8A8_UNORM; 
			break;
		default:
			DebugAssert(false, "Unsupported");
		}

		D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
		desc.Format = format;
		desc.ViewDimension = viewDimension;
		return desc;
	}

	DXGI_FORMAT GetTypelessDepthStencilFormat(DXGI_FORMAT depthStencilFormat)
	{
		switch (depthStencilFormat)
		{
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: 
			return DXGI_FORMAT_R32G8X24_TYPELESS;
		case DXGI_FORMAT_D32_FLOAT: 
			return DXGI_FORMAT_R32_TYPELESS;
		case DXGI_FORMAT_D24_UNORM_S8_UINT: 
			return DXGI_FORMAT_R24G8_TYPELESS;
		case DXGI_FORMAT_D16_UNORM: 
			return DXGI_FORMAT_R16_TYPELESS;
		default:
			DebugAssert(false);
			return DXGI_FORMAT_UNKNOWN;
		}
	}

	DXGI_FORMAT GetSrvDepthFormat(DXGI_FORMAT depthStencilFormat)
	{
		switch (depthStencilFormat)
		{
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
		case DXGI_FORMAT_D32_FLOAT:
			return DXGI_FORMAT_R32_FLOAT;
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
			return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		case DXGI_FORMAT_D16_UNORM:
			return DXGI_FORMAT_R16_UNORM;
		default:
			DebugAssert(false);
			return DXGI_FORMAT_UNKNOWN;
		}
	}

	DXGI_FORMAT GetSrvStencilFormat(DXGI_FORMAT depthStencilFormat)
	{
		switch (depthStencilFormat)
		{
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
			return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_D16_UNORM:
		default:
			DebugAssert(false);
			return DXGI_FORMAT_UNKNOWN;
		}
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

bool operator==(const DXGI_SAMPLE_DESC& lhs, const DXGI_SAMPLE_DESC& rhs)
{
	return lhs.Count == rhs.Count && lhs.Quality == rhs.Quality;
}

bool operator==(const D3D12_RESOURCE_DESC& lhs, const D3D12_RESOURCE_DESC& rhs)
{
	return lhs.Dimension == rhs.Dimension &&
		lhs.Width == rhs.Width &&
		lhs.Height == rhs.Height &&
		lhs.DepthOrArraySize == rhs.DepthOrArraySize &&
		lhs.MipLevels == rhs.MipLevels &&
		lhs.Format == rhs.Format &&
		lhs.SampleDesc == rhs.SampleDesc &&
		lhs.Layout == rhs.Layout &&
		lhs.Flags == rhs.Flags;
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
					cl->m_d3dCmdList->Reset(cl->m_cmdAllocator.get(), nullptr);
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
		IID_PPV_ARGS(m_d3dCmdList.put())));

	AssertIfFailed(GetDevice()->CreateFence(
		0,
		D3D12_FENCE_FLAG_NONE,
		IID_PPV_ARGS(m_fence.put())));
}

void FCommandList::SetName(const std::wstring& name)
{
	m_name = name;
	m_d3dCmdList->SetName(name.c_str());
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
	const std::vector<D3D12_SUBRESOURCE_DATA>& srcData,
	std::function<void(FCommandList*)> transition)
{
	// NOTE layout.Footprint.RowPitch is the D3D12 aligned pitch whereas rowSizeInBytes is the unaligned pitch
	UINT64 totalBytes = 0;
	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(srcData.size());
	std::vector<UINT64> rowSizeInBytes(srcData.size());
	std::vector<UINT> numRows(srcData.size());

	D3D12_RESOURCE_DESC destinationDesc = destinationResource->GetDesc();
	GetDevice()->GetCopyableFootprints(&destinationDesc, 0, srcData.size(), m_currentOffset, layouts.data(), numRows.data(), rowSizeInBytes.data(), &totalBytes);

	size_t capacity = m_sizeInBytes - m_currentOffset;
	DebugAssert(totalBytes <= capacity, "Upload buffer is too small!");

	for (UINT i = 0; i < srcData.size(); ++i)
	{
		D3D12_SUBRESOURCE_DATA src;
		src.pData = srcData[i].pData;
		src.RowPitch = srcData[i].RowPitch;
		src.SlicePitch = srcData[i].SlicePitch;

		D3D12_MEMCPY_DEST dest;
		dest.pData = m_mappedPtr + layouts[i].Offset;
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
		m_copyCommandlist->m_d3dCmdList->CopyBufferRegion(
			destinationResource,
			0,
			m_uploadBuffer->m_d3dResource,
			layouts[0].Offset,
			layouts[0].Footprint.Width);
	}
	else
	{
		for (UINT i = 0; i < srcData.size(); ++i)
		{
			D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
			srcLocation.pResource = m_uploadBuffer->m_d3dResource;
			srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			srcLocation.PlacedFootprint = layouts[i];

			D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
			dstLocation.pResource = destinationResource;
			dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dstLocation.SubresourceIndex = i;

			m_copyCommandlist->m_d3dCmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
		}
	}

	m_currentOffset += totalBytes;
	m_pendingTransitions.push_back(transition);
}

D3DFence_t* FResourceUploadContext::SubmitUploads(FCommandList* owningCL)
{
	ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_COPY, { m_copyCommandlist });

	GetUploadBufferPool()->Retire(m_uploadBuffer, m_copyCommandlist);

	// Transition all the destination resources on the owning/direct CL since the copy CLs have limited transition capabilities
	// Wait for the copy to finish before doing the transitions.
	D3DCommandQueue_t* queue = owningCL->m_type == D3D12_COMMAND_LIST_TYPE_DIRECT ? GetGraphicsQueue() : GetComputeQueue();
	queue->Wait(m_copyCommandlist->m_fence.get(), m_copyCommandlist->m_fenceValue);
	for (auto& transitionCallback : m_pendingTransitions)
	{
		transitionCallback(owningCL);
	}

	return m_copyCommandlist->m_fence.get();
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

HRESULT FResource::InitCommittedResource(
	const std::wstring& name, 
	const D3D12_HEAP_PROPERTIES& heapProperties, 
	const D3D12_RESOURCE_DESC& resourceDesc, 
	const D3D12_RESOURCE_STATES initialState,
	const D3D12_CLEAR_VALUE* clearValue)
{
	HRESULT hr = GetDevice()->CreateCommittedResource(
		&heapProperties, 
		D3D12_HEAP_FLAG_NONE, 
		&resourceDesc,
		initialState, 
		clearValue,
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

	// Check if there are any pending transitions for this resource on the same commandlist.
	// If yes, then update the before state immediately for the current transition call.
	auto pendingResourceTransition = cmdList->m_pendingTransitions.find(this);
	if (pendingResourceTransition != cmdList->m_pendingTransitions.cend())
	{
		pendingResourceTransition->second();
		cmdList->m_pendingTransitions.erase(pendingResourceTransition);
	}

	D3D12_RESOURCE_STATES beforeState = m_subresourceStates[subId];
	if (beforeState == destState)
		return;

	D3D12_RESOURCE_BARRIER barrierDesc = {};
	barrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrierDesc.Transition.pResource = m_d3dResource;
	barrierDesc.Transition.StateBefore = beforeState;
	barrierDesc.Transition.StateAfter = destState;
	barrierDesc.Transition.Subresource = subresourceIndex;
	cmdList->m_d3dCmdList->ResourceBarrier(1, &barrierDesc);

	// Update CPU side tracking of current state
	cmdList->m_pendingTransitions.emplace(
		this,
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
//														Bindless
//-----------------------------------------------------------------------------------------------------------------------------------------------

class FBindlessIndexPool
{
public:
	void Initialize(D3DDescriptorHeap_t* bindlessHeap)
	{
		// Initialize with null descriptors and cache available indices

		// Texture2D
		D3D12_SHADER_RESOURCE_VIEW_DESC nullTex2DDesc = GetNullSRVDesc(D3D12_SRV_DIMENSION_TEXTURE2D);
		for (int i = (uint32_t)BindlessDescriptorRange::Texture2DBegin; i <= (uint32_t)BindlessDescriptorRange::Texture2DEnd; ++i)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE srv;
			srv.ptr = bindlessHeap->GetCPUDescriptorHandleForHeapStart().ptr +
				i * GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			GetDevice()->CreateShaderResourceView(nullptr, &nullTex2DDesc, srv);

			m_indices[(uint32_t)BindlessResourceType::Texture2D].push(i);
		}

		// Buffer
		D3D12_SHADER_RESOURCE_VIEW_DESC nullBufferDesc = GetNullSRVDesc(D3D12_SRV_DIMENSION_BUFFER);
		for (int i = (uint32_t)BindlessDescriptorRange::BufferBegin; i <= (uint32_t)BindlessDescriptorRange::BufferEnd; ++i)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE srv;
			srv.ptr = bindlessHeap->GetCPUDescriptorHandleForHeapStart().ptr +
				i * GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			GetDevice()->CreateShaderResourceView(nullptr, &nullBufferDesc, srv);

			m_indices[(uint32_t)BindlessResourceType::Buffer].push(i);
		}

		// Texture Cube
		D3D12_SHADER_RESOURCE_VIEW_DESC nullTexCubeDesc = GetNullSRVDesc(D3D12_SRV_DIMENSION_TEXTURECUBE);
		for (int i = (uint32_t)BindlessDescriptorRange::TextureCubeBegin; i <= (uint32_t)BindlessDescriptorRange::TextureCubeEnd; ++i)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE srv;
			srv.ptr = bindlessHeap->GetCPUDescriptorHandleForHeapStart().ptr +
				i * GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			GetDevice()->CreateShaderResourceView(nullptr, &nullTexCubeDesc, srv);

			m_indices[(uint32_t)BindlessResourceType::TextureCube].push(i);
		}

		// RWTexture2D
		D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav2DDesc = GetNullUavDesc(D3D12_UAV_DIMENSION_TEXTURE2D);
		for (int i = (uint32_t)BindlessDescriptorRange::RWTexture2DBegin; i <= (uint32_t)BindlessDescriptorRange::RWTexture2DEnd; ++i)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE uav;
			uav.ptr = bindlessHeap->GetCPUDescriptorHandleForHeapStart().ptr +
				i * GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			GetDevice()->CreateUnorderedAccessView(nullptr, nullptr, &nullUav2DDesc, uav);

			m_indices[(uint32_t)BindlessResourceType::RWTexture2D].push(i);
		}

		// RWTexture2DArray
		D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav2DArrayDesc = GetNullUavDesc(D3D12_UAV_DIMENSION_TEXTURE2DARRAY);
		for (int i = (uint32_t)BindlessDescriptorRange::RWTexture2DArrayBegin; i <= (uint32_t)BindlessDescriptorRange::RWTexture2DArrayEnd; ++i)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE uav;
			uav.ptr = bindlessHeap->GetCPUDescriptorHandleForHeapStart().ptr +
				i * GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			GetDevice()->CreateUnorderedAccessView(nullptr, nullptr, &nullUav2DArrayDesc, uav);

			m_indices[(uint32_t)BindlessResourceType::RWTexture2DArray].push(i);
		}
	}

	uint32_t FetchIndex(BindlessResourceType type)
	{
		uint32_t index;
		bool ok = m_indices[(uint32_t)type].try_pop(index);
		DebugAssert(ok, "Ran out of bindless descriptors");

		return index;
	}

	void ReturnIndex(uint32_t index)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE descriptor = GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, index);

		if (index <= (uint32_t)BindlessDescriptorRange::BufferEnd)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC nullBufferDesc = GetNullSRVDesc(D3D12_SRV_DIMENSION_BUFFER);
			GetDevice()->CreateShaderResourceView(nullptr, &nullBufferDesc, descriptor);
			m_indices[(uint32_t)BindlessResourceType::Buffer].push(index);
		}
		else if (index <= (uint32_t)BindlessDescriptorRange::Texture2DEnd)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC nullTex2DDesc = GetNullSRVDesc(D3D12_SRV_DIMENSION_TEXTURE2D);
			GetDevice()->CreateShaderResourceView(nullptr, &nullTex2DDesc, descriptor);
			m_indices[(uint32_t)BindlessResourceType::Texture2D].push(index);
		}
		else if (index <= (uint32_t)BindlessDescriptorRange::TextureCubeEnd)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC nullTexCubeDesc = GetNullSRVDesc(D3D12_SRV_DIMENSION_TEXTURECUBE);
			GetDevice()->CreateShaderResourceView(nullptr, &nullTexCubeDesc, descriptor);
			m_indices[(uint32_t)BindlessResourceType::TextureCube].push(index);
		}
		else if (index <= (uint32_t)BindlessDescriptorRange::RWTexture2DEnd)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav2DDesc = GetNullUavDesc(D3D12_UAV_DIMENSION_TEXTURE2D);
			GetDevice()->CreateUnorderedAccessView(nullptr, nullptr, &nullUav2DDesc, descriptor);
			m_indices[(uint32_t)BindlessResourceType::RWTexture2D].push(index);
		}
		else if (index <= (uint32_t)BindlessDescriptorRange::RWTexture2DArrayEnd)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav2DArrayDesc = GetNullUavDesc(D3D12_UAV_DIMENSION_TEXTURE2DARRAY);
			GetDevice()->CreateUnorderedAccessView(nullptr, nullptr, &nullUav2DArrayDesc, descriptor);
			m_indices[(uint32_t)BindlessResourceType::RWTexture2DArray].push(index);
		}
		else
		{
			DebugAssert(false, "Unsupported");
		}
	}

	void Clear()
	{
		for (int i = 0; i < (uint32_t)BindlessResourceType::Count; ++i)
		{
			m_indices[i].clear();
		}
	}

private:
	concurrency::concurrent_queue<uint32_t> m_indices[(uint32_t)BindlessResourceType::Count];
};

FBindlessResource::~FBindlessResource()
{
	delete m_resource;

	if (m_srvIndex != ~0u)
	{
		GetBindlessPool()->ReturnIndex(m_srvIndex);
	}

	if (!m_uavIndices.empty())
	{
		for (const uint32_t uavIndex : m_uavIndices)
		{
			GetBindlessPool()->ReturnIndex(uavIndex);
		}
	}
}


//-----------------------------------------------------------------------------------------------------------------------------------------------
//														Render Textures
//-----------------------------------------------------------------------------------------------------------------------------------------------

class FRenderTexturePool
{
public:
	void Initialize(size_t sizeInBytes)
	{
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

		AssertIfFailed(GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.put())));
	}

	FResource* GetOrCreate(const std::wstring& name, const D3D12_RESOURCE_DESC& desc, const D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE& clearValue)
	{
		const std::lock_guard<std::mutex> lock(m_mutex);

		// Reuse buffer
		for (auto it = m_freeList.begin(); it != m_freeList.end(); ++it)
		{
			if (desc == (*it)->m_d3dResource->GetDesc())
			{
				m_useList.push_back(std::move(*it));
				m_freeList.erase(it);

				FResource* rt = m_useList.back().get();
				rt->SetName(name.c_str());
				return rt;
			}
		}

		D3D12_HEAP_PROPERTIES heapDesc = {};
		heapDesc.Type = D3D12_HEAP_TYPE_DEFAULT;
		heapDesc.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		// New render texture
		auto newRt = std::make_unique<FResource>();
		AssertIfFailed(newRt->InitCommittedResource(name, heapDesc, desc, initialState, &clearValue));

		m_useList.push_back(std::move(newRt));
		return m_useList.back().get();
	}

	void Retire(const FRenderTexture* rt)
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

		auto addToFreePool = [this, resource = rt->m_resource, depthStencil = rt->m_isDepthStencil, textureIndices = std::move(rt->m_renderTextureIndices)]() mutable
		{
			const std::lock_guard<std::mutex> lock(m_mutex);

			concurrency::concurrent_queue<uint32_t>& descriptorIndexPool = depthStencil ? GetDSVIndexPool() : GetRTVIndexPool();
			for (uint32_t descriptorIndex : textureIndices)
			{
				descriptorIndexPool.push(descriptorIndex);
			}

			for (auto it = m_useList.begin(); it != m_useList.end();)
			{
				if (it->get() == resource)
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

	D3DHeap_t* GetHeap()
	{
		return m_heap.get();
	}

	void Clear()
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		DebugAssert(m_useList.empty(), "All render textures should be retired at this point");
		m_freeList.clear();
	}

private:
	std::mutex m_mutex;
	winrt::com_ptr<D3DHeap_t> m_heap;
	winrt::com_ptr<D3DFence_t> m_fence;
	size_t m_fenceValue = 0;
	std::list<std::unique_ptr<FResource>> m_freeList;
	std::list<std::unique_ptr<FResource>> m_useList;
};

FRenderTexture::~FRenderTexture()
{
	GetRenderTexturePool()->Retire(this);
}

void FRenderTexture::Transition(FCommandList* cmdList, const uint32_t subresourceIndex, const D3D12_RESOURCE_STATES destState)
{
	if (m_srvIndex == ~0u && IsShaderResource(destState))
	{
		m_srvIndex = GetBindlessPool()->FetchIndex(BindlessResourceType::Texture2D);
		D3D12_CPU_DESCRIPTOR_HANDLE srv = GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_srvIndex);
		GetDevice()->CreateShaderResourceView(m_resource->m_d3dResource, &m_srvDesc, srv);
		m_resource->Transition(cmdList, subresourceIndex, destState);
	}
	else if (m_srvIndex != ~0u && 
		(destState == D3D12_RESOURCE_STATE_RENDER_TARGET || destState == D3D12_RESOURCE_STATE_DEPTH_WRITE || destState == D3D12_RESOURCE_STATE_DEPTH_READ))
	{
		GetBindlessPool()->ReturnIndex(m_srvIndex);
		m_srvIndex = ~0u;
		m_resource->Transition(cmdList, subresourceIndex, destState);
	}
	else
	{
		m_resource->Transition(cmdList, subresourceIndex, destState);
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
	winrt::com_ptr<IDXGraphicsAnalysis> s_graphicsAnalysis;
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
	FBindlessIndexPool s_bindlessPool;

	concurrency::concurrent_unordered_map<FShaderDesc, FTimestampedBlob> s_shaderCache;
	concurrency::concurrent_unordered_map<FRootsigDesc, FTimestampedBlob> s_rootsigCache;
	concurrency::concurrent_unordered_map<D3D12_GRAPHICS_PIPELINE_STATE_DESC, winrt::com_ptr<D3DPipelineState_t>> s_graphicsPSOPool;
	concurrency::concurrent_unordered_map<D3D12_COMPUTE_PIPELINE_STATE_DESC, winrt::com_ptr<D3DPipelineState_t>> s_computePSOPool;
	concurrency::concurrent_queue<uint32_t> s_rtvIndexPool;
	concurrency::concurrent_queue<uint32_t> s_dsvIndexPool;
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

	D3DDescriptorHeap_t* GetDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_TYPE type)
	{
		return RenderBackend12::s_descriptorHeaps[type].get();
	}

	uint32_t GetDescriptorSize(const D3D12_DESCRIPTOR_HEAP_TYPE type)
	{
		return RenderBackend12::s_descriptorSize[type];
	}

	FUploadBufferPool* GetUploadBufferPool()
	{
		return &RenderBackend12::s_uploadBufferPool;
	}

	FRenderTexturePool* GetRenderTexturePool()
	{
		return &RenderBackend12::s_renderTexturePool;
	}

	FBindlessIndexPool* GetBindlessPool()
	{
		return &RenderBackend12::s_bindlessPool;
	}

	concurrency::concurrent_queue<uint32_t>& GetRTVIndexPool()
	{
		return RenderBackend12::s_rtvIndexPool;
	}

	concurrency::concurrent_queue<uint32_t>& GetDSVIndexPool()
	{
		return RenderBackend12::s_dsvIndexPool;
	}
}

bool RenderBackend12::Initialize(const HWND& windowHandle, const uint32_t resX, const uint32_t resY)
{
	UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
	// Debug Layer
	AssertIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(s_debugController.put())));
	s_debugController->EnableDebugLayer();
	dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;

	// Programmatic Capture
	DXGIGetDebugInterface1(0, IID_PPV_ARGS(s_graphicsAnalysis.put()));
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
		cbvSrvUavHeapDesc.NumDescriptors = (uint32_t)BindlessDescriptorRange::TotalCount;
		cbvSrvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		cbvSrvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		AssertIfFailed(
			s_d3dDevice->CreateDescriptorHeap(
				&cbvSrvUavHeapDesc,
				IID_PPV_ARGS(s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].put()))
		);
		s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV]->SetName(L"bindless_descriptor_heap");

		s_bindlessPool.Initialize(s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].get());
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

	// DSV heap
	{
		D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
		dsvHeapDesc.NumDescriptors = k_dsvHeapSize;
		dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		AssertIfFailed(
			s_d3dDevice->CreateDescriptorHeap(
				&dsvHeapDesc,
				IID_PPV_ARGS(s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_DSV].put()))
		);
		s_descriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_DSV]->SetName(L"dsv_heap");

		// Cache available indices
		for (int i = 0; i < k_dsvHeapSize; ++i)
		{
			s_dsvIndexPool.push(i);
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
	AssertIfFailed(s_dxgiFactory->CreateSwapChainForHwnd(
			s_graphicsQueue.get(),
			windowHandle,
			&swapChainDesc,
			nullptr,
			nullptr,
			swapChain.put()));

	AssertIfFailed(swapChain->QueryInterface(IID_PPV_ARGS(s_swapChain.put())));

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

		D3D12_CPU_DESCRIPTOR_HANDLE rtvDescriptor = GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvIndex);
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
void RenderBackend12::FlushGPU()
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
}


void RenderBackend12::Teardown()
{
	MicroProfileGpuShutdown();

	s_commandListPool.Clear();
	s_uploadBufferPool.Clear();
	s_renderTexturePool.Clear();
	s_bindlessPool.Clear();

	s_shaderCache.clear();
	s_rootsigCache.clear();
	s_graphicsPSOPool.clear();
	s_computePSOPool.clear();
	s_rtvIndexPool.clear();
	s_dsvIndexPool.clear();

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
	FILETIME currentTimestamp = ShaderCompiler::GetLastModifiedTime(shaderDesc.m_filename);

	auto search = s_shaderCache.find(shaderDesc);
	if (search != s_shaderCache.cend())
	{
		if (search->second.m_timestamp != currentTimestamp &&
			SUCCEEDED(ShaderCompiler::CompileShader(
				shaderDesc.m_filename,
				shaderDesc.m_entrypoint,
				shaderDesc.m_defines,
				profile,
				search->second.m_blob.put())))
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
		AssertIfFailed(ShaderCompiler::CompileShader(
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
	FILETIME currentTimestamp = ShaderCompiler::GetLastModifiedTime(rootsigDesc.m_filename);

	auto search = s_rootsigCache.find(rootsigDesc);
	if (search != s_rootsigCache.cend())
	{
		if (search->second.m_timestamp != currentTimestamp &&
			ShaderCompiler::CompileRootsignature(
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
		AssertIfFailed(ShaderCompiler::CompileRootsignature(
			rootsigDesc.m_filename,
			rootsigDesc.m_entrypoint,
			profile,
			rsBlob.m_blob.put()));

		rsBlob.m_timestamp = currentTimestamp;
		return rsBlob.m_blob.get();
	}
}

winrt::com_ptr<D3DRootSignature_t> RenderBackend12::FetchRootSignature(const FRootsigDesc& rootsig)
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
	D3D12_CPU_DESCRIPTOR_HANDLE descriptor = GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, s_currentBufferIndex);
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
		D3DCommandList_t* d3dCL = cl->m_d3dCmdList.get();
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
		for (auto&& [resource, transitionProc] : cl->m_pendingTransitions)
		{
			transitionProc();
		}

		cl->m_pendingTransitions.clear();

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
	return GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE descriptorHeapType, uint32_t descriptorIndex)
{
	D3D12_CPU_DESCRIPTOR_HANDLE descriptor;
	descriptor.ptr = GetDescriptorHeap(descriptorHeapType)->GetCPUDescriptorHandleForHeapStart().ptr +
		descriptorIndex * GetDescriptorSize(descriptorHeapType);

	return descriptor;
}

D3D12_GPU_DESCRIPTOR_HANDLE RenderBackend12::GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE descriptorHeapType, uint32_t descriptorIndex)
{
	D3D12_GPU_DESCRIPTOR_HANDLE descriptor;
	descriptor.ptr = GetDescriptorHeap(descriptorHeapType)->GetGPUDescriptorHandleForHeapStart().ptr +
		descriptorIndex * GetDescriptorSize(descriptorHeapType);

	return descriptor;
}

uint32_t RenderBackend12::GetDescriptorTableOffset(BindlessDescriptorType descriptorType, uint32_t descriptorIndex)
{
	uint32_t offset = 0;

	switch (descriptorType)
	{
	case BindlessDescriptorType::Buffer:
		offset = descriptorIndex - (uint32_t)BindlessDescriptorRange::BufferBegin;
		DebugAssert(offset <= (uint32_t)BindlessDescriptorRange::BufferEnd);
		return offset;
	case BindlessDescriptorType::Texture2D:
		offset = descriptorIndex - (uint32_t)BindlessDescriptorRange::Texture2DBegin;
		DebugAssert(offset <= (uint32_t)BindlessDescriptorRange::Texture2DEnd);
		return offset;
	case BindlessDescriptorType::TextureCube:
		offset = descriptorIndex - (uint32_t)BindlessDescriptorRange::TextureCubeBegin;
		DebugAssert(offset <= (uint32_t)BindlessDescriptorRange::TextureCubeEnd);
		return offset;
	case BindlessDescriptorType::RWTexture2D:
		offset = descriptorIndex - (uint32_t)BindlessDescriptorRange::RWTexture2DBegin;
		DebugAssert(offset <= (uint32_t)BindlessDescriptorRange::RWTexture2DEnd);
		return offset;
	case BindlessDescriptorType::RWTexture2DArray:
		offset = descriptorIndex - (uint32_t)BindlessDescriptorRange::RWTexture2DArrayBegin;
		DebugAssert(offset <= (uint32_t)BindlessDescriptorRange::RWTexture2DArrayEnd);
		return offset;
	default:
		DebugAssert("Not Implemented");
		return offset;
	}
}

std::unique_ptr<FTransientBuffer> RenderBackend12::CreateTransientBuffer(
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

	auto tempBuffer = std::make_unique<FTransientBuffer>();
	tempBuffer->m_resource = buffer;
	tempBuffer->m_dependentCmdlist = dependentCL;

	return std::move(tempBuffer);
}

std::unique_ptr<FRenderTexture> RenderBackend12::CreateRenderTexture(
	const std::wstring& name,
	const DXGI_FORMAT format,
	const size_t width,
	const size_t height,
	const size_t mipLevels,
	const size_t depth,
	const size_t sampleCount)
{

	D3D12_RESOURCE_DESC rtDesc = {};
	rtDesc.Dimension = depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rtDesc.Width = width;
	rtDesc.Height = (UINT)height;
	rtDesc.DepthOrArraySize = (UINT16)depth;
	rtDesc.MipLevels = (UINT16)mipLevels;
	rtDesc.Format = format;
	rtDesc.SampleDesc.Count = sampleCount;
	rtDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	D3D12_CLEAR_VALUE clearValue = {};
	clearValue.Format = format;
	clearValue.Color[0] = clearValue.Color[1] = clearValue.Color[2] = clearValue.Color[3] = 0.f;

	FResource* rtResource = s_renderTexturePool.GetOrCreate(name, rtDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, clearValue);

	// RTV Descriptor
	std::vector<uint32_t> rtvIndices;
	for(int mip = 0; mip < mipLevels; ++mip)
	{
		uint32_t rtvIndex;
		bool ok = s_rtvIndexPool.try_pop(rtvIndex);
		DebugAssert(ok, "Ran out of RTV descriptors");
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvIndex);

		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = format;
		if (depth == 1)
		{
			if (sampleCount > 1)
			{
				rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
			}
			else
			{
				rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
				rtvDesc.Texture2D.MipSlice = mip;
			}
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
			if (sampleCount > 1)
			{
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
			}
			else
			{
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MipLevels = mipLevels;
				srvDesc.Texture2D.MostDetailedMip = 0;
			}
		}
		else
		{
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
			srvDesc.Texture3D.MipLevels = mipLevels;
			srvDesc.Texture3D.MostDetailedMip = 0;
		}
	}

	auto rt = std::make_unique<FRenderTexture>();
	rt->m_resource = rtResource;
	rt->m_renderTextureIndices = std::move(rtvIndices);
	rt->m_srvDesc = std::move(srvDesc);
	rt->m_isDepthStencil = false;

	return std::move(rt);
}

std::unique_ptr<FRenderTexture> RenderBackend12::CreateDepthStencilTexture(
	const std::wstring& name,
	const DXGI_FORMAT format,
	const size_t width,
	const size_t height,
	const size_t mipLevels,
	const size_t sampleCount)
{
	D3D12_RESOURCE_DESC dsDesc = {};
	dsDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	dsDesc.Width = width;
	dsDesc.Height = (UINT)height;
	dsDesc.DepthOrArraySize = 1;
	dsDesc.MipLevels = (UINT16)mipLevels;
	dsDesc.Format = GetTypelessDepthStencilFormat(format);
	dsDesc.SampleDesc.Count = sampleCount;
	dsDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	dsDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE clearValue = {};
	clearValue.Format = format;
	clearValue.DepthStencil.Depth = 0.f;
	clearValue.DepthStencil.Stencil = 0;

	FResource* rtResource = s_renderTexturePool.GetOrCreate(name, dsDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, clearValue);

	// DSV Descriptor
	std::vector<uint32_t> dsvIndices;
	for (int mip = 0; mip < mipLevels; ++mip)
	{
		uint32_t dsvIndex;
		bool ok = s_dsvIndexPool.try_pop(dsvIndex);
		DebugAssert(ok, "Ran out of DSV descriptors");
		D3D12_CPU_DESCRIPTOR_HANDLE dsv = GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsvIndex);

		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
		dsvDesc.Format = format;

		if (sampleCount > 1)
		{
			dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
		}
		else
		{
			dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			dsvDesc.Texture2D.MipSlice = mip;
		}

		GetDevice()->CreateDepthStencilView(rtResource->m_d3dResource, &dsvDesc, dsv);
		dsvIndices.push_back(dsvIndex);
	}

	// Cache SRV Description
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = GetSrvDepthFormat(format);

	if (sampleCount > 1)
	{
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
	}
	else
	{
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = mipLevels;
		srvDesc.Texture2D.MostDetailedMip = 0;
	}

	auto rt = std::make_unique<FRenderTexture>();
	rt->m_resource = rtResource;
	rt->m_renderTextureIndices = std::move(dsvIndices);
	rt->m_srvDesc = std::move(srvDesc);
	rt->m_isDepthStencil = true;


	return std::move(rt);
}

std::unique_ptr<FBindlessResource> RenderBackend12::CreateBindlessTexture(
	const std::wstring& name,
	const DXGI_FORMAT format,
	const size_t width,
	const size_t height,
	const DirectX::Image* images,
	const size_t imageCount,
	D3D12_RESOURCE_STATES resourceState,
	FResourceUploadContext* uploadContext)
{
	auto newTexture = std::make_unique<FBindlessResource>();

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
		desc.MipLevels = imageCount;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		newTexture->m_resource = new FResource;
		AssertIfFailed(newTexture->m_resource->InitCommittedResource(name, props, desc, D3D12_RESOURCE_STATE_COPY_DEST));
	}

	// Upload texture data
	{
		std::vector<D3D12_SUBRESOURCE_DATA> srcData(imageCount);
		for(int mipIndex = 0; mipIndex < imageCount; ++mipIndex)
		{
			srcData[mipIndex].pData = images[mipIndex].pixels;
			srcData[mipIndex].RowPitch = images[mipIndex].rowPitch;
			srcData[mipIndex].SlicePitch = images[mipIndex].slicePitch;
		}

		uploadContext->UpdateSubresources(
			newTexture->m_resource->m_d3dResource,
			srcData,
			[resource = newTexture->m_resource, resourceState](FCommandList* cmdList)
			{
				resource->Transition(cmdList, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, resourceState);
			});
	}

	// Descriptor
	{
		newTexture->m_srvIndex = GetBindlessPool()->FetchIndex(BindlessResourceType::Texture2D);
		D3D12_CPU_DESCRIPTOR_HANDLE srv = GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, newTexture->m_srvIndex);

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = imageCount;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		GetDevice()->CreateShaderResourceView(newTexture->m_resource->m_d3dResource, &srvDesc, srv);
	}

	return std::move(newTexture);
}

std::unique_ptr<FBindlessResource> RenderBackend12::CreateBindlessByteAddressBuffer(
	const std::wstring& name,
	const size_t size,
	const uint8_t* pData,
	D3D12_RESOURCE_STATES resourceState,
	FResourceUploadContext* uploadContext)
{
	auto newBuffer = std::make_unique<FBindlessResource>();

	// Create resource
	{
		D3D12_HEAP_PROPERTIES heapProps = {};
		heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
		heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Alignment = 0;
		desc.Width = size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		newBuffer->m_resource = new FResource;
		AssertIfFailed(newBuffer->m_resource->InitCommittedResource(name, heapProps, desc, D3D12_RESOURCE_STATE_COPY_DEST));
	}

	// Upload buffer data
	{
		std::vector<D3D12_SUBRESOURCE_DATA> srcData(1);
		srcData[0].pData = pData;
		srcData[0].RowPitch = size;
		srcData[0].SlicePitch = size;
		uploadContext->UpdateSubresources(
			newBuffer->m_resource->m_d3dResource,
			srcData,
			[resource = newBuffer->m_resource, resourceState](FCommandList* cmdList)
			{
				resource->Transition(cmdList, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, resourceState);
			});
	}

	// Descriptor
	{
		newBuffer->m_srvIndex = GetBindlessPool()->FetchIndex(BindlessResourceType::Buffer);
		D3D12_CPU_DESCRIPTOR_HANDLE srv = GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, newBuffer->m_srvIndex);

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = size / 4; // number of R32 elements
		srvDesc.Buffer.StructureByteStride = 0;
		srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		GetDevice()->CreateShaderResourceView(newBuffer->m_resource->m_d3dResource, &srvDesc, srv);
	}

	return std::move(newBuffer);
}

std::unique_ptr<FBindlessResource> RenderBackend12::CreateBindlessUavTexture(
	const std::wstring& name,
	const DXGI_FORMAT format,
	const size_t width,
	const size_t height,
	const size_t mipLevels,
	const size_t arraySize)
{
	auto newUav = std::make_unique<FBindlessResource>();

	// Create resource
	{
		D3D12_HEAP_PROPERTIES heapProps = {};
		heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
		heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC uavDesc = {};
		uavDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		uavDesc.Width = width;
		uavDesc.Height = (UINT)height;
		uavDesc.DepthOrArraySize = (UINT16)arraySize;
		uavDesc.MipLevels = mipLevels;
		uavDesc.Format = format;
		uavDesc.SampleDesc.Count = 1;
		uavDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		uavDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		newUav->m_resource = new FResource;
		AssertIfFailed(newUav->m_resource->InitCommittedResource(name, heapProps, uavDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	}

	// Descriptor(s)
	for(int mipIndex = 0; mipIndex < mipLevels; ++mipIndex)
	{
		uint32_t uavIndex = GetBindlessPool()->FetchIndex(arraySize > 1 ? BindlessResourceType::RWTexture2DArray : BindlessResourceType::RWTexture2D);
		D3D12_CPU_DESCRIPTOR_HANDLE uav = GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, uavIndex);

		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = format;

		if (arraySize > 1)
		{
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
			uavDesc.Texture2DArray.MipSlice = mipIndex;
			uavDesc.Texture2DArray.FirstArraySlice = 0;
			uavDesc.Texture2DArray.ArraySize = arraySize;
			uavDesc.Texture2DArray.PlaneSlice = 0;
		}
		else
		{
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = mipIndex;
			uavDesc.Texture2D.PlaneSlice = 0;
		}
	
		GetDevice()->CreateUnorderedAccessView(newUav->m_resource->m_d3dResource, nullptr, &uavDesc, uav);
		newUav->m_uavIndices.push_back(uavIndex);
	}

	return std::move(newUav);
}

void RenderBackend12::BeginCapture()
{
	if (s_graphicsAnalysis)
	{
		s_graphicsAnalysis->BeginCapture();
	}
}

void RenderBackend12::EndCapture()
{
	if (s_graphicsAnalysis)
	{
		s_graphicsAnalysis->EndCapture();
	}
}