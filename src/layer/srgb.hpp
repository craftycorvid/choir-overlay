#pragma once

// sRGB <-> linear 8-bit conversions (256-entry LUTs), factored out as pure functions
// so they can be unit-tested without ImGui/Vulkan.
//
// Why: a game's swapchain image can be an _SRGB format. ImGui authors colors as sRGB
// 8-bit values, but the GPU treats the fragment-shader output as LINEAR and applies the
// sRGB transfer function (encode) when storing to an _SRGB attachment. Without
// compensation the overlay looks too bright / over-saturated on those games (and fine
// on UNORM ones). Pre-converting our colors sRGB->linear makes the hardware's
// linear->sRGB encode on store reproduce the originally-authored color. linear_to_srgb
// is the inverse (used by tests to assert the round-trip).

#include <array>
#include <cmath>
#include <cstdint>

namespace choir {

// sRGB(8-bit) -> linear(8-bit), standard sRGB EOTF.
inline uint8_t srgb_to_linear_u8(uint8_t c) {
    static const std::array<uint8_t, 256> lut = [] {
        std::array<uint8_t, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float s = static_cast<float>(i) / 255.0f;
            const float l = (s <= 0.04045f) ? (s / 12.92f)
                                            : std::pow((s + 0.055f) / 1.055f, 2.4f);
            const int v = static_cast<int>(l * 255.0f + 0.5f);
            t[i] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        return t;
    }();
    return lut[c];
}

// linear(8-bit) -> sRGB(8-bit), inverse of the above (the hardware store encode).
inline uint8_t linear_to_srgb_u8(uint8_t c) {
    static const std::array<uint8_t, 256> lut = [] {
        std::array<uint8_t, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float l = static_cast<float>(i) / 255.0f;
            const float s = (l <= 0.0031308f) ? (l * 12.92f)
                                              : (1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f);
            const int v = static_cast<int>(s * 255.0f + 0.5f);
            t[i] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        return t;
    }();
    return lut[c];
}

}  // namespace choir
