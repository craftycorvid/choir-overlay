#include "swapchain.hpp"

#include <mutex>
#include <unordered_map>
#include <vector>

#include "dispatch.hpp"
#include "gating.hpp"

namespace choir {
namespace {

// The overlay test rectangle (Task 14): a solid red 64x64 box at (20,20). When
// Task 15 lands the ImGui backend this is replaced by the real overlay draw.
constexpr int32_t kRectX = 20;
constexpr int32_t kRectY = 20;
constexpr uint32_t kRectW = 64;
constexpr uint32_t kRectH = 64;
constexpr float kRectColor[4] = {1.0f, 0.0f, 0.0f, 1.0f};  // opaque red

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
    VkExtent2D extent{0, 0};
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<ImageState> images;
    bool ok = false;
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

    // --- Render pass: single color attachment, LOAD to preserve the app's frame,
    // PRESENT_SRC_KHR in and out so the image stays presentable in place. ---
    VkAttachmentDescription att{};
    att.format = s.format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    // External dependencies make the layout transitions visible: the present-source
    // read before the pass and after it. dstStageMask COLOR_ATTACHMENT_OUTPUT matches
    // where the clear-attachments load/store happen.
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 2;
    rpci.pDependencies = deps;
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

    // Begin the render pass on this image's framebuffer (full extent), draw the
    // overlay rectangle via vkCmdClearAttachments, end.
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (d.BeginCommandBuffer(img.cmd, &bi) != VK_SUCCESS) return nullptr;

    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = s.render_pass;
    rpbi.framebuffer = img.framebuffer;
    rpbi.renderArea.offset = {0, 0};
    rpbi.renderArea.extent = s.extent;
    rpbi.clearValueCount = 0;  // loadOp=LOAD: no clear values needed
    d.CmdBeginRenderPass(img.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    // Clamp the rectangle to the extent so a tiny swapchain can't overrun.
    VkClearAttachment clear{};
    clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear.colorAttachment = 0;
    for (int c = 0; c < 4; ++c) clear.clearValue.color.float32[c] = kRectColor[c];

    VkClearRect rect{};
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    int32_t x = kRectX, y = kRectY;
    uint32_t w = kRectW, h = kRectH;
    if (x >= static_cast<int32_t>(s.extent.width) || y >= static_cast<int32_t>(s.extent.height)) {
        // Rect entirely off-screen — still produce a valid (empty) pass.
        x = 0; y = 0; w = 0; h = 0;
    } else {
        if (x + w > s.extent.width) w = s.extent.width - x;
        if (y + h > s.extent.height) h = s.extent.height - y;
    }
    rect.rect.offset = {x, y};
    rect.rect.extent = {w, h};
    if (w > 0 && h > 0) d.CmdClearAttachments(img.cmd, 1, &clear, 1, &rect);

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

    // If recreating from an old swapchain, tear our state for it down first: the app
    // is retiring it, and the new swapchain may reuse the queue/images. We must idle
    // (or wait fences) before destroying objects that may be in flight; do that with
    // the old state's fences captured here, before the down-chain create.
    VkSwapchainKHR old = pCreateInfo ? pCreateInfo->oldSwapchain : VK_NULL_HANDLE;

    VkResult res = dd->disp.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    if (res != VK_SUCCESS) return res;

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
            // Idle the device so no overlay command buffer for the old swapchain is
            // still executing, then free its objects.
            if (dd->disp.DeviceWaitIdle) dd->disp.DeviceWaitIdle(device);
            destroy_state(retired);
        }
    }

    if (dd->overlay_disabled) return res;  // tracked nothing; present forwards unchanged

    SwapchainState s;
    s.device = device;
    s.dd = dd;
    s.format = pCreateInfo->imageFormat;
    s.extent = pCreateInfo->imageExtent;
    // build_state may fail (leaves s.ok == false); we still store it so present can
    // see "tracked but not drawable" and fall back, and so destroy cleans up.
    build_state(s, *pSwapchain);
    {
        std::lock_guard<std::mutex> g(g_sc_lock);
        g_swapchains[*pSwapchain] = std::move(s);
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                               const VkAllocationCallbacks* pAllocator) {
    DeviceData* dd = device_data(device);

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

    if (!dd || !dd->disp.QueuePresentKHR || dd->overlay_disabled || overlay_disabled() ||
        !pPresentInfo || pPresentInfo->swapchainCount == 0) {
        return forward();
    }

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
            if (i == 0) return forward();  // nothing submitted yet — clean fallback
            break;                         // present what already submitted; no deadlock
        }
        if (d.QueueSubmit(queue, 1, &si, img->fence) != VK_SUCCESS) {
            // Submit failed: the fence was reset but is NOT in flight (left false
            // above), so the next present of this image will skip the wait. No leak,
            // no deadlock.
            if (i == 0) {
                // Nothing submitted yet (app semaphores not consumed) — clean fallback.
                return forward();
            }
            // A later submit failed after the app semaphores were consumed: we cannot
            // forward with the original wait list. Present with what we have so far so
            // the frame still displays and we never deadlock.
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
}

}  // namespace choir
