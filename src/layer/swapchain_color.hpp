// Swapchain color-space classification for the Choir overlay layer.
//
// ImGui authors its colors in sRGB. Whether the overlay must pre-convert them to linear
// before writing depends on BOTH the swapchain format AND its color space — mirroring
// MangoHud's convert_colors_vk(format, colorspace, ...):
//
//   * 8-bit _SRGB format (e.g. B8G8R8A8_SRGB): the GPU applies linear->sRGB on store, so
//     we pre-convert sRGB->linear and the encode reproduces the authored color.
//   * scRGB-linear HDR (VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, typically a float format
//     like R16G16B16A16_SFLOAT): the buffer is LINEAR with no store-encode, so we must
//     write linear values directly — again sRGB->linear. This is the case that made
//     Diablo 4 (D3D12/VKD3D in HDR) look over-saturated: we wrote sRGB values verbatim
//     into a linear buffer.
//
// Other HDR transfer functions (HDR10 PQ / HLG) need different conversions we don't
// implement yet; they are reported as "unconverted HDR" so the layer logs them instead of
// applying the wrong (sRGB) transfer.
#pragma once

#include <vulkan/vulkan.h>

namespace choir {

// True for swapchain formats that apply the sRGB transfer function on store.
inline bool is_srgb_format(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        case VK_FORMAT_R8G8B8_SRGB:
        case VK_FORMAT_B8G8R8_SRGB:
            return true;
        default:
            return false;
    }
}

// True for the floating-point formats used by HDR swapchains. These carry LINEAR values
// (scRGB: 1.0 == 80 nits SDR-white), with no store-encode — so the overlay must write
// linear, i.e. pre-convert its sRGB colors.
inline bool is_hdr_float_format(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R16G16B16A16_SFLOAT:   // the common scRGB HDR swapchain format
        case VK_FORMAT_R16G16B16_SFLOAT:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            return true;
        default:
            return false;
    }
}

// True when the overlay must pre-convert its sRGB colors to linear for this swapchain:
//   * an _SRGB format (the GPU re-encodes linear->sRGB on store), or
//   * an scRGB-linear HDR color space (EXTENDED_SRGB_LINEAR), or
//   * a PASS_THROUGH HDR float swapchain — what DXVK/VKD3D use for HDR, where the buffer
//     is linear scRGB and the presentation engine applies no transfer. Writing sRGB
//     colors verbatim there leaves the minor channels too high -> washed-out/under-
//     saturated (observed in Overwatch/Diablo 4 in HDR). All three want sRGB->linear.
inline bool needs_srgb_conversion(VkFormat fmt, VkColorSpaceKHR cs) {
    if (is_srgb_format(fmt)) return true;
    if (cs == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) return true;
    if (cs == VK_COLOR_SPACE_PASS_THROUGH_EXT && is_hdr_float_format(fmt)) return true;
    return false;
}

// True for HDR color spaces whose transfer function we do NOT yet handle (PQ / HLG): the
// overlay's sRGB colors will be wrong on these until we add proper conversion. Used only
// for diagnostics/logging — we never apply the sRGB transfer to these.
inline bool is_unhandled_hdr(VkColorSpaceKHR cs) {
    return cs == VK_COLOR_SPACE_HDR10_ST2084_EXT || cs == VK_COLOR_SPACE_HDR10_HLG_EXT ||
           cs == VK_COLOR_SPACE_DOLBYVISION_EXT;
}

// Short name for a color space (diagnostics).
inline const char* colorspace_name(VkColorSpaceKHR cs) {
    switch (cs) {
        case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:          return "SRGB_NONLINEAR";
        case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:    return "EXTENDED_SRGB_LINEAR(scRGB)";
        case VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT: return "EXTENDED_SRGB_NONLINEAR";
        case VK_COLOR_SPACE_HDR10_ST2084_EXT:            return "HDR10_ST2084(PQ)";
        case VK_COLOR_SPACE_HDR10_HLG_EXT:               return "HDR10_HLG";
        case VK_COLOR_SPACE_BT2020_LINEAR_EXT:           return "BT2020_LINEAR";
        case VK_COLOR_SPACE_DOLBYVISION_EXT:             return "DOLBYVISION";
        case VK_COLOR_SPACE_PASS_THROUGH_EXT:            return "PASS_THROUGH";
        default:                                         return "other";
    }
}

}  // namespace choir
