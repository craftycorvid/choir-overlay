// Unit tests for the sRGB<->linear 8-bit conversions (src/layer/srgb.hpp).
//
// The layer pre-converts ImGui colors sRGB->linear when rendering into an _SRGB
// swapchain so the hardware's linear->sRGB encode on store reproduces the authored
// color. The load-bearing property is the round-trip: linear_to_srgb(srgb_to_linear(c))
// must recover c (within 8-bit rounding).

#include "srgb.hpp"

#include <cassert>
#include <cstdlib>

using choir::linear_to_srgb_u8;
using choir::srgb_to_linear_u8;

int main() {
    // Endpoints are exact in both directions.
    assert(srgb_to_linear_u8(0) == 0);
    assert(srgb_to_linear_u8(255) == 255);
    assert(linear_to_srgb_u8(0) == 0);
    assert(linear_to_srgb_u8(255) == 255);

    // Both transfer functions are monotonic non-decreasing.
    for (int i = 1; i < 256; ++i) {
        assert(srgb_to_linear_u8(static_cast<uint8_t>(i)) >=
               srgb_to_linear_u8(static_cast<uint8_t>(i - 1)));
        assert(linear_to_srgb_u8(static_cast<uint8_t>(i)) >=
               linear_to_srgb_u8(static_cast<uint8_t>(i - 1)));
    }

    // sRGB->linear darkens mid-tones (sRGB brightens darks), so result <= input,
    // and is strictly smaller somewhere in the mid-range.
    bool strictly_smaller_somewhere = false;
    for (int i = 0; i < 256; ++i) {
        const uint8_t c = static_cast<uint8_t>(i);
        assert(srgb_to_linear_u8(c) <= c);
        if (srgb_to_linear_u8(c) < c) strictly_smaller_somewhere = true;
    }
    assert(strictly_smaller_somewhere);

    // Round-trip = encode(decode(c)) — what the GPU does on store after our pre-convert.
    // 8-bit linear has very few codes in deep shadow, so near-black values lose precision
    // (a known limitation; avatars avoid it via a full-precision _SRGB image format). For
    // the bright/vivid range the overlay actually uses (>= 32) the round-trip is within ±2.
    for (int i = 0; i < 256; ++i) {
        const uint8_t c = static_cast<uint8_t>(i);
        const int rt = linear_to_srgb_u8(srgb_to_linear_u8(c));
        assert(std::abs(rt - i) <= 12);                 // bounded everywhere (catch gross bugs)
        if (i >= 32) assert(std::abs(rt - i) <= 2);     // accurate where it matters
    }

    // A concrete sanity point: a vivid green channel (230) decodes well below itself.
    assert(srgb_to_linear_u8(230) < 220);

    return 0;
}
