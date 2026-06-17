// Swapchain tracking + the present-time draw path for the Choir implicit layer.
//
// Task 14: hook vkCreateSwapchainKHR / vkDestroySwapchainKHR / vkQueuePresentKHR.
// For each presented image we record + submit a render pass that draws the overlay
// (for this task: a solid test rectangle via vkCmdClearAttachments) on top of the
// game's already-rendered frame, then forward the real present with correct
// semaphore chaining (app render-finished -> overlay submit -> present).
//
// The render pass uses loadOp=LOAD and initial/final layout PRESENT_SRC_KHR so it
// preserves what the app drew and leaves the image presentable in place.
//
// Robustness contract: if anything required is missing (null dispatch func, no
// per-swapchain state, a Vulkan call fails) the present hook MUST fall back to the
// real vkQueuePresentKHR unchanged so the frame still displays. Never crash the app.
#pragma once

#include <vulkan/vulkan.h>

namespace choir {

struct DeviceData;  // dispatch.hpp

// Hooked device entrypoints (definitions in swapchain.cpp). Routed through the
// layer's GetDeviceProcAddr (layer_entry.cpp).
VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device,
                                                  const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                  const VkAllocationCallbacks* pAllocator,
                                                  VkSwapchainKHR* pSwapchain);

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                               const VkAllocationCallbacks* pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue,
                                               const VkPresentInfoKHR* pPresentInfo);

}  // namespace choir
