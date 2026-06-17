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
};

// Per-instance bookkeeping. `overlay_disabled` latches the gating decision so
// later tasks can skip all overlay work for this instance.
struct InstanceData {
    InstanceDispatch disp;
    bool overlay_disabled = false;
};

// Per-device bookkeeping. Holds the owning instance's loader key so the device
// can reach its instance data if needed.
struct DeviceData {
    DeviceDispatch disp;
    bool overlay_disabled = false;
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

}  // namespace choir
