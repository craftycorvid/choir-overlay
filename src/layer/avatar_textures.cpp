// This TU calls Vulkan only through the layer's device dispatch table (never the
// global loader), so drop the prototypes — matches imgui_renderer.cpp and keeps the
// layer from binding libvulkan symbols.
#define VK_NO_PROTOTYPES

#include "avatar_textures.hpp"

#include <cstring>
#include <utility>
#include <vector>

#include "dispatch.hpp"
#include "imgui_renderer.hpp"
#include "ipc/avatar_file.hpp"

namespace choir {
namespace {

// Pick a memory type index satisfying `bits` (the requirements' memoryTypeBits) and
// having all of `want` property flags. Returns UINT32_MAX if none matches.
uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits,
                          VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

}  // namespace

AvatarTextures::~AvatarTextures() { shutdown(); }

void AvatarTextures::init(ImguiRenderer* renderer, bool srgb) {
    renderer_ = renderer;
    srgb_ = srgb;
}

ImTextureID AvatarTextures::lookup(const std::string& hash) const {
    auto it = textures_.find(hash);
    if (it == textures_.end()) return ImTextureID_Invalid;
    return reinterpret_cast<ImTextureID>(it->second.descriptor);
}

ImTextureID AvatarTextures::get_or_load(const AvatarReq& req) {
    if (req.hash.empty()) return ImTextureID_Invalid;
    if (auto it = textures_.find(req.hash); it != textures_.end())
        return reinterpret_cast<ImTextureID>(it->second.descriptor);

    if (!renderer_ || !renderer_->ready()) return ImTextureID_Invalid;

    // Decode the on-disk RGBA. read_avatar_rgba validates the magic + size, so a
    // truncated/corrupt file fails cleanly here (never crashes).
    uint32_t fw = 0, fh = 0;
    std::vector<uint8_t> rgba;
    if (!read_avatar_rgba(req.path, fw, fh, rgba)) return ImTextureID_Invalid;
    if (fw == 0 || fh == 0 || rgba.size() != static_cast<size_t>(fw) * fh * 4)
        return ImTextureID_Invalid;

    Texture t;
    if (!create_texture(fw, fh, rgba.data(), t)) return ImTextureID_Invalid;

    // Register with the ImGui backend (returns a VkDescriptorSet usable as
    // ImTextureID). The sampled image is in SHADER_READ_ONLY_OPTIMAL after upload.
    t.descriptor = renderer_->add_texture(t.sampler, t.view,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (t.descriptor == VK_NULL_HANDLE) {
        destroy_texture(t);
        return ImTextureID_Invalid;
    }

    ImTextureID id = reinterpret_cast<ImTextureID>(t.descriptor);
    textures_.emplace(req.hash, t);
    return id;
}

bool AvatarTextures::create_texture(uint32_t w, uint32_t h, const uint8_t* rgba,
                                    Texture& t) {
    t = Texture{};
    if (!renderer_) return false;
    const DeviceDispatch* dp = renderer_->dispatch();
    VkDevice dev = renderer_->device();
    if (!dp || dev == VK_NULL_HANDLE) return false;

    // Every entrypoint we touch must be present (a stripped ICD could leave some
    // null). Bail cleanly if so — the overlay just shows no avatar.
    if (!dp->CreateImage || !dp->DestroyImage || !dp->GetImageMemoryRequirements ||
        !dp->BindImageMemory || !dp->CreateBuffer || !dp->DestroyBuffer ||
        !dp->GetBufferMemoryRequirements || !dp->BindBufferMemory || !dp->AllocateMemory ||
        !dp->FreeMemory || !dp->MapMemory || !dp->UnmapMemory || !dp->CreateSampler ||
        !dp->DestroySampler || !dp->CreateImageView || !dp->DestroyImageView ||
        !dp->CreateCommandPool || !dp->DestroyCommandPool || !dp->AllocateCommandBuffers ||
        !dp->BeginCommandBuffer || !dp->EndCommandBuffer || !dp->CmdCopyBufferToImage ||
        !dp->CmdPipelineBarrier || !dp->CreateFence || !dp->DestroyFence ||
        !dp->WaitForFences || !dp->QueueSubmit) {
        return false;
    }

    DeviceData* dd = device_data(dev);
    if (!dd || !dd->has_graphics_queue_family || dd->graphics_queue == VK_NULL_HANDLE ||
        !dd->get_phys_mem_props || dd->physical_device == VK_NULL_HANDLE) {
        return false;
    }

    VkPhysicalDeviceMemoryProperties mp{};
    dd->get_phys_mem_props(dd->physical_device, &mp);

    const VkDeviceSize pixel_bytes = static_cast<VkDeviceSize>(w) * h * 4;

    // --- Sampled, transfer-dst RGBA8 image (device-local) ---
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = srgb_ ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (dp->CreateImage(dev, &ici, nullptr, &t.image) != VK_SUCCESS) {
        t.image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements img_req{};
    dp->GetImageMemoryRequirements(dev, t.image, &img_req);
    uint32_t img_type = find_memory_type(mp, img_req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (img_type == UINT32_MAX) img_type = find_memory_type(mp, img_req.memoryTypeBits, 0);
    if (img_type == UINT32_MAX) { destroy_texture(t); return false; }

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = img_req.size;
    mai.memoryTypeIndex = img_type;
    if (dp->AllocateMemory(dev, &mai, nullptr, &t.memory) != VK_SUCCESS) {
        t.memory = VK_NULL_HANDLE;
        destroy_texture(t);
        return false;
    }
    if (dp->BindImageMemory(dev, t.image, t.memory, 0) != VK_SUCCESS) {
        destroy_texture(t);
        return false;
    }

    // --- Host-visible staging buffer holding the pixels ---
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    auto free_staging = [&]() {
        if (staging_mem) dp->FreeMemory(dev, staging_mem, nullptr);
        if (staging) dp->DestroyBuffer(dev, staging, nullptr);
    };

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = pixel_bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dp->CreateBuffer(dev, &bci, nullptr, &staging) != VK_SUCCESS) {
        destroy_texture(t);
        return false;
    }
    VkMemoryRequirements buf_req{};
    dp->GetBufferMemoryRequirements(dev, staging, &buf_req);
    uint32_t buf_type = find_memory_type(
        mp, buf_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (buf_type == UINT32_MAX) {
        free_staging();
        destroy_texture(t);
        return false;
    }
    VkMemoryAllocateInfo smai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    smai.allocationSize = buf_req.size;
    smai.memoryTypeIndex = buf_type;
    if (dp->AllocateMemory(dev, &smai, nullptr, &staging_mem) != VK_SUCCESS) {
        staging_mem = VK_NULL_HANDLE;
        free_staging();
        destroy_texture(t);
        return false;
    }
    if (dp->BindBufferMemory(dev, staging, staging_mem, 0) != VK_SUCCESS) {
        free_staging();
        destroy_texture(t);
        return false;
    }
    void* mapped = nullptr;
    if (dp->MapMemory(dev, staging_mem, 0, pixel_bytes, 0, &mapped) != VK_SUCCESS ||
        !mapped) {
        free_staging();
        destroy_texture(t);
        return false;
    }
    std::memcpy(mapped, rgba, static_cast<size_t>(pixel_bytes));
    dp->UnmapMemory(dev, staging_mem);

    // --- One-shot command buffer: barrier UNDEFINED->TRANSFER_DST, copy, barrier
    // TRANSFER_DST->SHADER_READ_ONLY. Submit on the graphics queue, wait on a fence. ---
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = dd->graphics_queue_family;
    if (dp->CreateCommandPool(dev, &pci, nullptr, &pool) != VK_SUCCESS) {
        free_staging();
        destroy_texture(t);
        return false;
    }
    auto destroy_pool = [&]() { dp->DestroyCommandPool(dev, pool, nullptr); };

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (dp->AllocateCommandBuffers(dev, &cbai, &cmd) != VK_SUCCESS) {
        destroy_pool();
        free_staging();
        destroy_texture(t);
        return false;
    }

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dp->BeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        destroy_pool();
        free_staging();
        destroy_texture(t);
        return false;
    }

    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = t.image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dp->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &to_dst);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {w, h, 1};
    dp->CmdCopyBufferToImage(cmd, staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &region);

    VkImageMemoryBarrier to_read{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_read.image = t.image;
    to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dp->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                           1, &to_read);

    if (dp->EndCommandBuffer(cmd) != VK_SUCCESS) {
        destroy_pool();
        free_staging();
        destroy_texture(t);
        return false;
    }

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (dp->CreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS) {
        destroy_pool();
        free_staging();
        destroy_texture(t);
        return false;
    }

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    bool ok = dp->QueueSubmit(dd->graphics_queue, 1, &si, fence) == VK_SUCCESS &&
              dp->WaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;

    dp->DestroyFence(dev, fence, nullptr);
    destroy_pool();
    free_staging();
    if (!ok) {
        destroy_texture(t);
        return false;
    }

    // --- Image view + sampler for ImGui ---
    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = t.image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = srgb_ ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    ivci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (dp->CreateImageView(dev, &ivci, nullptr, &t.view) != VK_SUCCESS) {
        t.view = VK_NULL_HANDLE;
        destroy_texture(t);
        return false;
    }

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.minLod = 0.0f;
    sci.maxLod = 0.0f;
    sci.maxAnisotropy = 1.0f;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if (dp->CreateSampler(dev, &sci, nullptr, &t.sampler) != VK_SUCCESS) {
        t.sampler = VK_NULL_HANDLE;
        destroy_texture(t);
        return false;
    }

    return true;
}

void AvatarTextures::destroy_texture(Texture& t) {
    if (!renderer_) return;
    const DeviceDispatch* dp = renderer_->dispatch();
    VkDevice dev = renderer_->device();
    if (dp && dev != VK_NULL_HANDLE) {
        if (t.sampler && dp->DestroySampler) dp->DestroySampler(dev, t.sampler, nullptr);
        if (t.view && dp->DestroyImageView) dp->DestroyImageView(dev, t.view, nullptr);
        if (t.image && dp->DestroyImage) dp->DestroyImage(dev, t.image, nullptr);
        if (t.memory && dp->FreeMemory) dp->FreeMemory(dev, t.memory, nullptr);
    }
    t = Texture{};
}

void AvatarTextures::shutdown() {
    if (textures_.empty()) {
        renderer_ = nullptr;
        return;
    }
    // The caller idles the device before destroying the swapchain (DestroySwapchainKHR
    // / recreate both DeviceWaitIdle first), so the textures are not in use here.
    for (auto& [hash, t] : textures_) {
        // Unregister the ImGui descriptor before freeing the underlying objects.
        if (renderer_ && t.descriptor != VK_NULL_HANDLE)
            renderer_->remove_texture(t.descriptor);
        t.descriptor = VK_NULL_HANDLE;
        destroy_texture(t);
    }
    textures_.clear();
    renderer_ = nullptr;
}

}  // namespace choir
