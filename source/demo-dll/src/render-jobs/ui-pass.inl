namespace RenderJob::UIPass
{
	struct Desc
	{
		FShaderSurface* colorTarget;
		FConfig renderConfig;
	};

	Result Execute(Sync* jobSync, const Desc& passDesc)
	{
		size_t renderToken = jobSync->GetToken();
		size_t colorTargetTransitionToken = passDesc.colorTarget->m_resource->GetTransitionToken();
		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"imgui_job", D3D12_COMMAND_LIST_TYPE_DIRECT);

		Result passResult;
		passResult.m_syncObj = cmdList->GetSync();
		passResult.m_task = concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_ui", PIX_COLOR_DEFAULT);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "imgui_commands", 0);

			ImDrawData* drawData = ImGui::GetDrawData();
			size_t vtxBufferSize = 0;
			size_t idxBufferSize = 0;
			for (int i = 0; i < drawData->CmdListsCount; ++i)
			{
				const ImDrawList* imguiCL = drawData->CmdLists[i];
				vtxBufferSize += imguiCL->VtxBuffer.Size * sizeof(ImDrawVert);
				idxBufferSize += imguiCL->IdxBuffer.Size * sizeof(ImDrawIdx);
			}

			// Vertex Buffer
			{
				std::unique_ptr<FSystemBuffer> vtxBuffer{ RenderBackend12::CreateNewSystemBuffer(
					L"imgui_vb",
					FResource::AccessMode::CpuWriteOnly,
					vtxBufferSize,
					cmdList->GetFence(FCommandList::SyncPoint::GpuFinish),
					[drawData](uint8_t* pDest)
				{
					ImDrawVert* vbDest = reinterpret_cast<ImDrawVert*>(pDest);
					for (int i = 0; i < drawData->CmdListsCount; ++i)
					{
						const ImDrawList* imguiCL = drawData->CmdLists[i];
						memcpy(vbDest, imguiCL->VtxBuffer.Data, imguiCL->VtxBuffer.Size * sizeof(ImDrawVert));
						vbDest += imguiCL->VtxBuffer.Size;
					}
				}) };

				D3D12_VERTEX_BUFFER_VIEW vbDescriptor = {};
				vbDescriptor.BufferLocation = vtxBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress();
				vbDescriptor.SizeInBytes = vtxBufferSize;
				vbDescriptor.StrideInBytes = sizeof(ImDrawVert);
				d3dCmdList->IASetVertexBuffers(0, 1, &vbDescriptor);
			}

			// Index Buffer
			{
				std::unique_ptr<FSystemBuffer> idxBuffer{ RenderBackend12::CreateNewSystemBuffer(
					L"imgui_ib",
					FResource::AccessMode::CpuWriteOnly,
					idxBufferSize,
					cmdList->GetFence(FCommandList::SyncPoint::GpuFinish),
					[drawData](uint8_t* pDest)
				{
					ImDrawIdx* ibDest = reinterpret_cast<ImDrawIdx*>(pDest);
					for (int i = 0; i < drawData->CmdListsCount; ++i)
					{
						const ImDrawList* imguiCL = drawData->CmdLists[i];
						memcpy(ibDest, imguiCL->IdxBuffer.Data, imguiCL->IdxBuffer.Size * sizeof(ImDrawIdx));
						ibDest += imguiCL->IdxBuffer.Size;
					}
				}) };

				D3D12_INDEX_BUFFER_VIEW ibDescriptor = {};
				ibDescriptor.BufferLocation = idxBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress();
				ibDescriptor.SizeInBytes = idxBufferSize;
				ibDescriptor.Format = sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
				d3dCmdList->IASetIndexBuffer(&ibDescriptor);
			}

			// Descriptor Heaps
			D3DDescriptorHeap_t* descriptorHeaps[] = { RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
			d3dCmdList->SetDescriptorHeaps(1, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(L"imgui_rootsig", cmdList, FRootSignature::Desc{L"imgui.hlsl", L"rootsig", L"rootsig_1_1"});
			d3dCmdList->SetGraphicsRootSignature(rootsig->m_rootsig);

			// Vertex Constant Buffer
			{
				struct _cb
				{
					float   mvp[4][4];
				} vtxConstantBuffer;

				float L = drawData->DisplayPos.x;
				float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
				float T = drawData->DisplayPos.y;
				float B = drawData->DisplayPos.y + drawData->DisplaySize.y;
				float mvp[4][4] =
				{
					{ 2.0f / (R - L),   0.0f,				0.0f,       0.0f },
					{ 0.0f,				2.0f / (T - B),     0.0f,       0.0f },
					{ 0.0f,				0.0f,				0.5f,       0.0f },
					{ (R + L) / (L - R),(T + B) / (B - T),  0.5f,       1.0f },
				};
				memcpy(&vtxConstantBuffer.mvp, mvp, sizeof(mvp));
				d3dCmdList->SetGraphicsRoot32BitConstants(0, 16, &vtxConstantBuffer, 0);
			}

			// PSO
			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
			psoDesc.NodeMask = 1;
			psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.pRootSignature = rootsig->m_rootsig;
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = passDesc.renderConfig.BackBufferFormat;
			psoDesc.SampleDesc.Count = 1;
			psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

			// PSO - Shaders
			{
				D3D12_SHADER_BYTECODE& vs = psoDesc.VS;
				D3D12_SHADER_BYTECODE& ps = psoDesc.PS;

				IDxcBlob* vsBlob = RenderBackend12::CacheShader({ L"imgui.hlsl", L"vs_main", L"" , L"vs_6_6" });
				IDxcBlob* psBlob = RenderBackend12::CacheShader({ L"imgui.hlsl", L"ps_main", L"" , L"ps_6_6" });

				vs.pShaderBytecode = vsBlob->GetBufferPointer();
				vs.BytecodeLength = vsBlob->GetBufferSize();
				ps.pShaderBytecode = psBlob->GetBufferPointer();
				ps.BytecodeLength = psBlob->GetBufferSize();
			}

			// PSO - Input Layout
			{
				static D3D12_INPUT_ELEMENT_DESC inputLayout[] =
				{
					{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, (UINT)IM_OFFSETOF(ImDrawVert, pos), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
					{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, (UINT)IM_OFFSETOF(ImDrawVert, uv),  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
					{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, (UINT)IM_OFFSETOF(ImDrawVert, col), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				};
				psoDesc.InputLayout = { inputLayout, 3 };
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
				desc.RenderTarget[0].BlendEnable = TRUE;
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

			D3DPipelineState_t* pso = RenderBackend12::FetchGraphicsPipelineState(psoDesc);
			d3dCmdList->SetPipelineState(pso);

			// Viewport
			D3D12_VIEWPORT vp = {};
			vp.Width = drawData->DisplaySize.x;
			vp.Height = drawData->DisplaySize.y;
			vp.MinDepth = 0.0f;
			vp.MaxDepth = 1.0f;
			vp.TopLeftX = vp.TopLeftY = 0.0f;
			d3dCmdList->RSSetViewports(1, &vp);

			// Blend Factor
			const float blendFactor[4] = { 0.f, 0.f, 0.f, 0.f };
			d3dCmdList->OMSetBlendFactor(blendFactor);

			passDesc.colorTarget->m_resource->Transition(cmdList, colorTargetTransitionToken, 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RenderBackend12::GetBackBuffer()->m_descriptorIndices.RTVorDSVs[0]) };
			d3dCmdList->OMSetRenderTargets(1, rtvs, FALSE, nullptr);

			d3dCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			// Render commands
			int vertexOffset = 0;
			int indexOffset = 0;
			ImVec2 clipOffset = drawData->DisplayPos;
			for (int n = 0; n < drawData->CmdListsCount; n++)
			{
				const ImDrawList* imguiCmdList = drawData->CmdLists[n];
				for (int cmdIndex = 0; cmdIndex < imguiCmdList->CmdBuffer.Size; ++cmdIndex)
				{
					const ImDrawCmd* pcmd = &imguiCmdList->CmdBuffer[cmdIndex];

					ImTextureID textureIndex = pcmd->GetTexID();
					d3dCmdList->SetGraphicsRoot32BitConstants(1, 1, &textureIndex, 0);

					if (pcmd->UserCallback != NULL)
					{
						// User callback, registered via ImDrawList::AddCallback()
						// (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render state.)
						if (pcmd->UserCallback == ImDrawCallback_ResetRenderState)
						{
							DebugAssert(false);// ImGui_ImplDX12_SetupRenderState(drawData, d3dCmdList, fr);
						}
						else
						{
							pcmd->UserCallback(imguiCmdList, pcmd);
						}
					}
					else
					{
						// Apply Scissor, Bind texture, Draw
						const D3D12_RECT r =
						{
							(LONG)(pcmd->ClipRect.x - clipOffset.x),
							(LONG)(pcmd->ClipRect.y - clipOffset.y),
							(LONG)(pcmd->ClipRect.z - clipOffset.x),
							(LONG)(pcmd->ClipRect.w - clipOffset.y)
						};

						if (r.right > r.left && r.bottom > r.top)
						{
							d3dCmdList->RSSetScissorRects(1, &r);
							d3dCmdList->DrawIndexedInstanced(pcmd->ElemCount, 1, pcmd->IdxOffset + indexOffset, pcmd->VtxOffset + vertexOffset, 0);
						}
					}
				}

				vertexOffset += imguiCmdList->VtxBuffer.Size;
				indexOffset += imguiCmdList->IdxBuffer.Size;
			}

			return cmdList;

		}).then([=](FCommandList* recordedCl) mutable
		{
			jobSync->Execute(renderToken, recordedCl);
		});

		return passResult;
	}
}