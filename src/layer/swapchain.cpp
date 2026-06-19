#include "swapchain.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "avatar_textures.hpp"
#include "dispatch.hpp"
#include "gating.hpp"
#include "imgui_renderer.hpp"
#include "state_client.hpp"
#include "swapchain_color.hpp"
#include "swapchain_usage.hpp"

namespace choir {
namespace {

// Per-swapchain-image overlay resources. One command buffer + fence + semaphore
// per image so a buffer still in flight (FIFO can have several frames queued) is
// never re-recorded before its prior submit finishes.
struct ImageState {
    VkImage image = VK_NULL_HANDLE;       // owned by the swapchain, not us
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;  // freed with the pool
    VkFence fence = VK_NULL_HANDLE;        // created signaled
    VkSemaphore overlay_done = VK_NULL_HANDLE;
    // True once a QueueSubmit has been issued with `fence` and not yet waited.
    // The fence is created signaled but nothing is queued on it, so this starts
    // false: we must NOT wait on a fence that was never (or not yet) submitted,
    // or a failed/aborted submit would leave it unsignaled and deadlock the next
    // present of this image. Only wait when this is true; only set it true after
    // a successful submit; reset the fence immediately before that submit.
    bool fence_in_flight = false;
};

// Everything we build per swapchain. `ok` gates the present-time draw: if creation
// partially failed we tear down and never touch this swapchain again (present falls
// back to the real call).
struct SwapchainState {
    VkDevice device = VK_NULL_HANDLE;
    DeviceData* dd = nullptr;  // stable for device lifetime; entry lives in dispatch map
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{0, 0};
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<ImageState> images;
    bool ok = false;
    // ImGui renderer for this swapchain (Task 15). Lazily init()'d on the first
    // present (we have the render pass + image count by then). Held by unique_ptr so
    // SwapchainState stays movable (the map move-assigns on insert/recreate) and so a
    // failed/absent renderer is simply null. Torn down with the swapchain.
    std::unique_ptr<ImguiRenderer> imgui;
    bool imgui_tried = false;  // init attempted once; don't retry every present
    // Avatar texture cache for this swapchain (Task 16). Same lifetime as `imgui` (it
    // uses that backend's descriptor pool + device). Created when the renderer first
    // becomes ready; torn down in destroy_state BEFORE the renderer/device go away.
    std::unique_ptr<AvatarTextures> avatars;
};

std::mutex g_sc_lock;
std::unordered_map<VkSwapchainKHR, SwapchainState> g_swapchains;

// Tear down all overlay Vulkan objects for one swapchain. Caller holds g_sc_lock.
// The device must be idle (or all fences waited) before this runs. Does NOT call
// the down-chain vkDestroySwapchainKHR — that is the app's swapchain, freed by the
// real DestroySwapchainKHR / by recreate.
void destroy_state(SwapchainState& s) {
    const DeviceDispatch& d = s.dd->disp;
    VkDevice dev = s.device;
    // Tear avatar textures down BEFORE the ImGui backend: they unregister their
    // descriptors via the renderer (RemoveTexture) and free images/views/samplers/
    // memory through the device. Callers idle the device before destroy_state, so the
    // textures are not in flight. After this the renderer can drop its descriptor pool.
    if (s.avatars) {
        s.avatars->shutdown();
        s.avatars.reset();
    }
    // Tear the ImGui backend down (callers idle the device before destroy_state, so
    // its descriptors/pipelines are safe to free). Frees its descriptor pool too.
    if (s.imgui) {
        s.imgui->shutdown();
        s.imgui.reset();
    }
    s.imgui_tried = false;
    for (ImageState& img : s.images) {
        if (img.overlay_done && d.DestroySemaphore) d.DestroySemaphore(dev, img.overlay_done, nullptr);
        if (img.fence && d.DestroyFence) d.DestroyFence(dev, img.fence, nullptr);
        if (img.framebuffer && d.DestroyFramebuffer) d.DestroyFramebuffer(dev, img.framebuffer, nullptr);
        if (img.view && d.DestroyImageView) d.DestroyImageView(dev, img.view, nullptr);
        // img.cmd is freed implicitly with the pool below.
    }
    s.images.clear();
    if (s.pool && d.DestroyCommandPool) d.DestroyCommandPool(dev, s.pool, nullptr);
    s.pool = VK_NULL_HANDLE;
    if (s.render_pass && d.DestroyRenderPass) d.DestroyRenderPass(dev, s.render_pass, nullptr);
    s.render_pass = VK_NULL_HANDLE;
    s.ok = false;
}

// Build all overlay state for a freshly created swapchain. On any failure we tear
// down what we built and leave s.ok == false (present will fall back). Returns
// s.ok. Assumes the down-chain swapchain already exists.
bool build_state(SwapchainState& s, VkSwapchainKHR swapchain) {
    const DeviceDispatch& d = s.dd->disp;
    VkDevice dev = s.device;

    // Every dispatch func we touch must be present (the loader preloads *KHR
    // entrypoints in Task 13, but a stripped ICD could leave some null).
    if (!d.GetSwapchainImagesKHR || !d.CreateImageView || !d.DestroyImageView ||
        !d.CreateRenderPass || !d.DestroyRenderPass || !d.CreateFramebuffer ||
        !d.DestroyFramebuffer || !d.CreateCommandPool || !d.DestroyCommandPool ||
        !d.AllocateCommandBuffers || !d.CreateFence || !d.DestroyFence ||
        !d.CreateSemaphore || !d.DestroySemaphore || !d.BeginCommandBuffer ||
        !d.EndCommandBuffer || !d.CmdBeginRenderPass || !d.CmdEndRenderPass ||
        !d.CmdClearAttachments || !d.WaitForFences || !d.ResetFences ||
        !d.QueueSubmit) {
        return false;
    }
    if (!s.dd->has_graphics_queue_family) return false;

    // --- Swapchain images ---
    uint32_t count = 0;
    if (d.GetSwapchainImagesKHR(dev, swapchain, &count, nullptr) != VK_SUCCESS || count == 0)
        return false;
    std::vector<VkImage> images(count);
    if (d.GetSwapchainImagesKHR(dev, swapchain, &count, images.data()) != VK_SUCCESS)
        return false;

    // --- Render pass: single color attachment, LOAD to preserve the app's frame.
    // Layouts match MangoHud's overlay pass (setup_swapchain_data): initialLayout
    // COLOR_ATTACHMENT_OPTIMAL (the image is treated as already a render target — no
    // transition in), finalLayout PRESENT_SRC_KHR so it is presentable after the pass. A
    // single EXTERNAL->subpass dependency orders our color writes after the app's. ---
    VkAttachmentDescription att{};
    att.format = s.format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    if (d.CreateRenderPass(dev, &rpci, nullptr, &s.render_pass) != VK_SUCCESS) {
        s.render_pass = VK_NULL_HANDLE;
        return false;
    }

    // --- Command pool on the graphics queue family ---
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = s.dd->graphics_queue_family;
    if (d.CreateCommandPool(dev, &pci, nullptr, &s.pool) != VK_SUCCESS) {
        s.pool = VK_NULL_HANDLE;
        d.DestroyRenderPass(dev, s.render_pass, nullptr);
        s.render_pass = VK_NULL_HANDLE;
        return false;
    }

    std::vector<VkCommandBuffer> cmds(count);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = s.pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = count;
    if (d.AllocateCommandBuffers(dev, &cbai, cmds.data()) != VK_SUCCESS) {
        destroy_state(s);
        return false;
    }

    // --- Per-image view, framebuffer, fence (signaled), overlay-done semaphore ---
    s.images.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        ImageState& img = s.images[i];
        img.image = images[i];
        img.cmd = cmds[i];

        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = s.format;
        ivci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (d.CreateImageView(dev, &ivci, nullptr, &img.view) != VK_SUCCESS) {
            img.view = VK_NULL_HANDLE;
            destroy_state(s);
            return false;
        }

        VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbci.renderPass = s.render_pass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &img.view;
        fbci.width = s.extent.width;
        fbci.height = s.extent.height;
        fbci.layers = 1;
        if (d.CreateFramebuffer(dev, &fbci, nullptr, &img.framebuffer) != VK_SUCCESS) {
            img.framebuffer = VK_NULL_HANDLE;
            destroy_state(s);
            return false;
        }

        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // first wait+reset must not block
        if (d.CreateFence(dev, &fci, nullptr, &img.fence) != VK_SUCCESS) {
            img.fence = VK_NULL_HANDLE;
            destroy_state(s);
            return false;
        }

        VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (d.CreateSemaphore(dev, &semci, nullptr, &img.overlay_done) != VK_SUCCESS) {
            img.overlay_done = VK_NULL_HANDLE;
            destroy_state(s);
            return false;
        }
    }

    s.ok = true;
    return true;
}

// Wait for an image's fence (so its command buffer is safe to reuse) and record the
// overlay render pass into it. Returns the ImageState* on success, nullptr on any
// failure. Does NOT reset the fence or submit — the present hook resets the fence
// immediately before its single batched submit (across all swapchains, so the app's
// render-finished semaphores are waited exactly once). Resetting at submit time, not
// here, means a recording failure never leaves a reset-but-unsubmitted fence that a
// later UINT64_MAX wait would block on forever.
ImageState* record_one(SwapchainState& s, uint32_t image_index) {
    if (!s.ok || image_index >= s.images.size()) return nullptr;
    const DeviceDispatch& d = s.dd->disp;
    VkDevice dev = s.device;
    ImageState& img = s.images[image_index];

    // Wait on this image's fence ONLY if a prior submit is actually in flight on it.
    // If it isn't (first use, or a previous frame failed before/while submitting),
    // the command buffer is not in use and the fence may be unsignaled — waiting on
    // it would hang. The fence is reset + signalled only by the submit below.
    if (img.fence_in_flight &&
        d.WaitForFences(dev, 1, &img.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        return nullptr;

    // The layer's process-singleton state client (lazily started on first reference).
    // If the host sent a Disabled frame for this process, draw nothing — fall through
    // to an empty (LOAD-only) overlay pass. (DISABLE_CHOIR_OVERLAY is handled earlier
    // in QueuePresentKHR; this is the host-driven denylist path.)
    StateClient& client = StateClient::instance();
    const bool host_disabled = client.disabled();

    // Read the latest published snapshot (lock-free). Null until the host sends one (or
    // when host-disabled). The overlay is invisible until we are actually in a voice
    // channel (snap && snap->in_voice).
    std::shared_ptr<const Snapshot> snapshot =
        host_disabled ? nullptr : client.latest();
    const bool in_voice = snapshot && snapshot->in_voice;

    // --- LAZY OVERLAY INIT (Task 18) ---------------------------------------------
    // Do NO overlay Vulkan/ImGui allocation (ImguiRenderer init, descriptor pool,
    // AvatarTextures) until the FIRST snapshot with in_voice == true arrives and the
    // process is not disabled. A denylisted/Disabled process, or a process that is
    // simply never in voice, therefore does ZERO overlay allocation — only the cheap
    // dispatch passthrough + the state-client socket attempt. Once allocated we KEEP
    // the renderer for the swapchain's lifetime; later in_voice toggles just gate
    // drawing (no per-toggle teardown/realloc thrash).
    if (in_voice && !host_disabled && !s.imgui_tried) {
        s.imgui_tried = true;
        s.imgui = std::make_unique<ImguiRenderer>();
        if (!s.imgui->init(s.dd->instance, s.dd->physical_device, s.device,
                           s.dd->graphics_queue_family, s.dd->graphics_queue,
                           s.render_pass, static_cast<uint32_t>(s.images.size()),
                           s.dd->api_version,
                           needs_srgb_conversion(s.format, s.color_space), s.dd->instance_gipa,
                           d)) {
            s.imgui.reset();  // not ready — overlay draws nothing this swapchain
        }
        if (const char* dbg = ::getenv("CHOIR_DEBUG_LAZY_INIT"); dbg && *dbg) {
            std::fprintf(stderr, "[choir] overlay lazy-init on first in_voice: imgui=%s\n",
                         (s.imgui && s.imgui->ready()) ? "ready" : "failed");
            std::fflush(stderr);
        }
    }

    const bool renderer_ready = s.imgui && s.imgui->ready();

    // Once the renderer is ready, create the avatar texture cache bound to it (it uses
    // the renderer's device + descriptor pool). Created once per swapchain.
    if (renderer_ready && !s.avatars) {
        s.avatars = std::make_unique<AvatarTextures>();
        s.avatars->init(s.imgui.get(), needs_srgb_conversion(s.format, s.color_space));
    }

    // Build the ImGui draw list for this frame BEFORE recording the render pass
    // (ImGui::NewFrame/Render record no GPU commands). With no renderer, or when the
    // host disabled this process, this is a no-op and the render pass below just LOADs
    // the app's frame unchanged. draw_overlay itself draws nothing when the snapshot is
    // null or !in_voice.
    //
    // FAILURE ISOLATION (Task 18): the entire overlay block — avatar uploads (Vulkan),
    // begin_frame (ImGui NewFrame + our draw_overlay), end_frame (ImGui draw-data
    // record) — is wrapped in try/catch. ImGui can assert/abort/throw on misuse and the
    // avatar upload can throw (bad_alloc, etc.). On ANY exception we trip the per-device
    // overlay-off latch and return nullptr; the present hook then forwards the REAL
    // present so the frame still displays. We must not let a C++ exception unwind into
    // the C app, and we must not re-touch ImGui after a throw (its frame state may be
    // inconsistent) — the latch guarantees that.
    bool draw_imgui = renderer_ready && !host_disabled && s.avatars;
    try {
        // Eagerly drain any pending avatar-load requests and upload them on THIS
        // (render) thread — the only thread that may call Vulkan. Best-effort warm-up;
        // draw_overlay resolves each participant's texture by hash on demand. Cached by
        // hash, so repeats are cheap. Skip when host-disabled.
        if (renderer_ready && s.avatars && !host_disabled) {
            for (const AvatarReq& req : client.drain_avatar_requests())
                s.avatars->get_or_load(req);
            if (const char* dbg = ::getenv("CHOIR_DEBUG_AVATARS"); dbg && *dbg)
                std::fprintf(stderr, "[choir] avatar textures loaded: %zu\n",
                             s.avatars->size());
        }

        // Wall-clock ms for toast expiry: the host stamps Notification.created_ms with
        // std::chrono::system_clock ms, so read the same clock domain here.
        const int64_t now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();

        if (draw_imgui)
            s.imgui->begin_frame(s.extent, snapshot.get(), *s.avatars, client, now_ms);
    } catch (...) {
        // Tear down any started ImGui frame defensively, then latch overlay off.
        if (draw_imgui && s.imgui) {
            try { s.imgui->end_frame(VK_NULL_HANDLE); } catch (...) {}
        }
        mark_overlay_failed(s.dd, "exception during overlay frame build");
        return nullptr;
    }

    // Begin the render pass on this image's framebuffer (full extent), record the
    // ImGui draw data inside it, end.
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (d.BeginCommandBuffer(img.cmd, &bi) != VK_SUCCESS) {
        if (draw_imgui) {
            try { s.imgui->end_frame(VK_NULL_HANDLE); } catch (...) {}  // discard frame
        }
        return nullptr;
    }

    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = s.render_pass;
    rpbi.framebuffer = img.framebuffer;
    rpbi.renderArea.offset = {0, 0};
    rpbi.renderArea.extent = s.extent;
    rpbi.clearValueCount = 0;  // loadOp=LOAD: no clear values needed
    d.CmdBeginRenderPass(img.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    // Record the ImGui draw data into the command buffer, inside the active render
    // pass (replaces the Task-14 clear-rect placeholder). Wrapped: a throw here would
    // leave a half-recorded command buffer — we still close the render pass + buffer
    // and trip the latch so we never submit a malformed buffer twice.
    if (draw_imgui) {
        try {
            s.imgui->end_frame(img.cmd);
        } catch (...) {
            d.CmdEndRenderPass(img.cmd);
            d.EndCommandBuffer(img.cmd);
            mark_overlay_failed(s.dd, "exception recording ImGui draw data");
            return nullptr;
        }
    }

    d.CmdEndRenderPass(img.cmd);
    if (d.EndCommandBuffer(img.cmd) != VK_SUCCESS) return nullptr;
    return &img;
}

}  // namespace

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device,
                                                  const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                  const VkAllocationCallbacks* pAllocator,
                                                  VkSwapchainKHR* pSwapchain) {
    DeviceData* dd = device_data(device);
    if (!dd || !dd->disp.CreateSwapchainKHR) return VK_ERROR_INITIALIZATION_FAILED;

    VkSwapchainKHR old = pCreateInfo ? pCreateInfo->oldSwapchain : VK_NULL_HANDLE;

    // The overlay renders into the swapchain images as a COLOR attachment, so the
    // swapchain must be created with VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT. Some apps omit
    // it (e.g. DXVK's D3D11 blit-present path) — rendering into such images then corrupts
    // the GPU (NVIDIA Xid 13/32 -> device lost). Add it on a COPY of the create-info, like
    // MangoHud (see swapchain_usage.hpp). `overlay_capable` tracks whether the swapchain we
    // actually created carries the bit; build_state (which makes the framebuffers we render
    // into) runs ONLY when it does, so we can never render into a non-attachment image.
    VkSwapchainCreateInfoKHR overlay_ci = *pCreateInfo;
    overlay_ci.imageUsage = overlay_swapchain_usage(pCreateInfo->imageUsage);

    // Create the REAL swapchain first and unconditionally — this is the app's, and it
    // must succeed regardless of any overlay bookkeeping. Returned before any overlay
    // work that could throw.
    VkResult res = dd->disp.CreateSwapchainKHR(device, &overlay_ci, pAllocator, pSwapchain);
    bool overlay_capable =
        res == VK_SUCCESS && (overlay_ci.imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (res != VK_SUCCESS && overlay_ci.imageUsage != pCreateInfo->imageUsage) {
        // Our added usage bit was the only change; if the driver rejected it (only
        // possible on exotic shared-present surfaces that don't advertise color-attachment
        // support), retry with the app's EXACT request so the app's swapchain still
        // succeeds. The overlay simply won't render into this one (overlay_capable stays
        // false -> build_state skipped -> present forwards).
        res = dd->disp.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        overlay_capable = false;
    }
    if (res != VK_SUCCESS) return res;

    // ENTRYPOINT CATCH-ALL (Task 18): everything below is overlay bookkeeping. If it
    // throws, swallow it — the app already has a valid swapchain (res == VK_SUCCESS).
    // Trip the latch so the present hook stays out of the overlay path for this device.
    try {
        // If recreating from an old swapchain, tear our state for it down: the app is
        // retiring it and the new swapchain may reuse the queue/images. Idle (or wait
        // fences) before destroying objects that may be in flight.
        if (old != VK_NULL_HANDLE) {
            SwapchainState retired;
            bool have = false;
            {
                std::lock_guard<std::mutex> g(g_sc_lock);
                auto it = g_swapchains.find(old);
                if (it != g_swapchains.end()) {
                    retired = std::move(it->second);
                    g_swapchains.erase(it);
                    have = true;
                }
            }
            if (have) {
                if (dd->disp.DeviceWaitIdle) dd->disp.DeviceWaitIdle(device);
                destroy_state(retired);
            }
        }

        if (dd->overlay_disabled) return res;  // tracked nothing; present forwards

        SwapchainState s;
        s.device = device;
        s.dd = dd;
        s.format = pCreateInfo->imageFormat;
        s.color_space = pCreateInfo->imageColorSpace;
        s.extent = pCreateInfo->imageExtent;

        // Diagnostics: dump the swapchain's color format/space and how we'll treat it.
        // Set CHOIR_DEBUG_FORMAT=1 and launch the game to learn what it actually uses
        // (e.g. an HDR scRGB swapchain that needs sRGB->linear conversion).
        if (const char* dbg = ::getenv("CHOIR_DEBUG_FORMAT"); dbg && *dbg) {
            std::fprintf(stderr,
                         "[choir] swapchain format=%d colorspace=%d (%s) -> srgb_convert=%s%s\n",
                         static_cast<int>(s.format), static_cast<int>(s.color_space),
                         colorspace_name(s.color_space),
                         needs_srgb_conversion(s.format, s.color_space) ? "yes" : "no",
                         is_unhandled_hdr(s.color_space) ? " [UNHANDLED HDR transfer]" : "");
            std::fflush(stderr);
        }

        // build_state may fail (leaves s.ok == false); we still store it so present can
        // see "tracked but not drawable" and fall back, and so destroy cleans up. Skip it
        // entirely when the swapchain lacks color-attachment usage (see above): rendering
        // into those images would fault the GPU, so we leave s.ok == false and forward.
        if (overlay_capable) build_state(s, *pSwapchain);
        {
            std::lock_guard<std::mutex> g(g_sc_lock);
            g_swapchains[*pSwapchain] = std::move(s);
        }
    } catch (...) {
        mark_overlay_failed(dd, "exception in CreateSwapchainKHR bookkeeping");
    }
    return res;  // the app's swapchain is valid either way
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                               const VkAllocationCallbacks* pAllocator) {
    DeviceData* dd = device_data(device);

    // ENTRYPOINT CATCH-ALL (Task 18): free our overlay objects, but NEVER let an
    // exception stop us from destroying the app's swapchain down-chain (a leaked app
    // swapchain would break the game). Any throw here is swallowed; the real destroy
    // still runs below.
    try {
        SwapchainState s;
        bool have = false;
        {
            std::lock_guard<std::mutex> g(g_sc_lock);
            auto it = g_swapchains.find(swapchain);
            if (it != g_swapchains.end()) {
                s = std::move(it->second);
                g_swapchains.erase(it);
                have = true;
            }
        }
        if (have) {
            // Idle so no overlay command buffer for this swapchain is in flight, then
            // free our objects BEFORE the app's swapchain (and its images/views we view).
            if (dd && dd->disp.DeviceWaitIdle) dd->disp.DeviceWaitIdle(device);
            destroy_state(s);
        }
    } catch (...) {
        mark_overlay_failed(dd, "exception in DestroySwapchainKHR bookkeeping");
    }

    if (dd && dd->disp.DestroySwapchainKHR)
        dd->disp.DestroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue,
                                               const VkPresentInfoKHR* pPresentInfo) {
    DeviceData* dd = device_data(queue);

    // Fast path / fallback: anything missing -> forward the real present unchanged so
    // the frame still displays. Never crash the app.
    auto forward = [&]() -> VkResult {
        if (dd && dd->disp.QueuePresentKHR) return dd->disp.QueuePresentKHR(queue, pPresentInfo);
        return VK_ERROR_INITIALIZATION_FAILED;
    };

    // Failure-isolation latch (Task 18): once tripped for this device, do ZERO overlay
    // work forever — just forward the real present. Checked first (atomic, lock-free)
    // so a degraded device pays nothing but the real call. Also honor the static gates.
    if (!dd || !dd->disp.QueuePresentKHR || dd->overlay_disabled || overlay_disabled() ||
        dd->overlay_failed.load(std::memory_order_acquire) ||
        !pPresentInfo || pPresentInfo->swapchainCount == 0) {
        return forward();
    }

    // ENTRYPOINT CATCH-ALL (Task 18): a C++ exception unwinding through the C app/loader
    // is UB. Wrap the entire overlay block (record + submit) so any exception that
    // escapes record_one's inner guards (or std::vector/lock_guard allocation) trips the
    // latch and forwards the real present — the frame still displays, the game lives on.
    try {
        const uint32_t n = pPresentInfo->swapchainCount;
        const VkSemaphore* app_waits = pPresentInfo->pWaitSemaphores;
        const uint32_t app_wait_count = pPresentInfo->waitSemaphoreCount;
        const DeviceDispatch& d = dd->disp;

        std::lock_guard<std::mutex> g(g_sc_lock);

    // Phase 1: record the overlay render pass into every tracked swapchain's current
    // image. No submit yet, so the app's wait semaphores are still untouched — if any
    // recording fails we can cleanly fall back to the real present.
    std::vector<ImageState*> recorded;
    recorded.reserve(n);
    bool all_recorded = true;
    for (uint32_t i = 0; i < n; ++i) {
        auto it = g_swapchains.find(pPresentInfo->pSwapchains[i]);
        ImageState* img = (it != g_swapchains.end())
                              ? record_one(it->second, pPresentInfo->pImageIndices[i])
                              : nullptr;
        if (!img) { all_recorded = false; break; }
        recorded.push_back(img);
    }
    if (!all_recorded) return forward();

    // Phase 2: submit one overlay command buffer per swapchain, chained so the app's
    // render-finished semaphores are waited exactly once (a binary semaphore can be
    // waited at most once) and every overlay submit transitively executes after the
    // app's rendering. Submit[0] waits the app semaphores; submit[i>0] waits the prior
    // overlay-done. Each submit signals its own overlay-done and is fenced with its
    // image fence (so re-recording that image next time is safe).
    std::vector<VkPipelineStageFlags> app_stages(app_wait_count,
                                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    const VkPipelineStageFlags chain_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    std::vector<VkSemaphore> overlay_sems;
    overlay_sems.reserve(n);
    VkSemaphore prev_done = VK_NULL_HANDLE;

    for (uint32_t i = 0; i < n; ++i) {
        ImageState* img = recorded[i];
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        if (i == 0) {
            si.waitSemaphoreCount = app_wait_count;
            si.pWaitSemaphores = app_wait_count ? app_waits : nullptr;
            si.pWaitDstStageMask = app_wait_count ? app_stages.data() : nullptr;
        } else {
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &prev_done;
            si.pWaitDstStageMask = &chain_stage;
        }
        si.commandBufferCount = 1;
        si.pCommandBuffers = &img->cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &img->overlay_done;

        // Reset the fence immediately before the submit that signals it. We waited
        // on it (if it was in flight) back in record_one, so it is safe to reset
        // now; resetting an already-unsignaled fence is legal and harmless. Mark it
        // not-in-flight first so that if the submit below fails (or never happens),
        // the next present of this image skips the wait instead of deadlocking on a
        // reset-but-unsignaled fence.
        img->fence_in_flight = false;
        if (d.ResetFences(dd->device, 1, &img->fence) != VK_SUCCESS) {
            if (i == 0) {
                // Nothing submitted yet — clean fallback. A fence reset failing is a
                // sign the overlay path is unhealthy; latch it off so we stop trying.
                mark_overlay_failed(dd, "vkResetFences failed");
                return forward();
            }
            break;  // present what already submitted; no deadlock
        }
        if (d.QueueSubmit(queue, 1, &si, img->fence) != VK_SUCCESS) {
            // Submit failed: the fence was reset but is NOT in flight (left false
            // above), so the next present of this image will skip the wait. No leak,
            // no deadlock.
            if (i == 0) {
                // Nothing submitted yet (app semaphores not consumed) — clean fallback.
                mark_overlay_failed(dd, "vkQueueSubmit failed");
                return forward();
            }
            // A later submit failed after the app semaphores were consumed: we cannot
            // forward with the original wait list. Present with what we have so far so
            // the frame still displays and we never deadlock. Latch off for next time.
            mark_overlay_failed(dd, "vkQueueSubmit failed mid-batch");
            break;
        }
        // Submit succeeded: the fence is now genuinely in flight and will signal.
        img->fence_in_flight = true;
        overlay_sems.push_back(img->overlay_done);
        prev_done = img->overlay_done;
    }

        VkPresentInfoKHR pi = *pPresentInfo;
        pi.waitSemaphoreCount = static_cast<uint32_t>(overlay_sems.size());
        pi.pWaitSemaphores = overlay_sems.empty() ? nullptr : overlay_sems.data();
        return dd->disp.QueuePresentKHR(queue, &pi);
    } catch (...) {
        // An exception escaped the overlay block (e.g. std::bad_alloc growing a vector,
        // or a throw from deep in ImGui/avatar upload that record_one didn't catch).
        // Latch overlay off for this device and forward the real present so the frame
        // still displays. The lock_guard above has already released by the time we get
        // here (its scope ended with the throw), so forward() takes no lock — no
        // deadlock. NEVER rethrow: a C++ exception in a C caller is UB.
        mark_overlay_failed(dd, "exception in QueuePresentKHR overlay block");
        return forward();
    }
}

}  // namespace choir
