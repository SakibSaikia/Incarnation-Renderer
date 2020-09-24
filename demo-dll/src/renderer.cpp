#include <demo.h>
#include <d3d12layer.h>
#include <settings.h>
#include <ppltasks.h>
#include <sstream>
#include <assert.h>
#include <imgui.h>
#include <dxcapi.h>

namespace Jobs
{
	concurrency::task<FCommandList> PreRender()
	{
		return concurrency::create_task([]
		{
			FCommandList cmdList = Demo::D3D12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			D3DCommandList_t* d3dCmdList = cmdList.m_cmdList.Get();
			d3dCmdList->SetName(L"pre_render_job");

			D3D12_RESOURCE_BARRIER barrierDesc = {};
			barrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrierDesc.Transition.pResource = Demo::D3D12::GetBackBufferResource();
			barrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			barrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrierDesc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			d3dCmdList->ResourceBarrier(1, &barrierDesc);

			return cmdList;
		});
	}

	concurrency::task<FCommandList> Render()
	{
		return concurrency::create_task([]
		{
			FCommandList cmdList = Demo::D3D12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			D3DCommandList_t* d3dCmdList = cmdList.m_cmdList.Get();
			d3dCmdList->SetName(L"render_job");

			Microsoft::WRL::ComPtr<D3DRootSignature_t> rootsig = Demo::D3D12::FetchGraphicsRootSignature({ L"rootsig.hlsl", L"graphics_rootsig_main" });
			d3dCmdList->SetGraphicsRootSignature(rootsig.Get());

			// PSO
			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.NodeMask = 1;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.pRootSignature = rootsig.Get();
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = Demo::Settings::k_backBufferFormat;
			psoDesc.SampleDesc.Count = 1;
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			// PSO - Shaders
			{
				D3D12_SHADER_BYTECODE& vs = psoDesc.VS;
				D3D12_SHADER_BYTECODE& ps = psoDesc.PS;

				IDxcBlob* vsBlob = Demo::D3D12::CacheShader({ L"hello.hlsl", L"vs_main", L"" }, L"vs_6_4");
				IDxcBlob* psBlob = Demo::D3D12::CacheShader({ L"hello.hlsl", L"ps_main", L"" }, L"ps_6_4");

				vs.pShaderBytecode = vsBlob->GetBufferPointer();
				vs.BytecodeLength = vsBlob->GetBufferSize();
				ps.pShaderBytecode = psBlob->GetBufferPointer();
				ps.BytecodeLength = psBlob->GetBufferSize();
			}

			// PSO - Rasterizer State
			{
				D3D12_RASTERIZER_DESC& desc = psoDesc.RasterizerState;
				desc.FillMode = D3D12_FILL_MODE_SOLID;
				desc.CullMode = D3D12_CULL_MODE_BACK;
				desc.FrontCounterClockwise = FALSE;
				desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
				desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
				desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
				desc.DepthClipEnable = TRUE;
				desc.MultisampleEnable = FALSE;
				desc.AntialiasedLineEnable = FALSE;
				desc.ForcedSampleCount = 0;
				desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
			}

			// PSO - Blend State
			{
				D3D12_BLEND_DESC& desc = psoDesc.BlendState;
				desc.AlphaToCoverageEnable = FALSE;
				desc.IndependentBlendEnable = FALSE;
				desc.RenderTarget[0].BlendEnable = FALSE;
				desc.RenderTarget[0].LogicOpEnable = FALSE;
				desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			}

			// PSO - Depth Stencil State
			{
				D3D12_DEPTH_STENCIL_DESC& desc = psoDesc.DepthStencilState;
				desc.DepthEnable = FALSE;
				desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
				desc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
				desc.StencilEnable = FALSE;
			}

			D3DPipelineState_t* pso = Demo::D3D12::FetchGraphicsPipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			D3D12_VIEWPORT viewport{ 0.f, 0.f, Demo::Settings::k_screenWidth, Demo::Settings::k_screenHeight, 0.f, 1.f };
			D3D12_RECT screenRect{ 0.f, 0.f, Demo::Settings::k_screenWidth, Demo::Settings::k_screenHeight };
			d3dCmdList->RSSetViewports(1, &viewport);
			d3dCmdList->RSSetScissorRects(1, &screenRect);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { Demo::D3D12::GetBackBufferDescriptor() };
			d3dCmdList->OMSetRenderTargets(1, rtvs, FALSE, nullptr);

			float clearColor[] = { .8f, .8f, 1.f, 0.f };
			d3dCmdList->ClearRenderTargetView(Demo::D3D12::GetBackBufferDescriptor(), clearColor, 0, nullptr);

			d3dCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			d3dCmdList->DrawInstanced(3, 1, 0, 0);

			return cmdList;
		});
	}

	concurrency::task<FCommandList> UI()
	{
		return concurrency::create_task([]
		{
			FCommandList cmdList = Demo::D3D12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			D3DCommandList_t* d3dCmdList = cmdList.m_cmdList.Get();
			d3dCmdList->SetName(L"imgui_job");

			ImDrawData* drawData = ImGui::GetDrawData();
			size_t vtxBufferSize = 0;
			size_t idxBufferSize = 0;
			for (int i = 0; i < drawData->CmdListsCount; ++i)
			{
				const ImDrawList* imguiCL = drawData->CmdLists[i];
				vtxBufferSize += imguiCL->VtxBuffer.Size * sizeof(ImDrawVert);
				idxBufferSize += imguiCL->IdxBuffer.Size * sizeof(ImDrawIdx);
			}

			FResource vtxBuffer = Demo::D3D12::CreateUploadBuffer(
				L"imgui_vb",
				vtxBufferSize,
				[drawData](uint8_t* pDest)
			{
				ImDrawVert* vbDest = reinterpret_cast<ImDrawVert*>(pDest);
				for (int i = 0; i < drawData->CmdListsCount; ++i)
				{
					const ImDrawList* imguiCL = drawData->CmdLists[i];
					memcpy(vbDest, imguiCL->VtxBuffer.Data, imguiCL->VtxBuffer.Size * sizeof(ImDrawVert));
					vbDest += imguiCL->VtxBuffer.Size;
				}
			});

			FResource idxBuffer = Demo::D3D12::CreateUploadBuffer(
				L"imgui_ib",
				idxBufferSize,
				[drawData](uint8_t* pDest)
			{
				ImDrawIdx* ibDest = reinterpret_cast<ImDrawIdx*>(pDest);
				for (int i = 0; i < drawData->CmdListsCount; ++i)
				{
					const ImDrawList* imguiCL = drawData->CmdLists[i];
					memcpy(ibDest, imguiCL->IdxBuffer.Data, imguiCL->IdxBuffer.Size * sizeof(ImDrawIdx));
					ibDest += imguiCL->IdxBuffer.Size;
				}
			});

			Microsoft::WRL::ComPtr<D3DRootSignature_t> rootsig = Demo::D3D12::FetchGraphicsRootSignature({ L"rootsig.hlsl", L"imgui_rootsig" });
			d3dCmdList->SetGraphicsRootSignature(rootsig.Get());

			// PSO
			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.NodeMask = 1;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.pRootSignature = rootsig.Get();
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = Demo::Settings::k_backBufferFormat;
			psoDesc.SampleDesc.Count = 1;
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			// PSO - Shaders
			{
				D3D12_SHADER_BYTECODE& vs = psoDesc.VS;
				D3D12_SHADER_BYTECODE& ps = psoDesc.PS;

				IDxcBlob* vsBlob = Demo::D3D12::CacheShader({ L"imgui.hlsl", L"vs_main", L"" }, L"vs_6_4");
				IDxcBlob* psBlob = Demo::D3D12::CacheShader({ L"imgui.hlsl", L"vs_main", L"" }, L"ps_6_4");

				vs.pShaderBytecode = vsBlob->GetBufferPointer();
				vs.BytecodeLength = vsBlob->GetBufferSize();
				ps.pShaderBytecode = psBlob->GetBufferPointer();
				ps.BytecodeLength = psBlob->GetBufferSize();
			}

			// PSO - Rasterizer State
			{
				D3D12_RASTERIZER_DESC& desc = psoDesc.RasterizerState;
				desc.FillMode = D3D12_FILL_MODE_SOLID;
				desc.CullMode = D3D12_CULL_MODE_NONE;
				desc.FrontCounterClockwise = FALSE;
				desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
				desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
				desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
				desc.DepthClipEnable = TRUE;
				desc.MultisampleEnable = FALSE;
				desc.AntialiasedLineEnable = FALSE;
				desc.ForcedSampleCount = 0;
				desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
			}

			// PSO - Blend State
			{
				D3D12_BLEND_DESC& desc = psoDesc.BlendState;
				desc.AlphaToCoverageEnable = FALSE;
				desc.IndependentBlendEnable = FALSE;
				desc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
				desc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
				desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
				desc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				desc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
				desc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
				desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			}

			// PSO - Depth Stencil State
			{
				D3D12_DEPTH_STENCIL_DESC& desc = psoDesc.DepthStencilState;
				desc.DepthEnable = FALSE;
				desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
				desc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
				desc.StencilEnable = FALSE;
			}

			D3DPipelineState_t* pso = Demo::D3D12::FetchGraphicsPipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			D3D12_VIEWPORT viewport{ 0.f, 0.f, Demo::Settings::k_screenWidth, Demo::Settings::k_screenHeight, 0.f, 1.f };
			D3D12_RECT screenRect{ 0.f, 0.f, Demo::Settings::k_screenWidth, Demo::Settings::k_screenHeight };
			d3dCmdList->RSSetViewports(1, &viewport);
			d3dCmdList->RSSetScissorRects(1, &screenRect);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { Demo::D3D12::GetBackBufferDescriptor() };
			d3dCmdList->OMSetRenderTargets(1, rtvs, FALSE, nullptr);

			float clearColor[] = { .8f, .8f, 1.f, 0.f };
			d3dCmdList->ClearRenderTargetView(Demo::D3D12::GetBackBufferDescriptor(), clearColor, 0, nullptr);

			d3dCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			d3dCmdList->DrawInstanced(3, 1, 0, 0);

			return cmdList;
		});
	}

	concurrency::task<FCommandList> Present()
	{
		return concurrency::create_task([]
		{
			FCommandList cmdList = Demo::D3D12::FetchCommandlist(D3D12_COMMAND_LIST_TYPE_DIRECT);
			D3DCommandList_t* d3dCmdList = cmdList.m_cmdList.Get();
			d3dCmdList->SetName(L"present_job");

			D3D12_RESOURCE_BARRIER barrierDesc = {};
			barrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrierDesc.Transition.pResource = Demo::D3D12::GetBackBufferResource();
			barrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
			barrierDesc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			d3dCmdList->ResourceBarrier(1, &barrierDesc);

			return cmdList;
		});
	}
}

void Demo::Render()
{
	auto preRenderCL = Jobs::PreRender().get();
	auto renderCL = Jobs::Render().get();
	auto presentCL = Jobs::Present().get();

	Demo::D3D12::ExecuteCommandlists(D3D12_COMMAND_LIST_TYPE_DIRECT, { preRenderCL, renderCL, presentCL });
	Demo::D3D12::PresentDisplay();
}