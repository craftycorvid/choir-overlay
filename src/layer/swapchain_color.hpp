// Swapchain color-space handling for the Choir overlay layer.
//
// ImGui authors its colors in sRGB. What we must do to those colors before writing them
// into the swapchain depends on the swapchain's color space — this mirrors MangoHud's
// convert_colors_vk (src/vulkan.cpp) + convert_colors (src/hud_elements.cpp) EXACTLY:
//
//   color space                       transfer function   what we apply to each RGB color
//   --------------------------------- ------------------- --------------------------------
//   HDR10_ST2084 (PQ)                 PQ                  sRGB->linear, BT.709->BT.2020, PQ
//   HDR10_HLG                         HLG                 sRGB->linear, BT.709->BT.2020, HLG
//   EXTENDED_SRGB_LINEAR (scRGB)      ScRgb               sRGB->linear, scale by nits/80
//   PASS_THROUGH + float format       ScRgb               sRGB->linear, scale by nits/80
//   anything else (incl. PASS_THROUGH) NONE / SRGB        none, unless the FORMAT is _SRGB
//                                                          (hardware re-encodes) -> sRGB->linear
//
// The transfer math (srgb_to_linear / bt709_to_bt2020 / linear_to_pq / linear_to_hlg) is
// ported verbatim from MangoHud's src/hud_elements.cpp.
#pragma once

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdint>

namespace choir {

enum class TransferFunction { None, Srgb, ScRgb, Pq, Hlg };

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

// True for HDR floating-point swapchain formats (used to detect scRGB under PASS_THROUGH).
inline bool is_hdr_float_format(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R16G16B16_SFLOAT:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            return true;
        default:
            return false;
    }
}

// Pick the transfer function for a swapchain (MangoHud convert_colors_vk order: color
// space first, then the _SRGB-format fallback).
inline TransferFunction transfer_function_for(VkFormat fmt, VkColorSpaceKHR cs) {
    switch (cs) {
        case VK_COLOR_SPACE_HDR10_ST2084_EXT:         return TransferFunction::Pq;
        case VK_COLOR_SPACE_HDR10_HLG_EXT:            return TransferFunction::Hlg;
        case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: return TransferFunction::ScRgb;
        case VK_COLOR_SPACE_PASS_THROUGH_EXT:
            return is_hdr_float_format(fmt) ? TransferFunction::ScRgb : TransferFunction::None;
        default: break;
    }
    return is_srgb_format(fmt) ? TransferFunction::Srgb : TransferFunction::None;
}

// --- MangoHud color math (src/hud_elements.cpp), ported verbatim ----------------------
inline float srgb_to_linear(float in) {
    return in <= 0.04045f ? in / 12.92f : std::pow((in + 0.055f) / 1.055f, 2.4f);
}

// BT.709 -> BT.2020 primaries (applied to linear values). MangoHud's SRGBtoBT2020 matrix.
inline void bt709_to_bt2020(float& x, float& y, float& z) {
    const float r = 0.627392f * x + 0.329030f * y + 0.0432691f * z;
    const float g = 0.069123f * x + 0.919523f * y + 0.0113204f * z;
    const float b = 0.016423f * x + 0.088042f * y + 0.8956166f * z;
    x = r; y = g; z = b;
}

inline float linear_to_pq(float in, float targetL) {
    const float m1 = 0.1593017578125f, m2 = 78.84375f;
    const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    in = std::pow(in * (targetL / 10000.0f), m1);
    in = (c1 + c2 * in) / (1.0f + c3 * in);
    return std::pow(in, m2);
}

inline float linear_to_hlg(float in) {
    const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    return in <= 1.0f / 12.0f ? std::sqrt(3.0f * in) : a * std::log(12.0f * in - b) + c;
}

// Transform one sRGB-authored RGB triple (0..1) into what the swapchain expects.
// nits is the target paper-white luminance in cd/m^2 (used by ScRgb and Pq).
inline void apply_transfer(TransferFunction tf, float nits, float& r, float& g, float& b) {
    if (tf == TransferFunction::None) return;
    r = srgb_to_linear(r); g = srgb_to_linear(g); b = srgb_to_linear(b);
    if (tf == TransferFunction::Srgb) return;
    if (tf == TransferFunction::ScRgb) {
        const float s = nits / 80.0f;
        r *= s; g *= s; b *= s;
        return;
    }
    bt709_to_bt2020(r, g, b);
    if (tf == TransferFunction::Pq) {
        r = linear_to_pq(r, nits); g = linear_to_pq(g, nits); b = linear_to_pq(b, nits);
    } else {  // Hlg
        r = linear_to_hlg(r); g = linear_to_hlg(g); b = linear_to_hlg(b);
    }
}

inline const char* transfer_name(TransferFunction tf) {
    switch (tf) {
        case TransferFunction::Srgb:  return "srgb";
        case TransferFunction::ScRgb: return "scrgb";
        case TransferFunction::Pq:    return "pq";
        case TransferFunction::Hlg:   return "hlg";
        default:                      return "none";
    }
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
