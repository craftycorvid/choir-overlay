// Unit tests for the swapchain color-space classification (src/layer/swapchain_color.hpp).
//
// Whether the overlay pre-converts its sRGB colors to linear depends on BOTH the format
// and the color space. Getting this wrong is what made Diablo 4 (HDR scRGB) look
// over-saturated. These are pure, device-free checks of that decision.

#include "swapchain_color.hpp"

#include <cassert>

using choir::is_srgb_format;
using choir::is_unhandled_hdr;
using choir::needs_srgb_conversion;

int main() {
    // 8-bit _SRGB formats need conversion (hardware re-encodes on store).
    assert(is_srgb_format(VK_FORMAT_B8G8R8A8_SRGB));
    assert(is_srgb_format(VK_FORMAT_R8G8B8A8_SRGB));
    assert(!is_srgb_format(VK_FORMAT_B8G8R8A8_UNORM));
    assert(!is_srgb_format(VK_FORMAT_R16G16B16A16_SFLOAT));

    // Plain SDR: UNORM + SRGB_NONLINEAR -> ImGui's sRGB colors are already correct, no
    // conversion (this is the Overwatch-style path that must stay untouched).
    assert(!needs_srgb_conversion(VK_FORMAT_B8G8R8A8_UNORM,
                                  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR));

    // _SRGB format (any color space) -> convert.
    assert(needs_srgb_conversion(VK_FORMAT_B8G8R8A8_SRGB,
                                 VK_COLOR_SPACE_SRGB_NONLINEAR_KHR));

    // scRGB-linear HDR (float buffer, no store-encode) -> convert. This is the Diablo-4
    // case: a linear buffer that must receive linear values.
    assert(needs_srgb_conversion(VK_FORMAT_R16G16B16A16_SFLOAT,
                                 VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT));

    // scRGB-NONLINEAR expects sRGB-encoded values already -> NO conversion.
    assert(!needs_srgb_conversion(VK_FORMAT_R16G16B16A16_SFLOAT,
                                  VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT));

    // HDR10 PQ uses a different transfer function we don't handle: don't apply sRGB, and
    // flag it as unhandled so the layer logs it.
    assert(!needs_srgb_conversion(VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                  VK_COLOR_SPACE_HDR10_ST2084_EXT));
    assert(is_unhandled_hdr(VK_COLOR_SPACE_HDR10_ST2084_EXT));
    assert(is_unhandled_hdr(VK_COLOR_SPACE_HDR10_HLG_EXT));
    assert(!is_unhandled_hdr(VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT));
    assert(!is_unhandled_hdr(VK_COLOR_SPACE_SRGB_NONLINEAR_KHR));

    return 0;
}
