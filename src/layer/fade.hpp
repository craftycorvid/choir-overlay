#pragma once

// Per-participant opacity easing for the voice panel.
//
// Non-speaking indicators sit at a dim baseline; when a participant starts speaking
// their indicator quickly fades up to full, and eases back down when they stop. The
// math is a framerate-independent exponential ease, factored out here as a pure
// function so it can be unit-tested without ImGui/Vulkan.

#include <cmath>

namespace choir {

// Ease `current` toward `target` over `dt` seconds with time-constant `tau` (seconds).
// Smaller tau = faster approach. Framerate-independent (uses 1 - e^(-dt/tau)).
//   dt <= 0  -> no time passed, returns `current` unchanged.
//   tau <= 0 -> snap straight to `target`.
// The result never overshoots `target` (the easing factor is in [0,1]).
inline float fade_toward(float current, float target, float dt, float tau) {
    if (dt <= 0.0f) return current;
    if (tau <= 0.0f) return target;
    const float k = 1.0f - std::exp(-dt / tau);  // in (0,1]
    return current + (target - current) * k;
}

}  // namespace choir
