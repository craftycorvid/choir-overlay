#include "gl_renderer.hpp"
#include "gl_api.hpp"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "overlay_ui.hpp"
namespace choir {
namespace {
// Pixel-store GL constants (avoid pulling <GL/gl.h>).
constexpr GLenum GL_PIXEL_UNPACK_BUFFER = 0x88EC, GL_PIXEL_UNPACK_BUFFER_BINDING = 0x88EF;
constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5, GL_UNPACK_ROW_LENGTH = 0x0CF2;
constexpr GLenum GL_UNPACK_SKIP_ROWS = 0x0CF3, GL_UNPACK_SKIP_PIXELS = 0x0CF4;

using PFNGetIntegerv = void (*)(GLenum, GLint*);
using PFNPixelStorei = void (*)(GLenum, GLint);
using PFNBindBuffer  = void (*)(GLenum, GLuint);
struct PixelStoreGL {
    PFNGetIntegerv GetIntegerv = reinterpret_cast<PFNGetIntegerv>(glapi::get_proc("glGetIntegerv"));
    PFNPixelStorei PixelStorei = reinterpret_cast<PFNPixelStorei>(glapi::get_proc("glPixelStorei"));
    PFNBindBuffer  BindBuffer  = reinterpret_cast<PFNBindBuffer>(glapi::get_proc("glBindBuffer"));
    bool ok() const { return GetIntegerv && PixelStorei && BindBuffer; }
};
const PixelStoreGL& psgl() { static PixelStoreGL g; return g; }

// Save the game's GL pixel-unpack state, reset it to defaults for the duration of our texture
// uploads (ImGui's font atlas + avatar textures), and restore on scope exit. Load-bearing for
// games that use pixel-unpack buffers (Iris/Minecraft): with a GL_PIXEL_UNPACK_BUFFER bound,
// every glTexImage2D/glTexSubImage2D reads from that PBO instead of our client memory, giving
// garbled/missing glyphs and avatars. No-op if the GL entrypoints can't be resolved.
struct ScopedCleanUnpack {
    GLint pbo = 0, align = 4, row = 0, skip_rows = 0, skip_pix = 0;
    bool active = false;
    ScopedCleanUnpack() {
        if (!psgl().ok()) return;
        active = true;
        psgl().GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
        psgl().GetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        psgl().GetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
        psgl().GetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
        psgl().GetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pix);
        if (pbo) psgl().BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        psgl().PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        psgl().PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        psgl().PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        psgl().PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    }
    ~ScopedCleanUnpack() {
        if (!active) return;
        if (pbo) psgl().BindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(pbo));
        psgl().PixelStorei(GL_UNPACK_ALIGNMENT, align);
        psgl().PixelStorei(GL_UNPACK_ROW_LENGTH, row);
        psgl().PixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
        psgl().PixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pix);
    }
};
}  // namespace

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
    // Neutralize the game's pixel-unpack state for all our texture uploads this frame
    // (font atlas + avatars), restored when this scope ends.
    ScopedCleanUnpack clean_unpack;
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
