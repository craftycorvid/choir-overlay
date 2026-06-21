// Unit tests for the per-participant opacity easing (src/overlay/fade.hpp).
//
// The voice panel dims non-speaking indicators and quickly fades them up while a
// participant speaks. fade.hpp factors the framerate-independent easing out as a pure
// function so we can test it deterministically here (no ImGui/Vulkan/pixels).

#include "fade.hpp"

#include <cassert>

using choir::fade_toward;

int main() {
    // dt <= 0: no time passed -> value is unchanged (exact).
    assert(fade_toward(0.4f, 1.0f, 0.0f, 0.05f) == 0.4f);
    assert(fade_toward(0.4f, 1.0f, -0.1f, 0.05f) == 0.4f);

    // tau <= 0: snap straight to the target (exact).
    assert(fade_toward(0.4f, 1.0f, 0.016f, 0.0f) == 1.0f);

    // Quick fade UP: idle 0.4 -> 1.0 with a small tau is nearly complete in ~0.15s.
    {
        float a = 0.4f;
        const float tau = 0.05f, dt = 1.0f / 120.0f;  // 120 fps
        for (int i = 0; i < 18; ++i) a = fade_toward(a, 1.0f, dt, tau);  // ~0.15s
        assert(a > 0.95f && a <= 1.0f);
    }

    // Fade up is monotonic non-decreasing and never overshoots the target.
    {
        float a = 0.4f, prev = a;
        for (int i = 0; i < 300; ++i) {
            a = fade_toward(a, 1.0f, 0.016f, 0.05f);
            assert(a >= prev - 1e-6f);
            assert(a <= 1.0f + 1e-6f);
            prev = a;
        }
        assert(a > 0.999f);  // converges to ~full
    }

    // Fade down (gentler tau) is monotonic non-increasing and never undershoots idle.
    {
        float a = 1.0f, prev = a;
        for (int i = 0; i < 600; ++i) {
            a = fade_toward(a, 0.4f, 0.016f, 0.18f);
            assert(a <= prev + 1e-6f);
            assert(a >= 0.4f - 1e-6f);
            prev = a;
        }
        assert(a < 0.41f);  // converges to ~idle
    }

    // For equal elapsed time, the up-ease (smaller tau) covers more of its range than the
    // down-ease — i.e. the fade up is genuinely quicker than the fade down.
    {
        const float up = fade_toward(0.4f, 1.0f, 0.05f, 0.05f);
        const float down = fade_toward(1.0f, 0.4f, 0.05f, 0.18f);
        const float up_frac = (up - 0.4f) / 0.6f;
        const float down_frac = (1.0f - down) / 0.6f;
        assert(up_frac > down_frac);
    }

    return 0;
}
