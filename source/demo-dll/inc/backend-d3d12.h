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
class FResourceUploadContext;

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

//--------------------------------------------------------------------
struct FFenceMarker
{
	FFenceMarker() = default;
	FFenceMarker(D3DFence_t* fence, const size_t value) :
		m_fence{ fence }, m_value{ value }{}

	// CPU sync
	void Signal() const;
	void Wait() const;

	// GPU sync
	void Signal(D3DCommandQueue_t* cmdQueue) const;
	void Wait(D3DCommandQueue_t* cmdQueue) const;

private:
	D3DFence_t* m_fence;
	size_t m_value;
};

//--------------------------------------------------------------------
struct FCommandList
{
	enum class SyncPoint
	{
		CpuSubmission,
		GpuBegin,
		GpuFinish,
		Count
	};

	struct Sync
	{
		FFenceMarker m_fenceMarkers[(uint32_t)SyncPoint::Count];
	};

	D3D12_COMMAND_LIST_TYPE m_type;
	std::wstring m_name;
	winrt::com_ptr<D3DCommandList_t> m_d3dCmdList;
	winrt::com_ptr<D3DCommandAllocator_t> m_cmdAllocator;
	winrt::com_ptr<D3DFence_t> m_fence[(uint32_t)SyncPoint::Count];
	size_t m_fenceValues[(uint32_t)SyncPoint::Count];
	std::vector<std::function<void(void)>> m_postExecuteCallbacks;

	FCommandList() = default;
	FCommandList(const D3D12_COMMAND_LIST_TYPE type);
	void ResetFence(const size_t fenceValue);
	void SetName(const std::wstring& name);
	FFenceMarker GetFence(const SyncPoint type) const;
	Sync GetSync() const;
};

//--------------------------------------------------------------------
// Saves the capture to a file named PIXGpuCapture.wpix in the binaries directory
struct FScopedGpuCapture
{
	FScopedGpuCapture() = delete;
	FScopedGpuCapture(FCommandList* cl);
	~FScopedGpuCapture();
	FFenceMarker m_waitFence;
};

//--------------------------------------------------------------------
struct FShaderDesc
{
	std::wstring m_relativepath;
	std::wstring m_entrypoint;
	std::wstring m_defines;
	std::wstring m_profile;
};

//--------------------------------------------------------------------
struct FRootSignature
{
	struct Desc
	{
		std::wstring m_relativepath;
		std::wstring m_entrypoint;
		std::wstring m_profile;
	};

	D3DRootSignature_t* m_rootsig;
	FFenceMarker m_fenceMarker;
	~FRootSignature();
};

//--------------------------------------------------------------------
struct FResource
{
	enum class Type
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

	enum class AccessMode
	{
		GpuReadOnly,
		GpuReadWrite,
		GpuWriteOnly,	// Used for acceleration structure scratch buffers
		CpuWriteOnly,	// Upload resources
		CpuReadOnly,	// Readback resources
	};

	struct Allocation
	{
		enum class Type
		{
			Persistent,
			Transient
		};
		
		Type m_type;				// Persistent or Transient
		FFenceMarker m_lifetime;	// Transient resources must specify lifetime via fence marker

		static Allocation Persistent() { return { Type::Persistent }; }
		static Allocation Transient(const FFenceMarker fence) { return { Type::Transient, fence }; }
	};

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

//--------------------------------------------------------------------
struct FTexture
{
	enum class Type
	{
		Tex2D,
		Tex2DArray,
		TexCube
	};

	struct FUploadDesc
	{
		const DirectX::Image* images = nullptr;
		FResourceUploadContext* context = nullptr;
	};

	struct FResourceDesc
	{
		const std::wstring& name;
		FTexture::Type type;
		FResource::Allocation alloc;
		DXGI_FORMAT format;
		size_t width;
		size_t height;
		size_t numMips = 1;
		size_t numSlices = 1;
		D3D12_RESOURCE_STATES resourceState;
		FUploadDesc upload;
	};

	FResource* m_resource;
	FResource::Allocation m_alloc;
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

//--------------------------------------------------------------------
struct FShaderSurface
{
	// Not using enum class here so that we can easily pass in OR'd values from call sites to create surfaces
	enum Type : uint32_t
	{
		RenderTarget = (1 << 0),
		DepthStencil = (1 << 1),
		SwapChain = (1 << 2),
		UAV = (1 << 3)
	};

	struct FResourceDesc
	{
		const std::wstring& name;
		uint32_t type;
		FResource::Allocation alloc;
		DXGI_FORMAT format;
		size_t width;
		size_t height;
		size_t mipLevels = 1;
		size_t depth = 1;
		size_t arraySize = 1;
		size_t sampleCount = 1;
		bool bCreateSRV = true;
		bool bCreateNonShaderVisibleDescriptors = false;
	};

	struct FDescriptors
	{
		uint32_t SRV;
		std::vector<uint32_t> RTVorDSVs;					// RTV or DSV indices. One for each mip level
		std::vector<uint32_t> UAVs;							// One for each mip level
		std::vector<uint32_t> NonShaderVisibleUAVs;			// One for each mip level
		void Release(const uint32_t surfaceType);
	};

	uint32_t m_type;
	FResource::Allocation m_alloc;
	FResource* m_resource;
	FDescriptors m_descriptorIndices;
	~FShaderSurface();
	FShaderSurface& operator=(FShaderSurface&& other);
};

//--------------------------------------------------------------------
struct FShaderBuffer
{
	enum class Type
	{
		Raw,
		AccelerationStructure
	};

	struct FUploadDesc
	{
		const uint8_t* pData = nullptr;
		FResourceUploadContext* context = nullptr;
	};

	struct FResourceDesc
	{
		const std::wstring& name;
		Type type;
		FResource::AccessMode accessMode;
		FResource::Allocation alloc;
		size_t size;
		bool bCreateNonShaderVisibleDescriptor = false;
		FUploadDesc upload;
		// Use provided UAV index instead of fetching one from bindless descriptor pool
		int fixedUavIndex = -1;
		// Use provided SRV index instead of fetching one from bindless descriptor pool
		int fixedSrvIndex = -1;
	};

	struct FDescriptors
	{
		uint32_t UAV;
		uint32_t NonShaderVisibleUAV;
		uint32_t SRV;
		void Release();
	};

	FResource::AccessMode m_accessMode;
	FResource::Allocation m_alloc;
	FResource* m_resource;
	FDescriptors m_descriptorIndices;
	
	~FShaderBuffer();
	FShaderBuffer& operator=(FShaderBuffer&& other);
};

//--------------------------------------------------------------------
struct FSystemBuffer
{
	struct FResourceDesc
	{
		const std::wstring& name;
		FResource::AccessMode accessMode;
		FResource::Allocation alloc;
		size_t size;							
		std::function<void(uint8_t*)> uploadCallback = nullptr;
	};

	FResource* m_resource;
	FResource::AccessMode m_accessMode;
	FResource::Allocation m_alloc;
	~FSystemBuffer();
};

//--------------------------------------------------------------------
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

//--------------------------------------------------------------------
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
	std::unique_ptr<FRootSignature> FetchRootSignature(const std::wstring& name, const FCommandList* dependentCL, const FRootSignature::Desc& rootsig);
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
	IDxcBlob* CacheRootsignature(const FRootSignature::Desc& rootsigDesc);
	void RecompileModifiedShaders(ShadersDirtiedCallback);

	// Descriptor Management
	D3DDescriptorHeap_t* GetDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_TYPE type);
	D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE descriptorHeapType, uint32_t descriptorIndex, bool bShaderVisible = true);
	D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE descriptorHeapType, uint32_t descriptorIndex);

	// Command Signatures
	D3DCommandSignature_t* CacheCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC desc, D3DRootSignature_t* rootsig);

	// Feature Support
	uint32_t GetLaneCount();

	// Resource Management (User owns the memory and is responsible for freeing it)
	FShaderSurface* CreateNewShaderSurface(const FShaderSurface::FResourceDesc& desc);
	FShaderBuffer* CreateNewShaderBuffer(const FShaderBuffer::FResourceDesc& desc);
	FSystemBuffer* CreateNewSystemBuffer(const FSystemBuffer::FResourceDesc& desc);
	FTexture* CreateNewTexture(const FTexture::FResourceDesc& desc);

	uint32_t CreateSampler(
		const D3D12_FILTER filter,
		const D3D12_TEXTURE_ADDRESS_MODE addressU,
		const D3D12_TEXTURE_ADDRESS_MODE addressV,
		const D3D12_TEXTURE_ADDRESS_MODE addressW);

	size_t GetResourceSize(const DirectX::ScratchImage& image);

	FFenceMarker GetCurrentFrameFence();
}