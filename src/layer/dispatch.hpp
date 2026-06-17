// Instance + device dispatch tables and the create/destroy hooks for the Choir
// implicit layer.
//
// Wiring follows the standard Khronos loader-layer interface (LoaderLayerInterface.md):
//   * vkCreateInstance walks pNext for VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
//     grabs pfnNextGetInstanceProcAddr, advances the link, calls the next
//     vkCreateInstance, then builds an instance dispatch table from the resulting
//     VkInstance and stores it keyed by the instance's loader dispatch key.
//   * vkCreateDevice mirrors that with VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
//     and pfnNextGetDeviceProcAddr.
//   * vkDestroyInstance / vkDestroyDevice forward down and tear down per-handle data.
//
// Per-handle data lives in maps keyed by the dispatchable-handle loader key
// (*reinterpret_cast<void**>(handle)), guarded by a global mutex.
//
// For the skeleton the layer is a transparent pass-through: it draws nothing and
// vk_min_present must behave identically with or without it. The device table
// already loads the swapchain/queue/command entrypoints the present hook will need
// in Task 14+, even though those hooks are not wired yet.
#pragma once

#include <vulkan/vulkan.h>

#include <atomic>

namespace choir {

// Instance-level functions we forward / will need. Loaded via the next layer's
// GetInstanceProcAddr after the real vkCreateInstance returns.
struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    // Surface queries the present path will need later.
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR = nullptr;
};

// Device-level functions. Loaded via the next layer's GetDeviceProcAddr after the
// real vkCreateDevice returns. The swapchain/queue/command entries are loaded now
// (harmless) so Task 14's present hook can use them without re-plumbing dispatch.
struct DeviceDispatch {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    // Command/sync entrypoints the present-time draw path will need (Task 14+).
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkCreateSemaphore CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    // Render-pass / framebuffer / image-view + draw recording for the present-time
    // overlay pass (Task 14+).
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateRenderPass CreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
    PFN_vkCmdClearAttachments CmdClearAttachments = nullptr;
    // Descriptor pool for the ImGui Vulkan backend (Task 15). ImGui itself allocates
    // its font-atlas + AddTexture descriptors from a pool we create and hand it.
    PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
    // Image / buffer / memory / sampler entrypoints for avatar textures (Task 16).
    // The render thread uploads each .rgba into a sampled VkImage via a staging
    // buffer + one-shot copy, then registers a sampler+view with the ImGui backend.
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkFlushMappedMemoryRanges FlushMappedMemoryRanges = nullptr;
    PFN_vkCreateSampler CreateSampler = nullptr;
    PFN_vkDestroySampler DestroySampler = nullptr;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
};

// Per-instance bookkeeping. `overlay_disabled` latches the gating decision so
// later tasks can skip all overlay work for this instance.
struct InstanceData {
    InstanceDispatch disp;
    bool overlay_disabled = false;
    // The VkInstance handle (so a device, which only sees a VkPhysicalDevice sharing
    // the instance's loader dispatch key, can recover the instance for ImGui init).
    VkInstance instance = VK_NULL_HANDLE;
    // The instance's apiVersion (from VkApplicationInfo, or VK_API_VERSION_1_0 if the
    // app supplied none). Forwarded to ImGui's backend (Task 15) so it loads the right
    // function variants; harmless for our render-pass (non-dynamic-rendering) path.
    uint32_t api_version = VK_API_VERSION_1_0;
};

// Per-device bookkeeping. Holds the owning instance's loader key so the device
// can reach its instance data if needed.
//
// NOT copyable (it owns a std::atomic latch). It is constructed in place inside the
// dispatch map (see CreateDevice), never copy-assigned.
struct DeviceData {
    DeviceData() = default;
    DeviceData(const DeviceData&) = delete;
    DeviceData& operator=(const DeviceData&) = delete;

    DeviceDispatch disp;
    bool overlay_disabled = false;
    // Failure-isolation latch (Task 18). Tripped the first time ANY overlay operation
    // throws or returns a fatal VkResult for this device. Once set, the present hook
    // does ZERO overlay work and just forwards the real vkQueuePresentKHR, so a single
    // internal failure degrades to "no overlay, game runs normally" — never a crash,
    // hang, or validation storm. Atomic so the present hook reads it without the
    // swapchain lock. Latches for the device lifetime (we never auto-recover).
    std::atomic<bool> overlay_failed{false};
    // The VkDevice handle itself (needed by the present hook, which only receives a
    // VkQueue) and a graphics-capable queue family on it (for the overlay command
    // pool). Captured at vkCreateDevice from the device's queue-create infos +
    // physical-device queue-family properties.
    VkDevice device = VK_NULL_HANDLE;
    uint32_t graphics_queue_family = 0;
    bool has_graphics_queue_family = false;
    // Handles the ImGui Vulkan backend needs at init (Task 15), captured at
    // vkCreateDevice. `graphics_queue` is the queue on `graphics_queue_family`
    // (fetched via vkGetDeviceQueue); ImGui uploads its font atlas on it.
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    uint32_t api_version = VK_API_VERSION_1_0;
    // The next-down vkGetInstanceProcAddr (the owning instance's). The ImGui Vulkan
    // backend (Task 15), built VK_NO_PROTOTYPES, resolves instance/physical-device
    // functions through this so it calls into the layer chain, not the loader.
    PFN_vkGetInstanceProcAddr instance_gipa = nullptr;
    // Physical-device memory properties (instance-level fn) captured at device
    // create. Avatar texture upload (Task 16) needs it to pick a device-local /
    // host-visible memory type for the image + staging buffer.
    PFN_vkGetPhysicalDeviceMemoryProperties get_phys_mem_props = nullptr;
};

// --- Hooked entrypoints (definitions in dispatch.cpp) ---
VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                              const VkAllocationCallbacks* pAllocator,
                                              VkInstance* pInstance);
VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance,
                                           const VkAllocationCallbacks* pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice,
                                            const VkDeviceCreateInfo* pCreateInfo,
                                            const VkAllocationCallbacks* pAllocator,
                                            VkDevice* pDevice);
VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device,
                                         const VkAllocationCallbacks* pAllocator);

// --- Per-handle data lookup (used by future hooks; keyed by loader dispatch key) ---
InstanceData* instance_data(void* dispatchable_handle);
DeviceData* device_data(void* dispatchable_handle);

// Trip the per-device overlay-off latch (Task 18) and log the reason ONCE to stderr
// (the very first trip for this device). After this the present hook forwards the
// real present unchanged for the device. `dd` may be null (no-op). `reason` is a short
// static string. Safe to call from the present hook (no lock held).
void mark_overlay_failed(DeviceData* dd, const char* reason);

}  // namespace choir
