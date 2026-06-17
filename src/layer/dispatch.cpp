#include "dispatch.hpp"

#include <vulkan/vk_layer.h>

#include <mutex>
#include <unordered_map>
#include <vector>

#include "gating.hpp"

namespace choir {
namespace {

// Per-handle data, keyed by the dispatchable-handle loader key
// (*reinterpret_cast<void**>(handle)) — the first pointer-sized word of any
// dispatchable handle is the loader's dispatch table pointer, which is stable for
// the lifetime of the handle and identical for child handles of the same parent.
std::mutex g_lock;
std::unordered_map<void*, InstanceData> g_instances;
std::unordered_map<void*, DeviceData> g_devices;

void* dispatch_key(void* handle) { return *reinterpret_cast<void**>(handle); }

// Find the loader's VkLayerInstanceCreateInfo link (function == VK_LAYER_LINK_INFO)
// in the pNext chain of a VkInstanceCreateInfo.
VkLayerInstanceCreateInfo* find_instance_link(const VkInstanceCreateInfo* ci) {
    auto* p = static_cast<const VkLayerInstanceCreateInfo*>(ci->pNext);
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                  p->function == VK_LAYER_LINK_INFO)) {
        p = static_cast<const VkLayerInstanceCreateInfo*>(p->pNext);
    }
    return const_cast<VkLayerInstanceCreateInfo*>(p);
}

VkLayerDeviceCreateInfo* find_device_link(const VkDeviceCreateInfo* ci) {
    auto* p = static_cast<const VkLayerDeviceCreateInfo*>(ci->pNext);
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                  p->function == VK_LAYER_LINK_INFO)) {
        p = static_cast<const VkLayerDeviceCreateInfo*>(p->pNext);
    }
    return const_cast<VkLayerDeviceCreateInfo*>(p);
}

void load_instance_dispatch(InstanceDispatch& d, VkInstance instance,
                            PFN_vkGetInstanceProcAddr gipa) {
    d.GetInstanceProcAddr = gipa;
#define LOAD(name) d.name = reinterpret_cast<PFN_vk##name>(gipa(instance, "vk" #name))
    LOAD(DestroyInstance);
    LOAD(EnumeratePhysicalDevices);
    LOAD(GetPhysicalDeviceMemoryProperties);
    LOAD(GetPhysicalDeviceQueueFamilyProperties);
    LOAD(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD(GetPhysicalDeviceSurfaceFormatsKHR);
#undef LOAD
}

void load_device_dispatch(DeviceDispatch& d, VkDevice device,
                          PFN_vkGetDeviceProcAddr gdpa) {
    d.GetDeviceProcAddr = gdpa;
#define LOAD(name) d.name = reinterpret_cast<PFN_vk##name>(gdpa(device, "vk" #name))
    LOAD(DestroyDevice);
    LOAD(GetDeviceQueue);
    LOAD(QueueSubmit);
    LOAD(QueuePresentKHR);
    LOAD(CreateSwapchainKHR);
    LOAD(DestroySwapchainKHR);
    LOAD(GetSwapchainImagesKHR);
    LOAD(AcquireNextImageKHR);
    LOAD(DeviceWaitIdle);
    LOAD(CreateCommandPool);
    LOAD(DestroyCommandPool);
    LOAD(AllocateCommandBuffers);
    LOAD(FreeCommandBuffers);
    LOAD(BeginCommandBuffer);
    LOAD(EndCommandBuffer);
    LOAD(ResetCommandBuffer);
    LOAD(CreateSemaphore);
    LOAD(DestroySemaphore);
    LOAD(CreateFence);
    LOAD(DestroyFence);
    LOAD(WaitForFences);
    LOAD(ResetFences);
    LOAD(CreateImageView);
    LOAD(DestroyImageView);
    LOAD(CreateRenderPass);
    LOAD(DestroyRenderPass);
    LOAD(CreateFramebuffer);
    LOAD(DestroyFramebuffer);
    LOAD(CmdBeginRenderPass);
    LOAD(CmdEndRenderPass);
    LOAD(CmdClearAttachments);
#undef LOAD
}

}  // namespace

InstanceData* instance_data(void* dispatchable_handle) {
    std::lock_guard<std::mutex> g(g_lock);
    auto it = g_instances.find(dispatch_key(dispatchable_handle));
    return it == g_instances.end() ? nullptr : &it->second;
}

DeviceData* device_data(void* dispatchable_handle) {
    std::lock_guard<std::mutex> g(g_lock);
    auto it = g_devices.find(dispatch_key(dispatchable_handle));
    return it == g_devices.end() ? nullptr : &it->second;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                              const VkAllocationCallbacks* pAllocator,
                                              VkInstance* pInstance) {
    VkLayerInstanceCreateInfo* link = find_instance_link(pCreateInfo);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa =
        link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto next_create = reinterpret_cast<PFN_vkCreateInstance>(
        next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;

    // Advance the chain so the next layer sees its own link at the head.
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    VkResult res = next_create(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) return res;

    InstanceData data;
    data.overlay_disabled = overlay_disabled();
    load_instance_dispatch(data.disp, *pInstance, next_gipa);

    {
        std::lock_guard<std::mutex> g(g_lock);
        g_instances[dispatch_key(*pInstance)] = data;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance,
                                           const VkAllocationCallbacks* pAllocator) {
    if (!instance) return;
    PFN_vkDestroyInstance down = nullptr;
    {
        std::lock_guard<std::mutex> g(g_lock);
        void* key = dispatch_key(instance);
        auto it = g_instances.find(key);
        if (it != g_instances.end()) {
            down = it->second.disp.DestroyInstance;
            g_instances.erase(it);
        }
    }
    if (down) down(instance, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice,
                                            const VkDeviceCreateInfo* pCreateInfo,
                                            const VkAllocationCallbacks* pAllocator,
                                            VkDevice* pDevice) {
    VkLayerDeviceCreateInfo* link = find_device_link(pCreateInfo);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa =
        link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa =
        link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    auto next_create = reinterpret_cast<PFN_vkCreateDevice>(
        next_gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;

    // Advance the chain so the next layer sees its own link at the head.
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    VkResult res = next_create(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS) return res;

    DeviceData data;
    data.overlay_disabled = overlay_disabled();
    data.device = *pDevice;
    load_device_dispatch(data.disp, *pDevice, next_gdpa);

    // Pick a graphics-capable queue family the app actually created a queue on, so
    // the overlay command pool/buffers (and the present submit) run on a family that
    // supports graphics (vkCmdBeginRenderPass / clear-attachments need GRAPHICS).
    // The physical device shares the instance's loader dispatch key, so we can reach
    // the instance dispatch table through it.
    if (InstanceData* idata = instance_data(physicalDevice)) {
        if (idata->disp.GetPhysicalDeviceQueueFamilyProperties) {
            uint32_t qfc = 0;
            idata->disp.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qfc, nullptr);
            std::vector<VkQueueFamilyProperties> qfs(qfc);
            idata->disp.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qfc, qfs.data());
            for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
                uint32_t fam = pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex;
                if (fam < qfc && (qfs[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                    data.graphics_queue_family = fam;
                    data.has_graphics_queue_family = true;
                    break;
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> g(g_lock);
        g_devices[dispatch_key(*pDevice)] = data;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device,
                                         const VkAllocationCallbacks* pAllocator) {
    if (!device) return;
    PFN_vkDestroyDevice down = nullptr;
    {
        std::lock_guard<std::mutex> g(g_lock);
        void* key = dispatch_key(device);
        auto it = g_devices.find(key);
        if (it != g_devices.end()) {
            down = it->second.disp.DestroyDevice;
            g_devices.erase(it);
        }
    }
    if (down) down(device, pAllocator);
}

}  // namespace choir
