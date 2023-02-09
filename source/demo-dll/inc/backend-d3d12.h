#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <DirectXTex.h>
#include <winrt/base.h>
#include <pix3.h>
#include <vector>
#include <string>
#include <functional>
#include <optional>
#include <concurrent_vector.h>

// Aliased types
using DXGIFactory_t = IDXGIFactory4;
using DXGIAdapter_t = IDXGIAdapter;
using DXGISwapChain_t = IDXGISwapChain3;
using D3DDebug_t = ID3D12Debug5;
using D3DInfoQueue_t = ID3D12InfoQueue;
using D3DDredSettings_t = ID3D12DeviceRemovedExtendedDataSettings;
using D3DDevice_t = ID3D12Device5;
using D3DCommandQueue_t = ID3D12CommandQueue;
using D3DCommandAllocator_t = ID3D12CommandAllocator;
using D3DDescriptorHeap_t = ID3D12DescriptorHeap;
using D3DHeap_t = ID3D12Heap;
using D3DResource_t = ID3D12Resource;
using D3DCommandList_t = ID3D12GraphicsCommandList4;
using D3DFence_t = ID3D12Fence1;
using D3DPipelineState_t = ID3D12PipelineState;
using D3DRootSignature_t = ID3D12RootSignature;
using D3DStateObject_t = ID3D12StateObject;
using D3DStateObjectProperties_t = ID3D12StateObjectProperties;
using D3DCommandSignature_t = ID3D12CommandSignature;
using PhysicalAlloc_t = std::vector<uint32_t>;

struct IDxcBlob;
struct FResource;
struct FConfig;

enum class ResourceType
{
	Buffer,
	Texture2D,
	Texture2DMultisample,
	Texture2DArray,
	TextureCube,
	Texture3D,
	RWTexture2D,
	RWTexture2DArray,
	AccelerationStructure,
	Count
};

enum class ResourceAllocationType
{
	Committed,
	Pooled
};

enum class DescriptorType
{
	Buffer,
	Texture2D,
	Texture2DMultisample,
	Texture2DArray,
	TextureCube,
	RWTexture2D,
	RWTexture2DArray,
	AccelerationStructure
};

enum class DescriptorRange : uint32_t
{
	// 0-99 is reserved for special descriptors. See gpu-shared-types.h
	BufferBegin = 100,
	BufferEnd = BufferBegin + 4999,
	Texture2DBegin,
	Texture2DEnd = Texture2DBegin + 4999,
	Texture2DMultisampleBegin,
	Texture2DMultisampleEnd = Texture2DMultisampleBegin + 999,
	Texture2DArrayBegin,
	Texture2DArrayEnd = Texture2DArrayBegin + 999,
	TextureCubeBegin,
	TextureCubeEnd = TextureCubeBegin + 999,
	RWTexture2DBegin,
	RWTexture2DEnd = RWTexture2DBegin + 999,
	RWTexture2DArrayBegin,
	RWTexture2DArrayEnd = RWTexture2DArrayBegin + 999,
	AccelerationStructureBegin,
	AccelerationStructureEnd = AccelerationStructureBegin + 4999,
	TotalCount
};

// Not using enum class here so that we can easily pass in OR'd values from call sites to create surfaces
enum SurfaceType : uint32_t
{
	RenderTarget	= (1 << 0),
	DepthStencil	= (1 << 1),
	SwapChain		= (1 << 2),
	UAV				= (1 << 3)
};

enum class BufferType
{
	Raw,
	AccelerationStructure
};

enum class TextureType
{
	Tex2D,
	Tex2DArray,
	TexCube
};

enum class ResourceAccessMode
{
	GpuReadOnly,
	GpuReadWrite,
	GpuWriteOnly,	// Used for acceleration structure scratch buffers
	CpuWriteOnly,	// Upload resources
	CpuReadOnly,	// Readback resources
};

enum class SyncFence
{
	CpuSubmission,
	GpuBegin,
	GpuFinish
};

struct FFenceMarker
{
	D3DFence_t* m_fence;
	size_t m_value;

	FFenceMarker() = default;
	FFenceMarker(D3DFence_t* fence, const size_t value) :
		m_fence{ fence }, m_value{ value }{}

	// CPU sync
	void Signal() const;
	void Wait() const;

	// GPU sync
	void Signal(D3DCommandQueue_t* cmdQueue) const;
	void Wait(D3DCommandQueue_t* cmdQueue) const;
};

struct ResourceAllocation
{
	// Specifies pooled or committed resource type
	ResourceAllocationType m_type;

	// Pooled resources must specify lifetime via fence marker
	FFenceMarker m_lifetime;

	static ResourceAllocation Committed()
	{
		return { ResourceAllocationType::Committed };
	}

	static ResourceAllocation Pooled(const FFenceMarker fence)
	{
		return { ResourceAllocationType::Pooled, fence };
	}
};

struct FCommandList
{
	D3D12_COMMAND_LIST_TYPE m_type;
	std::wstring m_name;
	size_t m_fenceValues[3];
	winrt::com_ptr<D3DCommandList_t> m_d3dCmdList;
	winrt::com_ptr<D3DCommandAllocator_t> m_cmdAllocator;
	winrt::com_ptr<D3DFence_t> m_fence[3];
	std::vector<std::function<void(void)>> m_postExecuteCallbacks;

	FCommandList() = default;
	FCommandList(const D3D12_COMMAND_LIST_TYPE type);
	void ResetFence(const size_t fenceValue);
	void SetName(const std::wstring& name);
	FFenceMarker GetFence(const SyncFence type) const;
};

// Saves the capture to a file named PIXGpuCapture.wpix in the binaries directory
struct FScopedGpuCapture
{
	FScopedGpuCapture() = delete;
	FScopedGpuCapture(FCommandList* cl);
	~FScopedGpuCapture();
	FFenceMarker m_waitFence;
};

struct FShaderDesc
{
	std::wstring m_relativepath;
	std::wstring m_entrypoint;
	std::wstring m_defines;
	std::wstring m_profile;
};

struct FRootsigDesc
{
	std::wstring m_relativepath;
	std::wstring m_entrypoint;
	std::wstring m_profile;
};

struct FRootSignature
{
	D3DRootSignature_t* m_rootsig;
	FFenceMarker m_fenceMarker;
	~FRootSignature();
};

struct FResource
{
	D3DResource_t* m_d3dResource;
	std::wstring m_name;
	concurrency::concurrent_vector<D3D12_RESOURCE_STATES> m_subresourceStates;
	winrt::com_ptr<D3DFence_t> m_transitionFence;
	size_t m_transitionFenceValue;

	FResource();
	~FResource();
	FResource& operator==(FResource&& other)
	{
		// Resources are moved during async level load. 
		// A custom move assingment is used here because we want to avoid 
		// calling the desctructor which can release the resource.
		m_d3dResource = other.m_d3dResource;
		m_name = other.m_name;
		m_subresourceStates = std::move(other.m_subresourceStates);
		m_transitionFence = other.m_transitionFence;
		m_transitionFenceValue = other.m_transitionFenceValue;
		other.m_d3dResource = nullptr;
	}

	void SetName(const std::wstring& name);
	HRESULT InitCommittedResource(const std::wstring& name, const D3D12_HEAP_PROPERTIES& heapProperties, const D3D12_RESOURCE_DESC& resourceDesc, const D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue = nullptr);
	HRESULT InitReservedResource(const std::wstring& name, const D3D12_RESOURCE_DESC& resourceDesc, const D3D12_RESOURCE_STATES initialState);
	size_t GetTransitionToken();
	void Transition(FCommandList* cmdList, const size_t token, const uint32_t subresourceIndex, const D3D12_RESOURCE_STATES destState);
	void UavBarrier(FCommandList* cmdList);
	size_t GetSizeBytes() const;
};

struct FTexture
{
	FResource* m_resource;
	uint32_t m_srvIndex = ~0u;

	~FTexture();
	FTexture& operator==(FTexture&& other)
	{
		// BindlessShaderResources are moved during async level load. 
		// A custom move assingment is used here because we want to avoid 
		// calling the desctructor which can release the resource.
		m_resource = other.m_resource;
		m_srvIndex = other.m_srvIndex;
		other.m_resource = nullptr;
		other.m_srvIndex = ~0u;
	}
};

struct FShaderSurface
{
	struct FDescriptors
	{
		uint32_t SRV;
		std::vector<uint32_t> RTVorDSVs;					// RTV or DSV indices. One for each mip level
		std::vector<uint32_t> UAVs;							// One for each mip level
		std::vector<uint32_t> NonShaderVisibleUAVs;			// One for each mip level
		void Release(const uint32_t surfaceType);
	};

	uint32_t m_type;
	ResourceAllocation m_alloc;
	FResource* m_resource;
	FDescriptors m_descriptorIndices;
	~FShaderSurface();
	FShaderSurface& operator=(FShaderSurface&& other);
};

struct FShaderBuffer
{
	struct FDescriptors
	{
		uint32_t UAV;
		uint32_t NonShaderVisibleUAV;
		uint32_t SRV;
		void Release();
	};

	ResourceAccessMode m_accessMode;
	ResourceAllocation m_alloc;
	FResource* m_resource;
	FDescriptors m_descriptorIndices;
	
	~FShaderBuffer();
	FShaderBuffer& operator=(FShaderBuffer&& other);
};

struct FSystemBuffer
{
	FResource* m_resource;
	ResourceAccessMode m_accessMode;
	FFenceMarker m_fenceMarker;
	~FSystemBuffer();
};

class FResourceUploadContext
{
public:
	FResourceUploadContext() = delete;
	FResourceUploadContext(const size_t uploadBufferSizeInBytes);
	~FResourceUploadContext();

	void UpdateSubresources(
		FResource* destinationResource,
		const std::vector<D3D12_SUBRESOURCE_DATA>& srcData,
		std::function<void(FCommandList*)> transition);

	void SubmitUploads(FCommandList* owningCL, FFenceMarker* waitEvent = nullptr);

private:
	std::unique_ptr<FSystemBuffer> m_uploadBuffer;
	FCommandList* m_copyCommandlist;
	uint8_t* m_mappedPtr;
	size_t m_currentOffset;
	size_t m_sizeInBytes;
	std::vector<std::function<void(FCommandList*)>> m_pendingTransitions;
};

class FResourceReadbackContext
{
public:
	FResourceReadbackContext() = delete;
	explicit FResourceReadbackContext(const FResource* resource);
	~FResourceReadbackContext();

	FFenceMarker StageSubresources(const FFenceMarker sourceReadyMarker);
	D3D12_SUBRESOURCE_DATA GetTextureData(int subresourceIndex = 0);

	template<class T> 
	T* GetBufferData()
	{
		if (!m_mappedPtr)
		{
			m_readbackBuffer->m_resource->m_d3dResource->Map(0, nullptr, (void**)&m_mappedPtr);
		}

		return reinterpret_cast<T*>(m_mappedPtr);
	}

private:
	const FResource* m_source;
	std::unique_ptr<FSystemBuffer> m_readbackBuffer;
	FCommandList* m_copyCommandlist;
	uint8_t* m_mappedPtr;
	size_t m_sizeInBytes;
	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> m_layouts;
};

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														RenderUtils12
//-----------------------------------------------------------------------------------------------------------------------------------------------
namespace RenderUtils12
{
	// From d3dx12.h
	inline constexpr uint32_t CalcSubresource(uint32_t MipSlice, uint32_t ArraySlice, uint32_t PlaneSlice, uint32_t MipLevels, uint32_t ArraySize) noexcept
	{
		return MipSlice + ArraySlice * MipLevels + PlaneSlice * MipLevels * ArraySize;
	}

	inline constexpr size_t CalcMipCount(size_t texWidth, size_t texHeight, bool bCompressed)
	{
		//  Min size is 4x4 for block compression
		size_t minSize = bCompressed ? 4 : 1;

		size_t numMips = 0;
		size_t width = texWidth, height = texHeight;
		while (width >= minSize && height >= minSize)
		{
			numMips++;
			width = width >> 1;
			height = height >> 1;
		}

		return numMips;
	}
}

//-----------------------------------------------------------------------------------------------------------------------------------------------
//														RenderBackend12
//-----------------------------------------------------------------------------------------------------------------------------------------------

namespace RenderBackend12
{
	bool Initialize(const HWND& windowHandle, const uint32_t resX, const uint32_t resY, const FConfig& config);
	void Teardown();
	D3DDevice_t* GetDevice();
	void FlushGPU();

	// Command Lists
	FCommandList* FetchCommandlist(const std::wstring& name, const D3D12_COMMAND_LIST_TYPE type);
	void ExecuteCommandlists(const D3D12_COMMAND_LIST_TYPE commandQueueType, std::vector<FCommandList*> commandLists);
	D3DCommandQueue_t* GetCommandQueue(D3D12_COMMAND_LIST_TYPE type);

	// Root Signatures
	std::unique_ptr<FRootSignature> FetchRootSignature(const std::wstring& name, const FCommandList* dependentCL, const FRootsigDesc& rootsig);
	std::unique_ptr<FRootSignature> FetchRootSignature(const std::wstring& name, const FCommandList* dependentCL, IDxcBlob* blob);

	// Pipeline States
	D3DPipelineState_t* FetchGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);
	D3DPipelineState_t* FetchComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC&  desc);
	D3DStateObject_t* FetchRaytracePipelineState(const D3D12_STATE_OBJECT_DESC& desc);

	// Swap chain and back buffers
	FShaderSurface* GetBackBuffer();
	void WaitForSwapChain();
	void PresentDisplay();

	// Shaders
	typedef void (*ShadersDirtiedCallback)();
	IDxcBlob* CacheShader(const FShaderDesc& shaderDesc);
	IDxcBlob* CacheRootsignature(const FRootsigDesc& rootsigDesc);
	void RecompileModifiedShaders(ShadersDirtiedCallback);

	// Descriptor Management
	D3DDescriptorHeap_t* GetDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_TYPE type);
	D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE descriptorHeapType, uint32_t descriptorIndex, bool bShaderVisible = true);
	D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE descriptorHeapType, uint32_t descriptorIndex);

	// Command Signatures
	D3DCommandSignature_t* CacheCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC desc, D3DRootSignature_t* rootsig);

	// Feature Support
	uint32_t GetLaneCount();

	// Resource Management
	FShaderSurface* CreateNewShaderSurface(
		const std::wstring& name,
		const uint32_t surfaceType,
		const ResourceAllocation alloc,
		const DXGI_FORMAT format,
		const size_t width,
		const size_t height,
		const size_t mipLevels = 1,
		const size_t depth = 1,
		const size_t arraySize = 1,
		const size_t sampleCount = 1,
		const bool bCreateSRV = true,
		const bool bCreateNonShaderVisibleDescriptors = false);

	FTexture* CreateNewTexture(
		const std::wstring& name, 
		const TextureType type,
		const DXGI_FORMAT format,
		const size_t width,
		const size_t height,
		const size_t numMips,
		const size_t numSlices,
		D3D12_RESOURCE_STATES resourceState,
		const DirectX::Image* images = nullptr,
		FResourceUploadContext* uploadContext = nullptr);

	uint32_t CreateSampler(
		const D3D12_FILTER filter,
		const D3D12_TEXTURE_ADDRESS_MODE addressU,
		const D3D12_TEXTURE_ADDRESS_MODE addressV,
		const D3D12_TEXTURE_ADDRESS_MODE addressW);

	FShaderBuffer* CreateNewShaderBuffer(
		const std::wstring& name,
		const BufferType type,
		const ResourceAccessMode accessMode,
		const ResourceAllocation alloc,
		const size_t size,
		const bool bCreateNonShaderVisibleDescriptor = false,
		const uint8_t* pData = nullptr,
		FResourceUploadContext* uploadContext = nullptr,
		const int fixedUavIndex = -1,								// Use provided UAV index instead of fetching one from bindless descriptor pool
		const int fixedSrvIndex = -1								// Use provided SRV index instead of fetching one from bindless descriptor pool
		);

	FSystemBuffer* CreateNewSystemBuffer(
		const std::wstring& name,
		const ResourceAccessMode accessMode,
		const size_t size,
		const FFenceMarker retireFence,								// Fence marker that decides whether the buffer is ready to be released. This is usually the fence for the associated command list that uses this buffer.
		std::function<void(uint8_t*)> uploadFunc = nullptr);

	size_t GetResourceSize(const DirectX::ScratchImage& image);

	FFenceMarker GetCurrentFrameFence();
}