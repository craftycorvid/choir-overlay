// Minimal Vulkan implicit-layer stub. Real layer wiring (negotiate, dispatch
// tables, swapchain hooks) lands in Task 13. For now this only needs to compile
// into libchoir_overlay.so.
#include <vulkan/vulkan.h>

extern "C" PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance /*instance*/, const char* /*pName*/) {
    return nullptr;
}
