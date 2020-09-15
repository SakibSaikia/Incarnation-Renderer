#define WIN32_LEAN_AND_MEAN
#include <d3d12layer.h>
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
using D3D12Debug_t = ID3D12Debug;
using D3D12Device_t = ID3D12Device5;

namespace Demo
{
	namespace D3D12
	{
		Microsoft::WRL::ComPtr<DXGIFactory_t> s_dxgiFactory;
		Microsoft::WRL::ComPtr<D3D12Device_t> s_d3dDevice;

	#if defined(DEBUG) || defined (_DEBUG)
		Microsoft::WRL::ComPtr<D3D12Debug_t> s_debugController;
	#endif

		std::array<uint32_t, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> s_descriptorSize;
	}
} 

using namespace Demo::D3D12;

namespace
{
	Microsoft::WRL::ComPtr<DXGIAdapter_t> EnumerateAdapters(DXGIFactory_t* dxgiFactory)
	{
		Microsoft::WRL::ComPtr<DXGIAdapter_t> bestAdapter;
		size_t maxVram = 0;

		Microsoft::WRL::ComPtr<DXGIAdapter_t> adapter;
		uint32_t i = 0;

		while (s_dxgiFactory->EnumAdapters(i++, adapter.GetAddressOf()) != DXGI_ERROR_NOT_FOUND)
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

bool Demo::D3D12::Initialize()
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

	return true;
}