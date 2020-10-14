#pragma once

namespace Settings
{
	constexpr DXGI_FORMAT k_backBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
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
