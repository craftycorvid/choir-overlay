// Backend-agnostic seam between the shared overlay drawing (overlay_ui.cpp) and a
// concrete GPU texture cache. The Vulkan layer (AvatarTextures) and the GL interposer
// (GlAvatarTextures) each implement this; overlay_ui only needs to resolve an avatar
// hash to an opaque ImTextureID, so the interface is exactly those two calls.
#pragma once

#include <cstdint>
#include <string>

#include "imgui.h"            // ImTextureID
#include "state_client.hpp"   // AvatarReq

namespace choir {

// Framebuffer extent in pixels. Replaces VkExtent2D in the shared overlay code so it
// carries no Vulkan dependency. Field names match VkExtent2D for drop-in call sites.
struct Extent2D {
    uint32_t width = 0;
    uint32_t height = 0;
};

// Resolve avatar hashes to textures. RENDER-THREAD ONLY (touches the GPU/ImGui backend).
class IAvatarTextures {
public:
    virtual ~IAvatarTextures() = default;
    // Load (or return cached) texture for `req`; ImTextureID_Invalid on any failure.
    virtual ImTextureID get_or_load(const AvatarReq& req) = 0;
    // Already-loaded texture for `hash`, or ImTextureID_Invalid if not loaded.
    virtual ImTextureID lookup(const std::string& hash) const = 0;
};

}  // namespace choir
