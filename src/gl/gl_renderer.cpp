#include "gl_renderer.hpp"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "overlay_ui.hpp"
namespace choir {
GlRenderer::~GlRenderer() { shutdown(); }

bool GlRenderer::init() {
    if (init_done_) return true;
    ctx_ = ImGui::CreateContext();
    if (!ctx_) return false;
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;            // never write imgui.ini from inside the game
    io.LogFilename = nullptr;
    // nullptr => the backend auto-detects desktop GL vs GLES from the current context.
    if (!ImGui_ImplOpenGL3_Init(nullptr)) {
        ImGui::DestroyContext(ctx_); ctx_ = nullptr; return false;
    }
    init_done_ = true;
    return true;
}

void GlRenderer::draw(const Snapshot* snap, IAvatarTextures& textures, StateClient& client,
                      Extent2D extent, int64_t now_ms) {
    if (!init_done_) return;
    ImGui::SetCurrentContext(ctx_);
    ImGui_ImplOpenGL3_NewFrame();
    // Same defensive clamp as the Vulkan renderer: never feed ImGui a 0/huge/garbage size.
    auto sane = [](uint32_t v) -> float {
        if (v == 0) return 1.0f;
        if (v > 16384u) return 16384.0f;
        return static_cast<float>(v);
    };
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(sane(extent.width), sane(extent.height));
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    if (snap) draw_overlay(*snap, textures, client, extent, now_ms);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void GlRenderer::shutdown() {
    if (!init_done_ && !ctx_) return;
    if (ctx_) ImGui::SetCurrentContext(ctx_);
    if (init_done_) { ImGui_ImplOpenGL3_Shutdown(); init_done_ = false; }
    if (ctx_) { ImGui::DestroyContext(ctx_); ctx_ = nullptr; }
}
}  // namespace choir
