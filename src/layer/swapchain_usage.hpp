// Swapchain image-usage adjustment for the Choir overlay layer.
//
// The overlay renders into the app's swapchain images via a render pass, i.e. it uses
// them as a COLOR attachment. That is only legal if the swapchain was created with
// VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT. Some apps create swapchains WITHOUT it — notably
// DXVK's D3D11 present path, which only ever blits/copies into the swapchain images
// (transfer usage). Using such an image as a color attachment is undefined behaviour:
// on NVIDIA it corrupts the render-target compression metadata and faults the GPU
// (kernel NVRM "Xid 13" graphics exception / "Xid 32" corrupted pushbuffer), surfacing
// to the app as VK_ERROR_DEVICE_LOST ("Your rendering device has been lost"). VKD3D's
// D3D12 swapchains include the bit, so the same overlay works there.
//
// The fix (matching MangoHud's overlay_CreateSwapchainKHR) is to OR the bit into a COPY
// of the swapchain create-info before calling the driver down-chain. The Vulkan spec
// guarantees VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT is always in
// VkSurfaceCapabilitiesKHR::supportedUsageFlags for the normal present modes games use,
// so adding it never makes a valid swapchain creation fail.
#pragma once

#include <vulkan/vulkan.h>

namespace choir {

// The swapchain usage the layer must request so it can render the overlay into the
// images: the app's requested usage plus the color-attachment bit.
inline VkImageUsageFlags overlay_swapchain_usage(VkImageUsageFlags app_usage) {
    return app_usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
}

}  // namespace choir
