#define graphics_rootsig_main  "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)" \
				    ", CBV(b0)" \
				    ", CBV(b1, visibility = SHADER_VISIBILITY_VERTEX)" \
                    ", CBV(b2, visibility = SHADER_VISIBILITY_ALL)" \
                    ", CBV(b3, visibility = SHADER_VISIBILITY_VERTEX)" \
                    ", CBV(b4, visibility = SHADER_VISIBILITY_PIXEL)" \
				    ", StaticSampler(s0, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_ANISOTROPIC, maxAnisotropy = 8, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE)" \
                    ", StaticSampler(s1, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, comparisonFunc = COMPARISON_LESS_EQUAL, addressU = TEXTURE_ADDRESS_BORDER, addressV = TEXTURE_ADDRESS_BORDER, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE)" \
                    ", DescriptorTable(SRV(t0, space = 0, numDescriptors = 1), visibility = SHADER_VISIBILITY_PIXEL)"