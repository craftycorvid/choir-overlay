// Avatar texture cache for the Choir layer (Task 16).
//
// Turns the host's AvatarReady files (on-disk RGBA8, see ipc/avatar_file.hpp) into
// Vulkan textures usable by ImGui as an ImTextureID, keyed by avatar hash.
//
// RENDER-THREAD ONLY. Every method touches the Vulkan device and/or the ImGui
// backend, so it MUST be called from the present hook (swapchain.cpp), under the
// swapchain lock, with the owning ImguiRenderer ready(). The client thread only
// enqueues AvatarReq{hash,path,w,h}; it never calls in here.
//
// Lifetime: owned per swapchain alongside the ImguiRenderer (it uses that backend's
// descriptor pool + the same device). Must be torn down (shutdown()) BEFORE the
// ImguiRenderer/device are destroyed — swapchain.cpp's destroy_state() does this.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <unordered_map>

#include "imgui.h"            // ImTextureID
#include "state_client.hpp"   // AvatarReq

namespace choir {

class ImguiRenderer;

class AvatarTextures {
public:
    AvatarTextures() = default;
    ~AvatarTextures();

    AvatarTextures(const AvatarTextures&) = delete;
    AvatarTextures& operator=(const AvatarTextures&) = delete;

    // Bind to the ImguiRenderer that owns the device + descriptor pool. Call once,
    // after the renderer is ready(). Cheap; no Vulkan calls here. `srgb` must match
    // the swapchain's color space: when true, avatar images use an _SRGB format so the
    // sampler decodes them and the sRGB attachment's store-encode reproduces the source.
    void init(ImguiRenderer* renderer, bool srgb);

    // Load (or return the cached) texture for `req`. Reads req.path via
    // read_avatar_rgba, uploads a sampled VkImage (staging buffer + one-shot copy,
    // submit + fence wait), registers it with the ImGui backend, and caches by hash.
    // Returns the ImTextureID, or 0 (ImTextureID_Invalid) on any failure — never
    // throws, never crashes the game.
    ImTextureID get_or_load(const AvatarReq& req);

    // Look up an already-loaded texture by hash (for Task 17's panel). Returns 0 if
    // not loaded.
    ImTextureID lookup(const std::string& hash) const;

    // How many textures are cached (for tests / diagnostics).
    size_t size() const { return textures_.size(); }

    // Destroy all textures (idles the device first) and unregister their ImGui
    // descriptors. Safe to call multiple times. MUST run before the renderer/device
    // are destroyed.
    void shutdown();

private:
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;  // == ImTextureID
    };

    // Create the Vulkan texture from decoded RGBA pixels. On any failure, frees what
    // it built and returns false (leaving `t` zeroed).
    bool create_texture(uint32_t w, uint32_t h, const uint8_t* rgba, Texture& t);
    void destroy_texture(Texture& t);

    ImguiRenderer* renderer_ = nullptr;
    bool srgb_ = false;  // use an _SRGB image format (matches the swapchain color space)
    std::unordered_map<std::string, Texture> textures_;
};

}  // namespace choir
