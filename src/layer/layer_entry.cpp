// Choir Vulkan implicit-layer entry points: the layer's GetInstanceProcAddr /
// GetDeviceProcAddr and the loader interface negotiation.
//
// This is standard Khronos loader-layer-interface boilerplate (the pattern shared
// by the Khronos VK_LAYER samples and MangoHud). It negotiates interface version 2
// and routes the funcs we intercept (CreateInstance/DestroyInstance/CreateDevice/
// DestroyDevice + the two GetProcAddrs) to our hooks; everything else chains down.
//
// For the skeleton (Task 13) the layer draws nothing — it is a transparent
// pass-through. Swapchain/present hooks arrive in Task 14+, at which point
// choir_GetDeviceProcAddr starts returning those too.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <cstring>

#include "dispatch.hpp"

#if defined(__GNUC__)
#define CHOIR_EXPORT __attribute__((visibility("default")))
#else
#define CHOIR_EXPORT
#endif

extern "C" {

CHOIR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
choir_GetInstanceProcAddr(VkInstance instance, const char* pName);

CHOIR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
choir_GetDeviceProcAddr(VkDevice device, const char* pName);

// The layer's instance proc-addr. Returns our hooks for the entrypoints we
// intercept; otherwise chains to the next layer's GetInstanceProcAddr stored in
// the instance dispatch table.
CHOIR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
choir_GetInstanceProcAddr(VkInstance instance, const char* pName) {
#define CHOIR_INTERCEPT(fn)                                                     \
    if (std::strcmp(pName, "vk" #fn) == 0)                                      \
        return reinterpret_cast<PFN_vkVoidFunction>(choir::fn)

    // These must be resolvable with a NULL instance (loader bootstrap).
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(choir_GetInstanceProcAddr);
    CHOIR_INTERCEPT(CreateInstance);
    CHOIR_INTERCEPT(DestroyInstance);
    CHOIR_INTERCEPT(CreateDevice);
    CHOIR_INTERCEPT(DestroyDevice);
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(choir_GetDeviceProcAddr);
#undef CHOIR_INTERCEPT

    // Not intercepted: chain to the next layer's GIPA via the instance table.
    if (instance == VK_NULL_HANDLE) return nullptr;
    choir::InstanceData* id = choir::instance_data(instance);
    if (!id || !id->disp.GetInstanceProcAddr) return nullptr;
    return id->disp.GetInstanceProcAddr(instance, pName);
}

// The layer's device proc-addr. Mirrors the instance variant. For the skeleton we
// only intercept DestroyDevice; later tasks add swapchain/queue present hooks.
CHOIR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
choir_GetDeviceProcAddr(VkDevice device, const char* pName) {
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(choir_GetDeviceProcAddr);
    if (std::strcmp(pName, "vkDestroyDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(choir::DestroyDevice);

    if (device == VK_NULL_HANDLE) return nullptr;
    choir::DeviceData* dd = choir::device_data(device);
    if (!dd || !dd->disp.GetDeviceProcAddr) return nullptr;
    return dd->disp.GetDeviceProcAddr(device, pName);
}

// Exported under the well-known Vulkan name as well: with a manifest that does not
// pin an explicit entry-point name, some loaders look up "vkGetInstanceProcAddr"
// directly. Forward to our GIPA.
CHOIR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return choir_GetInstanceProcAddr(instance, pName);
}

CHOIR_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return choir_GetDeviceProcAddr(device, pName);
}

// Loader <-> layer interface negotiation. Pin to <= 2 and publish our GIPA/GDPA.
CHOIR_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (pVersionStruct == nullptr ||
        pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pVersionStruct->loaderLayerInterfaceVersion >
        CURRENT_LOADER_LAYER_INTERFACE_VERSION) {
        pVersionStruct->loaderLayerInterfaceVersion =
            CURRENT_LOADER_LAYER_INTERFACE_VERSION;
    }
    pVersionStruct->pfnGetInstanceProcAddr = choir_GetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = choir_GetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

}  // extern "C"
