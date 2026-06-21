// Per-GL-context ImGui frame driver for the Choir GL interposer. Owns the ImGui context
// + the imgui_impl_opengl3 backend for one GL context; draws the shared overlay each
// present. RENDER-THREAD ONLY (the thread that owns the GL context). No-op when not ready.
#pragma once
#include "iavatar_textures.hpp"   // Extent2D, IAvatarTextures
#include <cstdint>
struct ImGuiContext;
namespace choir {
struct Snapshot;
class StateClient;
class GlRenderer {
public:
    GlRenderer() = default;
    ~GlRenderer();
    GlRenderer(const GlRenderer&) = delete;
    GlRenderer& operator=(const GlRenderer&) = delete;
    // Create the ImGui context + GL backend against the CURRENT GL context. Returns
    // false (and stays !ready) on any failure, so the caller just presents untouched.
    bool init();
    // NewFrame -> draw_overlay(snap) -> Render -> RenderDrawData into the bound FBO.
    void draw(const Snapshot* snap, IAvatarTextures& textures, StateClient& client,
              Extent2D extent, int64_t now_ms);
    void shutdown();
    bool ready() const { return init_done_; }
    ImGuiContext* context() const { return ctx_; }
private:
    bool init_done_ = false;
    ImGuiContext* ctx_ = nullptr;
};
}  // namespace choir
