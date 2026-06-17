// vk_min_present — a minimal headless Vulkan app (Task 12 harness).
//
// Creates a real swapchain on a VK_EXT_headless_surface (no compositor/window),
// presents N frames cleared to a known color, and optionally reads the last
// presented image back to a PPM. This is the app the injected overlay layer
// (Tasks 13-18) is loaded into for automated golden-image tests: the layer hooks
// vkQueuePresentKHR and draws on top of our clear, and --readback captures the
// result.
//
// Exit codes: 0 ok; 77 = VK_EXT_headless_surface unavailable (test SKIP);
// other = a real Vulkan error.
//
// Usage: vk_min_present [--frames N] [--readback OUT.ppm] [--layer-info]

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define VK_CHECK(x)                                                            \
    do {                                                                       \
        VkResult _r = (x);                                                     \
        if (_r != VK_SUCCESS) {                                                \
            std::fprintf(stderr, "vk_min_present: %s -> VkResult %d\n", #x, _r); \
            return 2;                                                          \
        }                                                                      \
    } while (0)

namespace {

constexpr float kClearColor[4] = {0.10f, 0.20f, 0.80f, 1.0f};  // distinctive blue
constexpr uint32_t kExtent = 256;

bool has_instance_extension(const char* name) {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, props.data());
    for (const auto& p : props)
        if (std::strcmp(p.extensionName, name) == 0) return true;
    return false;
}

uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                   VkAccessFlags src_access, VkAccessFlags dst_access,
                   VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

int main(int argc, char** argv) {
    int frames = 1;
    const char* readback = nullptr;
    bool layer_info = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--readback") == 0 && i + 1 < argc) readback = argv[++i];
        else if (std::strcmp(argv[i], "--layer-info") == 0) layer_info = true;
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::puts("vk_min_present [--frames N] [--readback OUT.ppm] [--layer-info]");
            return 0;
        }
    }
    if (frames < 1) frames = 1;

    if (!has_instance_extension(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)) {
        std::fprintf(stderr, "vk_min_present: VK_EXT_headless_surface unavailable — skipping\n");
        return 77;
    }

    // --- Instance ---
    const char* inst_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                               VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vk_min_present";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_exts;
    VkInstance instance;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

    if (layer_info) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> lp(n);
        vkEnumerateInstanceLayerProperties(&n, lp.data());
        std::printf("vk_min_present: %u instance layer(s) available\n", n);
        for (auto& l : lp) std::printf("  layer: %s\n", l.layerName);
    }

    auto pfnCreateHeadless = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT"));
    if (!pfnCreateHeadless) {
        std::fprintf(stderr, "vk_min_present: vkCreateHeadlessSurfaceEXT not found — skipping\n");
        vkDestroyInstance(instance, nullptr);
        return 77;
    }

    // --- Headless surface (instance-level; created before device selection so we
    // can choose a physical device + queue family that can actually present to it.
    // Not every ICD supports headless present on every queue/GPU.) ---
    VkHeadlessSurfaceCreateInfoEXT sci{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VkSurfaceKHR surface;
    VK_CHECK(pfnCreateHeadless(instance, &sci, nullptr, &surface));

    // --- Pick a (physical device, graphics queue family) that can present here ---
    uint32_t pdc = 0;
    vkEnumeratePhysicalDevices(instance, &pdc, nullptr);
    if (pdc == 0) { std::fprintf(stderr, "vk_min_present: no physical devices\n"); return 2; }
    std::vector<VkPhysicalDevice> pds(pdc);
    vkEnumeratePhysicalDevices(instance, &pdc, pds.data());

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    uint32_t gfx = UINT32_MAX;
    for (auto pd : pds) {
        uint32_t qfc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfc);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, qfs.data());
        for (uint32_t i = 0; i < qfc; ++i) {
            if (!(qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 sup = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &sup);
            if (sup) { phys = pd; gfx = i; break; }
        }
        if (phys != VK_NULL_HANDLE) break;
    }
    if (phys == VK_NULL_HANDLE) {
        std::fprintf(stderr, "vk_min_present: no device/queue can present to a headless surface — skipping\n");
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        return 77;
    }

    // --- Logical device + swapchain ext ---
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = gfx;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    VkDevice device;
    VK_CHECK(vkCreateDevice(phys, &dci, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, gfx, 0, &queue);

    // --- Swapchain ---
    uint32_t fmtc = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmtc, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtc);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmtc, fmts.data());
    VkSurfaceFormatKHR chosen = fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = f; break; }

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    uint32_t img_count = caps.minImageCount + 1;
    if (caps.maxImageCount && img_count > caps.maxImageCount) img_count = caps.maxImageCount;
    VkExtent2D extent{kExtent, kExtent};
    if (caps.currentExtent.width != UINT32_MAX) extent = caps.currentExtent;

    VkSwapchainCreateInfoKHR swci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swci.surface = surface;
    swci.minImageCount = img_count;
    swci.imageFormat = chosen.format;
    swci.imageColorSpace = chosen.colorSpace;
    swci.imageExtent = extent;
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swci.clipped = VK_TRUE;
    VkSwapchainKHR swapchain;
    VK_CHECK(vkCreateSwapchainKHR(device, &swci, nullptr, &swapchain));

    uint32_t sic = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &sic, nullptr);
    std::vector<VkImage> images(sic);
    vkGetSwapchainImagesKHR(device, swapchain, &sic, images.data());

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = gfx;
    VkCommandPool pool;
    VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VK_CHECK(vkAllocateCommandBuffers(device, &cbai, &cb));

    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquire_sem, render_sem;
    VK_CHECK(vkCreateSemaphore(device, &semci, nullptr, &acquire_sem));
    VK_CHECK(vkCreateSemaphore(device, &semci, nullptr, &render_sem));

    uint32_t last_index = 0;
    for (int f = 0; f < frames; ++f) {
        uint32_t idx = 0;
        VK_CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquire_sem, VK_NULL_HANDLE, &idx));
        last_index = idx;

        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &bi));
        // UNDEFINED -> TRANSFER_DST, clear, -> PRESENT_SRC
        image_barrier(cb, images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue ccv;
        std::memcpy(ccv.float32, kClearColor, sizeof(kClearColor));
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ccv, 1, &range);
        image_barrier(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        VK_CHECK(vkEndCommandBuffer(cb));

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquire_sem;
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &render_sem;
        VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &render_sem;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &idx;
        VK_CHECK(vkQueuePresentKHR(queue, &pi));
        VK_CHECK(vkQueueWaitIdle(queue));
    }

    int rc = 0;
    if (readback) {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buf;
        VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &buf));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(device, buf, &mr);
        uint32_t mt = find_memory_type(phys, mr.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = mt;
        VkDeviceMemory mem;
        VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &mem));
        VK_CHECK(vkBindBufferMemory(device, buf, mem, 0));

        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &bi));
        image_barrier(cb, images[last_index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cb, images[last_index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
        VK_CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(queue));

        void* mapped = nullptr;
        VK_CHECK(vkMapMemory(device, mem, 0, bytes, 0, &mapped));
        const uint8_t* px = static_cast<const uint8_t*>(mapped);
        const bool bgra = (chosen.format == VK_FORMAT_B8G8R8A8_UNORM ||
                           chosen.format == VK_FORMAT_B8G8R8A8_SRGB);
        FILE* fp = std::fopen(readback, "wb");
        if (!fp) { std::perror("fopen readback"); rc = 2; }
        else {
            std::fprintf(fp, "P6\n%u %u\n255\n", extent.width, extent.height);
            for (uint32_t i = 0; i < extent.width * extent.height; ++i) {
                uint8_t r = px[i * 4 + (bgra ? 2 : 0)];
                uint8_t g = px[i * 4 + 1];
                uint8_t b = px[i * 4 + (bgra ? 0 : 2)];
                std::fputc(r, fp); std::fputc(g, fp); std::fputc(b, fp);
            }
            std::fclose(fp);
            std::printf("vk_min_present: wrote %s (%ux%u)\n", readback, extent.width, extent.height);
        }
        vkUnmapMemory(device, mem);
        vkDestroyBuffer(device, buf, nullptr);
        vkFreeMemory(device, mem, nullptr);
    }

    vkDeviceWaitIdle(device);
    vkDestroySemaphore(device, acquire_sem, nullptr);
    vkDestroySemaphore(device, render_sem, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return rc;
}
