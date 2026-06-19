// Unit tests for overlay_swapchain_usage (src/layer/swapchain_usage.hpp).
//
// The layer renders the overlay into the swapchain images as a COLOR attachment, so the
// swapchain it creates down-chain MUST carry VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT — even
// when the app omitted it (e.g. DXVK's D3D11 blit-present path). This is the pure logic
// behind that fix; a regression that stops adding the bit fails here, with no GPU.

#include "swapchain_usage.hpp"

#include <cassert>

using choir::overlay_swapchain_usage;

int main() {
    // The color-attachment bit is always present in the result.
    assert(overlay_swapchain_usage(0) & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

    // DXVK-like transfer-only swapchain: the bit is ADDED.
    const VkImageUsageFlags dxvk =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags fixed = overlay_swapchain_usage(dxvk);
    assert(fixed & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    // The app's own usages are preserved (we only add, never drop).
    assert((fixed & dxvk) == dxvk);

    // Already-present bit: idempotent (VKD3D-like swapchain stays unchanged).
    const VkImageUsageFlags vkd3d =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    assert(overlay_swapchain_usage(vkd3d) == vkd3d);

    return 0;
}
