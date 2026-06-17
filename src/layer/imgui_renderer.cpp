// The vendored imgui_impl_vulkan backend is compiled with VK_NO_PROTOTYPES (see
// src/layer/meson.build), so the backend uses function pointers loaded via
// ImGui_ImplVulkan_LoadFunctions instead of the global vk* symbols. Match that here
// (before any <vulkan/vulkan.h> include) so the imgui backend header's declarations
// agree with the backend translation unit. This TU itself never calls a global vk*
// symbol — it goes through the layer's dispatch table — so dropping the prototypes is
// safe.
#define VK_NO_PROTOTYPES
#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES

#include "imgui_renderer.hpp"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include "dispatch.hpp"
#include "overlay_ui.hpp"

namespace choir {
namespace {

// Descriptor pool size for the ImGui backend. The backend needs a handful of
// COMBINED_IMAGE_SAMPLER descriptors for the font atlas, plus one per texture
// registered via ImGui_ImplVulkan_AddTexture (Task 16 = avatar textures). Size it
// generously so avatars never exhaust it.
constexpr uint32_t kImguiMaxSets = 1000;

// User data for the ImGui Vulkan function loader thunk. Resolves Vulkan entrypoints
// through the layer's own dispatch chain (NOT the global loader), so the unwrapped
// VkInstance/VkPhysicalDevice/VkDevice handles a layer sees are accepted.
struct LoaderCtx {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
};

PFN_vkVoidFunction imgui_load_fn(const char* name, void* user_data) {
    auto* ctx = static_cast<LoaderCtx*>(user_data);
    // Prefer the device-level resolver for device functions; fall back to the
    // instance resolver for instance/physical-device functions (and anything the
    // device resolver doesn't know). vkGetInstanceProcAddr(instance, ...) resolves
    // both instance- and device-level commands, so it is the safe fallback.
    if (ctx->gdpa && ctx->device) {
        if (PFN_vkVoidFunction f = ctx->gdpa(ctx->device, name)) return f;
    }
    if (ctx->gipa) return ctx->gipa(ctx->instance, name);
    return nullptr;
}

}  // namespace

ImguiRenderer::~ImguiRenderer() { shutdown(); }

bool ImguiRenderer::init(VkInstance inst, VkPhysicalDevice phys, VkDevice dev,
                         uint32_t queue_family, VkQueue queue, VkRenderPass render_pass,
                         uint32_t image_count, uint32_t api_version,
                         PFN_vkGetInstanceProcAddr gipa, const DeviceDispatch& disp) {
    if (init_done_) return true;

    // Hard requirements — without these we cannot init the backend. Fall back to
    // "not ready" so the present hook draws nothing and still presents the frame.
    if (inst == VK_NULL_HANDLE || phys == VK_NULL_HANDLE || dev == VK_NULL_HANDLE ||
        queue == VK_NULL_HANDLE || render_pass == VK_NULL_HANDLE || image_count == 0 ||
        !gipa || !disp.GetDeviceProcAddr ||
        !disp.CreateDescriptorPool || !disp.DestroyDescriptorPool) {
        return false;
    }

    device_ = dev;
    disp_ = &disp;

    // --- Descriptor pool. FREE_DESCRIPTOR_SET_BIT is required by the backend (it
    // frees per-texture sets via RemoveTexture). One COMBINED_IMAGE_SAMPLER pool sized
    // for the font atlas + many avatar textures. ---
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = kImguiMaxSets;

    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets = kImguiMaxSets;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &pool_size;
    if (disp.CreateDescriptorPool(dev, &dpci, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        descriptor_pool_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        disp_ = nullptr;
        return false;
    }

    // --- ImGui context (no .ini settings file inside a game; no platform backend). ---
    ctx_ = ImGui::CreateContext();
    if (!ctx_) {
        disp.DestroyDescriptorPool(dev, descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        disp_ = nullptr;
        return false;
    }
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // never write imgui.ini from inside the game
    io.LogFilename = nullptr;

    // --- Feed the vendored backend its Vulkan function pointers, resolved through the
    // layer's OWN dispatch chain (gipa / disp.GetDeviceProcAddr). The backend is built
    // with VK_NO_PROTOTYPES, so it calls these pointers, not the global loader. This is
    // the crux of running ImGui from inside a layer: the loader's trampolines reject
    // the unwrapped handles we hold (they expect the loader's top-level wrappers),
    // whereas the next-down dispatch accepts them. ---
    LoaderCtx lctx;
    lctx.gipa = gipa;
    lctx.gdpa = disp.GetDeviceProcAddr;
    lctx.instance = inst;
    lctx.device = dev;
    if (!ImGui_ImplVulkan_LoadFunctions(api_version, imgui_load_fn, &lctx)) {
        ImGui::DestroyContext(ctx_);
        ctx_ = nullptr;
        disp.DestroyDescriptorPool(dev, descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        disp_ = nullptr;
        return false;
    }

    // --- ImGui Vulkan backend. In ImGui 1.92 the render-pass / MSAA fields moved into
    // PipelineInfoMain. We use a render pass (not dynamic rendering), so set
    // PipelineInfoMain.RenderPass. ---
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = api_version;
    info.Instance = inst;
    info.PhysicalDevice = phys;
    info.Device = dev;
    info.QueueFamily = queue_family;
    info.Queue = queue;
    info.DescriptorPool = descriptor_pool_;
    info.MinImageCount = image_count;
    info.ImageCount = image_count;
    info.PipelineCache = VK_NULL_HANDLE;
    info.PipelineInfoMain.RenderPass = render_pass;
    info.PipelineInfoMain.Subpass = 0;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.UseDynamicRendering = false;
    info.Allocator = nullptr;
    info.CheckVkResultFn = nullptr;

    if (!ImGui_ImplVulkan_Init(&info)) {
        ImGui::DestroyContext(ctx_);
        ctx_ = nullptr;
        disp.DestroyDescriptorPool(dev, descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        disp_ = nullptr;
        return false;
    }

    // Fonts: in ImGui 1.92 the backend manages the font atlas as a dynamic texture
    // (ImGuiBackendFlags_RendererHasTextures); ImGui_ImplVulkan_CreateFontsTexture was
    // removed. The atlas uploads lazily on the first ImGui_ImplVulkan_RenderDrawData
    // (via the backend's own command buffer/queue), so there is nothing to do here.

    init_done_ = true;
    return true;
}

void ImguiRenderer::begin_frame(VkExtent2D extent, const Snapshot* snap,
                                AvatarTextures& textures, StateClient& client,
                                int64_t now_ms) {
    if (!init_done_ || frame_started_) return;
    ImGui::SetCurrentContext(ctx_);

    // No platform backend: we set the display size manually and skip any
    // ImGui_ImplXxx_NewFrame for a windowing system. ImGui_ImplVulkan_NewFrame is the
    // renderer-side new-frame (no GPU commands; it handles texture bookkeeping).
    ImGui_ImplVulkan_NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(extent.width),
                            static_cast<float>(extent.height));
    // Some ImGui paths (e.g. timing) read DeltaTime; supply a sane fixed value since
    // we have no real per-frame clock wired here. Must be > 0.
    io.DeltaTime = 1.0f / 60.0f;

    ImGui::NewFrame();

    // --- The real overlay (Task 17): voice panel + toasts, driven by the snapshot.
    // draw_overlay draws NOTHING when snap is null or !snap->in_voice, so an empty
    // frame is built and end_frame stays balanced (present forwards unchanged). ---
    if (snap) draw_overlay(*snap, textures, client, extent, now_ms);

    ImGui::Render();
    frame_started_ = true;
}

void ImguiRenderer::end_frame(VkCommandBuffer cmd) {
    if (!init_done_ || !frame_started_) return;
    ImGui::SetCurrentContext(ctx_);
    // A null command buffer means "discard the frame we started" (a recording failure
    // between begin_frame and here): clear the pending-frame flag without recording.
    // Records ImGui's draw commands into `cmd` (must be inside the active render pass).
    // The font atlas / textures upload here on first use, via the backend's own
    // command buffer + queue (synchronous), independent of `cmd`.
    if (cmd != VK_NULL_HANDLE) {
        ImDrawData* draw_data = ImGui::GetDrawData();
        if (draw_data) ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
    }
    frame_started_ = false;
}

VkDescriptorSet ImguiRenderer::add_texture(VkSampler sampler, VkImageView view,
                                           VkImageLayout layout) {
    if (!init_done_) return VK_NULL_HANDLE;
    ImGui::SetCurrentContext(ctx_);
    return ImGui_ImplVulkan_AddTexture(sampler, view, layout);
}

void ImguiRenderer::remove_texture(VkDescriptorSet set) {
    if (!init_done_ || set == VK_NULL_HANDLE) return;
    ImGui::SetCurrentContext(ctx_);
    ImGui_ImplVulkan_RemoveTexture(set);
}

void ImguiRenderer::shutdown() {
    if (ctx_) ImGui::SetCurrentContext(ctx_);

    // If a frame was started but never ended (e.g. recording failed between
    // begin_frame and end_frame), close it so the next init doesn't leave dangling
    // frame state. EndFrame is idempotent enough for our manual loop here.
    if (init_done_ && frame_started_) {
        ImGui::EndFrame();
        frame_started_ = false;
    }

    if (init_done_) {
        ImGui_ImplVulkan_Shutdown();
        init_done_ = false;
    }
    if (ctx_) {
        ImGui::DestroyContext(ctx_);
        ctx_ = nullptr;
    }
    if (descriptor_pool_ && disp_ && disp_->DestroyDescriptorPool && device_) {
        disp_->DestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    }
    descriptor_pool_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    disp_ = nullptr;
}

}  // namespace choir
