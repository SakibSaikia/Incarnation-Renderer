namespace RenderJob::GBufferRasterPass
{
	struct Desc
	{
		FShaderSurface* sourceVisBuffer;
		FShaderSurface* colorTarget;
		FShaderSurface* gbufferTargets[3];
		FShaderSurface* depthStencilTarget;
		FSystemBuffer* sceneConstantBuffer;
		FSystemBuffer* viewConstantBuffer;
		uint32_t resX;
		uint32_t resY;
		const FScene* scene;
	};

	Result Execute(Sync* jobSync, const Desc& passDesc)
	{
		size_t renderToken = jobSync->GetToken();
		size_t gbufferTransitionTokens[3] = {
			passDesc.gbufferTargets[0]->m_resource->GetTransitionToken(),
			passDesc.gbufferTargets[1]->m_resource->GetTransitionToken(),
			passDesc.gbufferTargets[2]->m_resource->GetTransitionToken()
		};

		FCommandList* cmdList = RenderBackend12::FetchCommandlist(L"gbuffer_raster", D3D12_COMMAND_LIST_TYPE_DIRECT);

		Result passResult;
		passResult.m_syncObj = cmdList->GetSync();
		passResult.m_task = concurrency::create_task([=]
		{
			SCOPED_CPU_EVENT("record_gbuffer_raster", PIX_COLOR_DEFAULT);
			D3DCommandList_t* d3dCmdList = cmdList->m_d3dCmdList.get();
			SCOPED_COMMAND_LIST_EVENT(cmdList, "gbuffer_raster", 0);

			passDesc.gbufferTargets[0]->m_resource->Transition(cmdList, gbufferTransitionTokens[0], 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
			passDesc.gbufferTargets[1]->m_resource->Transition(cmdList, gbufferTransitionTokens[1], 0, D3D12_RESOURCE_STATE_RENDER_TARGET);
			passDesc.gbufferTargets[2]->m_resource->Transition(cmdList, gbufferTransitionTokens[2], 0, D3D12_RESOURCE_STATE_RENDER_TARGET);

			// Descriptor heaps need to be set before setting the root signature when using HLSL Dynamic Resources
			// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html
			D3DDescriptorHeap_t* descriptorHeaps[] =
			{
				RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
				RenderBackend12::GetDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
			};
			d3dCmdList->SetDescriptorHeaps(2, descriptorHeaps);

			// Root Signature
			std::unique_ptr<FRootSignature> rootsig = RenderBackend12::FetchRootSignature(
				L"gbuffer_raster_rootsig",
				cmdList,
				FRootSignature::Desc{ L"geo-raster/gbuffer-raster.hlsl", L"rootsig", L"rootsig_1_1" });
			d3dCmdList->SetGraphicsRootSignature(rootsig->m_rootsig);

			d3dCmdList->SetGraphicsRootConstantBufferView(1, passDesc.viewConstantBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress());
			d3dCmdList->SetGraphicsRootConstantBufferView(2, passDesc.sceneConstantBuffer->m_resource->m_d3dResource->GetGPUVirtualAddress());

			D3D12_VIEWPORT viewport{ 0.f, 0.f, (float)passDesc.resX, (float)passDesc.resY, 0.f, 1.f };
			D3D12_RECT screenRect{ 0, 0, (LONG)passDesc.resX, (LONG)passDesc.resY };
			d3dCmdList->RSSetViewports(1, &viewport);
			d3dCmdList->RSSetScissorRects(1, &screenRect);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { 
				RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passDesc.gbufferTargets[0]->m_descriptorIndices.RTVorDSVs[0]),
				RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passDesc.gbufferTargets[1]->m_descriptorIndices.RTVorDSVs[0]),
				RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passDesc.gbufferTargets[2]->m_descriptorIndices.RTVorDSVs[0])
			};

			D3D12_CPU_DESCRIPTOR_HANDLE dsv = RenderBackend12::GetCPUDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, passDesc.depthStencilTarget->m_descriptorIndices.RTVorDSVs[0]);
			d3dCmdList->OMSetRenderTargets(3, rtvs, FALSE, &dsv);

			// Issue decal draws
			for (int meshIndex = 0; meshIndex < passDesc.scene->m_sceneMeshDecals.GetCount(); ++meshIndex)
			{
				const FMesh& mesh = passDesc.scene->m_sceneMeshDecals.m_entityList[meshIndex];
				SCOPED_COMMAND_LIST_EVENT(cmdList, passDesc.scene->m_sceneMeshDecals.m_entityNames[meshIndex].c_str(), 0);

				for (const FMeshPrimitive& primitive : mesh.m_primitives)
				{
					const FMaterial& material = passDesc.scene->m_materialList[primitive.m_materialIndex];
					d3dCmdList->IASetPrimitiveTopology(primitive.m_topology);

					// PSO
					D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
					psoDesc.NodeMask = 1;
					psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
					psoDesc.pRootSignature = rootsig->m_rootsig;
					psoDesc.SampleMask = UINT_MAX;
					psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
					psoDesc.NumRenderTargets = 3;
					psoDesc.RTVFormats[0] = passDesc.gbufferTargets[0]->m_resource->m_d3dResource->GetDesc().Format;
					psoDesc.RTVFormats[1] = passDesc.gbufferTargets[1]->m_resource->m_d3dResource->GetDesc().Format;
					psoDesc.RTVFormats[2] = passDesc.gbufferTargets[2]->m_resource->m_d3dResource->GetDesc().Format;
					psoDesc.SampleDesc.Count = 1;
					psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

					// PSO - Shaders
					{
						IDxcBlob* vsBlob = RenderBackend12::CacheShader({ L"geo-raster/gbuffer-raster.hlsl", L"vs_main", L"" , L"vs_6_6" });
						IDxcBlob* psBlob = RenderBackend12::CacheShader({ L"geo-raster/gbuffer-raster.hlsl", L"ps_main", L"" , L"ps_6_6"});

						psoDesc.VS.pShaderBytecode = vsBlob->GetBufferPointer();
						psoDesc.VS.BytecodeLength = vsBlob->GetBufferSize();
						psoDesc.PS.pShaderBytecode = psBlob->GetBufferPointer();
						psoDesc.PS.BytecodeLength = psBlob->GetBufferSize();
					}

					// PSO - Rasterizer State
					{
						bool bDoubleSidedMaterial = material.m_doubleSided;

						D3D12_RASTERIZER_DESC& desc = psoDesc.RasterizerState;
						desc.FillMode = D3D12_FILL_MODE_SOLID;
						desc.CullMode = bDoubleSidedMaterial ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
						desc.FrontCounterClockwise = TRUE;
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
						desc.IndependentBlendEnable = TRUE;

						// For base color, blend using opacity value
						desc.RenderTarget[0].BlendEnable = TRUE;
						desc.RenderTarget[0].LogicOpEnable = FALSE;
						desc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
						desc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
						desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
						desc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
						desc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
						desc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
						desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

						// If the decal specifies a normal map, allow it to overwrite the existing 
						// GBuffer value (no use blending normals). Otherwise, retain the existing value.
						if (material.m_normalTextureIndex != -1)
						{
							desc.RenderTarget[1].BlendEnable = FALSE;
							desc.RenderTarget[1].LogicOpEnable = FALSE;
							desc.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
						}
						else
						{
							desc.RenderTarget[1].BlendEnable = TRUE;
							desc.RenderTarget[1].LogicOpEnable = FALSE;
							desc.RenderTarget[1].SrcBlend = D3D12_BLEND_ZERO;
							desc.RenderTarget[1].DestBlend = D3D12_BLEND_ONE;
							desc.RenderTarget[1].BlendOp = D3D12_BLEND_OP_ADD;
							desc.RenderTarget[1].SrcBlendAlpha = D3D12_BLEND_ZERO;
							desc.RenderTarget[1].DestBlendAlpha = D3D12_BLEND_ONE;
							desc.RenderTarget[1].BlendOpAlpha = D3D12_BLEND_OP_ADD;
							desc.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
						}

						// If the decal specifies a metallic/roughness map, allow it to overwrite the existing 
						// GBuffer value. Otherwise, retain the existing value.
						if (material.m_metallicRoughnessTextureIndex != -1)
						{
							desc.RenderTarget[2].BlendEnable = FALSE;
							desc.RenderTarget[2].LogicOpEnable = FALSE;
							desc.RenderTarget[2].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
						}
						else
						{
							desc.RenderTarget[2].BlendEnable = TRUE;
							desc.RenderTarget[2].LogicOpEnable = FALSE;
							desc.RenderTarget[2].SrcBlend = D3D12_BLEND_ZERO;
							desc.RenderTarget[2].DestBlend = D3D12_BLEND_ONE;
							desc.RenderTarget[2].BlendOp = D3D12_BLEND_OP_ADD;
							desc.RenderTarget[2].SrcBlendAlpha = D3D12_BLEND_ZERO;
							desc.RenderTarget[2].DestBlendAlpha = D3D12_BLEND_ONE;
							desc.RenderTarget[2].BlendOpAlpha = D3D12_BLEND_OP_ADD;
							desc.RenderTarget[2].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
						}
					}

					// PSO - Depth Stencil State
					{
						D3D12_DEPTH_STENCIL_DESC& desc = psoDesc.DepthStencilState;
						desc.DepthEnable = TRUE;
						desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
						desc.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
						desc.StencilEnable = FALSE;
					}

					D3DPipelineState_t* pso = RenderBackend12::FetchGraphicsPipelineState(psoDesc);
					d3dCmdList->SetPipelineState(pso);

					// Geometry constants
					struct PrimitiveCbLayout
					{
						Matrix localToWorldTransform;
						int m_indexAccessor;
						int m_positionAccessor;
						int m_uvAccessor;
						int m_normalAccessor;
						int m_tangentAccessor;
						int m_materialIndex;
					} primCb =
						{
							passDesc.scene->m_sceneMeshDecals.m_transformList[meshIndex],
							primitive.m_indexAccessor,
							primitive.m_positionAccessor,
							primitive.m_uvAccessor,
							primitive.m_normalAccessor,
							primitive.m_tangentAccessor,
							primitive.m_materialIndex
						};

					d3dCmdList->SetGraphicsRoot32BitConstants(0, sizeof(PrimitiveCbLayout) / 4, &primCb, 0);

					d3dCmdList->DrawInstanced(primitive.m_indexCount, 1, 0, 0);
				}
			}

			return cmdList;

		}).then([=](FCommandList* recordedCl) mutable
		{
			jobSync->Execute(renderToken, recordedCl);
		});

		return passResult;
	}
}