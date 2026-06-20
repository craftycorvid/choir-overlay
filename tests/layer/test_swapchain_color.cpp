// Unit tests for the swapchain color transfer model (src/layer/swapchain_color.hpp).
//
// Mirrors MangoHud: the transfer function is chosen from the color space (then the _SRGB
// format fallback), and the per-color math matches MangoHud's convert_colors. PASS_THROUGH
// is deliberately NOT special-cased — it falls through to None.

#include "swapchain_color.hpp"

#include <cassert>
#include <cmath>

using choir::apply_transfer;
using choir::is_srgb_format;
using choir::TransferFunction;
using choir::transfer_function_for;

static bool approx(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }

int main() {
    // --- Transfer-function selection (MangoHud convert_colors_vk) ---
    assert(transfer_function_for(VK_FORMAT_R16G16B16A16_SFLOAT,
                                 VK_COLOR_SPACE_HDR10_ST2084_EXT) == TransferFunction::Pq);
    assert(transfer_function_for(VK_FORMAT_R16G16B16A16_SFLOAT,
                                 VK_COLOR_SPACE_HDR10_HLG_EXT) == TransferFunction::Hlg);
    assert(transfer_function_for(VK_FORMAT_R16G16B16A16_SFLOAT,
                                 VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) == TransferFunction::Srgb);
    // _SRGB format on a plain color space -> Srgb (hardware re-encodes on store).
    assert(transfer_function_for(VK_FORMAT_B8G8R8A8_SRGB,
                                 VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) == TransferFunction::Srgb);
    // Plain SDR (UNORM + SRGB_NONLINEAR) -> None (Overwatch-in-SDR style; must stay None).
    assert(transfer_function_for(VK_FORMAT_B8G8R8A8_UNORM,
                                 VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) == TransferFunction::None);
    // PASS_THROUGH is NOT special-cased -> None, exactly like MangoHud.
    assert(transfer_function_for(VK_FORMAT_R16G16B16A16_SFLOAT,
                                 VK_COLOR_SPACE_PASS_THROUGH_EXT) == TransferFunction::None);

    assert(is_srgb_format(VK_FORMAT_B8G8R8A8_SRGB));
    assert(!is_srgb_format(VK_FORMAT_R16G16B16A16_SFLOAT));

    // --- Transfer math (endpoints, ported from MangoHud) ---
    {
        // None: identity.
        float r = 0.5f, g = 0.3f, b = 0.7f;
        apply_transfer(TransferFunction::None, r, g, b);
        assert(r == 0.5f && g == 0.3f && b == 0.7f);
    }
    {
        // Srgb: sRGB->linear. 0->0, 1->1, and it darkens mids (0.5 sRGB ~= 0.214 linear).
        float r = 0.0f, g = 1.0f, b = 0.5f;
        apply_transfer(TransferFunction::Srgb, r, g, b);
        assert(approx(r, 0.0f));
        assert(approx(g, 1.0f));
        assert(approx(b, 0.214f, 5e-3f));
    }
    {
        // Pq: black stays black; white maps to a bounded value in (0,1) (peaks at 200 nits,
        // far below the PQ 10000-nit ceiling, so white < 1.0).
        float r = 0.0f, g = 0.0f, b = 0.0f;
        apply_transfer(TransferFunction::Pq, r, g, b);
        assert(approx(r, 0.0f, 5e-3f) && approx(g, 0.0f, 5e-3f) && approx(b, 0.0f, 5e-3f));
        float wr = 1.0f, wg = 1.0f, wb = 1.0f;
        apply_transfer(TransferFunction::Pq, wr, wg, wb);
        assert(wr > 0.3f && wr < 0.8f);  // ~0.57 (200/10000 nits PQ)
    }
    {
        // Hlg: black stays black; white is bounded in (0,1).
        float r = 0.0f, g = 0.0f, b = 0.0f;
        apply_transfer(TransferFunction::Hlg, r, g, b);
        assert(approx(r, 0.0f, 5e-3f));
        float wr = 1.0f, wg = 1.0f, wb = 1.0f;
        apply_transfer(TransferFunction::Hlg, wr, wg, wb);
        assert(wr > 0.5f && wr <= 1.0f);
    }

    return 0;
}
