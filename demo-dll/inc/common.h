#pragma once

namespace Settings
{
	constexpr DXGI_FORMAT k_backBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
	constexpr char* k_scenePath = CONTENT_DIR "\\metal-rough-spheres\\MetalRoughSpheres.gltf";
}

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

inline void DebugAssert(bool success, const char* msg = nullptr)
{
#if defined _DEBUG
	if (!success)
	{
		if (msg)
		{
			OutputDebugStringA("\n*****\n");
			OutputDebugStringA(msg);
			OutputDebugStringA("\n*****\n");
		}

		_CrtDbgBreak();
	}
#endif
}