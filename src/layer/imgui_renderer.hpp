// Dear ImGui Vulkan-backend wrapper for the Choir layer (Task 15).
//
// Wraps ImGui + imgui_impl_vulkan against the layer's own device/queue/render pass
// so the present hook can draw the overlay into the swapchain image. There is no
// platform/input backend: we drive frames manually, setting io.DisplaySize from the
// swapchain extent each present (no ImGui_ImplXxx_NewFrame for any windowing system).
//
// Lifecycle (owned by swapchain.cpp, one renderer per swapchain):
//   init(...)          once, on the first present of a swapchain (we have the render
//                      pass + image count by then). Builds a descriptor pool and
//                      initializes the ImGui Vulkan backend against the layer's funcs.
//   begin_frame(ext)   per present, BEFORE vkCmdBeginRenderPass (NewFrame records no
//                      GPU commands): sets io.DisplaySize, builds the ImGui draw list.
//   end_frame(cmd)     per present, INSIDE the active render pass: records the ImGui
//                      draw data into `cmd`.
//   shutdown()         on swapchain destroy/recreate (the render pass + image count
//                      can change, so the backend must be rebuilt).
//
// Robustness: every entry point is a no-op when not ready(), so a failed init never
// crashes the game — the present hook simply draws nothing and still presents.
//
// ImGui is NOT thread-safe. All calls happen on the single present thread under the
// swapchain lock held by swapchain.cpp.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

#include "swapchain_color.hpp"  // TransferFunction

// Forward declaration so the header stays free of the ImGui include.
struct ImGuiContext;

namespace choir {

struct DeviceDispatch;  // src/layer/dispatch.hpp
struct Snapshot;        // ipc/state.hpp
class AvatarTextures;   // src/layer/avatar_textures.hpp
class StateClient;      // src/overlay/state_client.hpp

class ImguiRenderer {
public:
    ImguiRenderer() = default;
    ~ImguiRenderer();

    ImguiRenderer(const ImguiRenderer&) = delete;
    ImguiRenderer& operator=(const ImguiRenderer&) = delete;

    // Initialize ImGui + the Vulkan backend against the layer's device/queue/render
    // pass. `api_version` is the instance's apiVersion (passed to the backend; only
    // used by it for dynamic-rendering fn loading, which we do not use). `gipa` is the
    // layer's next-down vkGetInstanceProcAddr — the vendored ImGui Vulkan backend is
    // built with VK_NO_PROTOTYPES, so we feed it function pointers resolved through
    // `gipa`/`disp.GetDeviceProcAddr` (i.e. the layer's own chain) rather than the
    // global loader, whose trampolines reject the unwrapped handles a layer sees.
    // Returns true on success; on failure the renderer stays not-ready() and tears
    // down cleanly.
    // `transfer` is the swapchain's color transfer function (see swapchain_color.hpp) and
    // `nits` the HDR paper-white target. When `transfer` is not None, the renderer draws
    // through a custom HDR pipeline whose fragment shader applies the transfer to the final
    // color (sRGB->linear, + nits scale for scRGB, + BT.2020 + PQ/HLG for HDR10) so it
    // displays correctly. None uses ImGui's default pipeline unchanged.
    bool init(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t queue_family,
              VkQueue queue, VkRenderPass render_pass, uint32_t image_count,
              uint32_t api_version, TransferFunction transfer, float nits,
              PFN_vkGetInstanceProcAddr gipa, const DeviceDispatch& disp);

    // Begin and build a frame for the given framebuffer extent: NewFrame -> draw the
    // real overlay (voice panel + toasts) from `snap` -> Render. No platform backend,
    // no GPU commands recorded (Render only builds draw lists; end_frame records them).
    //
    // `snap` is the latest published snapshot (may be null if the host hasn't sent one);
    // when null or !snap->in_voice nothing visible is drawn (an empty frame is built so
    // end_frame is still balanced). `textures`/`client` resolve avatar textures by hash;
    // `now_ms` is a wall-clock ms timestamp for toast expiry.
    void begin_frame(VkExtent2D extent, const Snapshot* snap, AvatarTextures& textures,
                     StateClient& client, int64_t now_ms);

    // Record the current ImGui draw data into `cmd`. MUST be called inside the active
    // render pass (between vkCmdBeginRenderPass / vkCmdEndRenderPass). The font atlas
    // (and any user textures) upload lazily on the first call, via the backend's own
    // command buffer/queue — independent of `cmd`.
    void end_frame(VkCommandBuffer cmd);

    // Tear down the ImGui Vulkan backend + descriptor pool + ImGui context. Safe to
    // call when not initialized. The caller must have idled the device first (the
    // swapchain teardown path already does DeviceWaitIdle).
    void shutdown();

    bool ready() const { return init_done_; }

    // --- Avatar-texture support (Task 16). Called only on the render thread, with
    // this renderer ready() and the same ImGui context active. ---

    // The device this backend was initialized against (AvatarTextures creates its
    // VkImage/buffer/sampler on it). VK_NULL_HANDLE before init.
    VkDevice device() const { return device_; }

    // The layer's device dispatch (null before init). AvatarTextures uses it for all
    // its Vulkan calls (it never touches the global loader).
    const DeviceDispatch* dispatch() const { return disp_; }

    // Register a sampled image with the ImGui Vulkan backend, returning the
    // VkDescriptorSet usable as an ImTextureID. Sets the ImGui context current first
    // (the backend reads the current context). Returns VK_NULL_HANDLE if not ready().
    VkDescriptorSet add_texture(VkSampler sampler, VkImageView view, VkImageLayout layout);

    // Free a descriptor set previously returned by add_texture(). No-op when not
    // ready() (the whole backend is gone, so the set is gone with it).
    void remove_texture(VkDescriptorSet set);

private:
    bool init_done_ = false;
    bool frame_started_ = false;  // begin_frame called this present, end_frame pending
    TransferFunction transfer_ = TransferFunction::None;  // swapchain color transfer fn
    float nits_ = 200.0f;
    VkPipeline hdr_pipeline_ = VK_NULL_HANDLE;
    ImGuiContext* ctx_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    const DeviceDispatch* disp_ = nullptr;
};

}  // namespace choir
