#define graphics_rootsig_main
    ", CBV(b0)" \
    ", StaticSampler(s0, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_ANISOTROPIC, maxAnisotropy = 8, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE)" \
    ", StaticSampler(s1, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, comparisonFunc = COMPARISON_LESS_EQUAL, addressU = TEXTURE_ADDRESS_BORDER, addressV = TEXTURE_ADDRESS_BORDER, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE)" \
    ", DescriptorTable(SRV(t0, space = 0, numDescriptors = 1), visibility = SHADER_VISIBILITY_PIXEL)"