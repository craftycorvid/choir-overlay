// Headless GL golden: render the Choir overlay through the GL backend into an offscreen
// EGL pbuffer FBO and assert it drew. Real-GPU test (like the Vulkan goldens). Exits 77
// (treated as SKIP by meson) if no EGL pbuffer GL context can be created here.
//
// This verifies the GL *render path* (GlRenderer + GlAvatarTextures + the shared overlay
// drawing) against the real GL backend the interposer ships. It does NOT exercise the
// LD_PRELOAD/dlsym interposer hooking — that is manual.
//
// Desktop GL (not GLES): the production GL backend (gl_backend_lib) is compiled for
// desktop GL, so ImGui_ImplOpenGL3_Init(nullptr) emits a desktop "#version 130" shader.
// Real Linux games run desktop GL contexts, so we create one here too (EGL_OPENGL_API /
// EGL_OPENGL_BIT, a 3.3 core profile) — a GLES context would mismatch that shader. The
// plan explicitly sanctions this desktop-GL path.
#include <EGL/egl.h>
#include <GL/gl.h>  // core desktop GL entrypoints, for the test harness's own FBO/readback

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "gl_avatar_textures.hpp"
#include "gl_renderer.hpp"
#include "iavatar_textures.hpp"  // Extent2D
#include "ipc/avatar_file.hpp"
#include "ipc/state.hpp"
#include "state_client.hpp"

// GL_RGBA8 isn't in the legacy <GL/gl.h> 1.1 header on every system; define if missing.
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

static const int W = 640, H = 360;

int main() {
    // --- EGL pbuffer desktop-GL 3.3 core context ------------------------------------
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) {
        fprintf(stderr, "no EGL display/init -> SKIP\n");
        return 77;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "no desktop-GL EGL API -> SKIP\n");
        return 77;
    }
    const EGLint cfg_attr[] = {EGL_SURFACE_TYPE,   EGL_PBUFFER_BIT,
                               EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                               EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8,
                               EGL_BLUE_SIZE,  8, EGL_ALPHA_SIZE, 8,
                               EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "no matching EGL config -> SKIP\n");
        return 77;
    }
    const EGLint pb_attr[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
    const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3,
                               EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(dpy, surf, surf, ctx)) {
        fprintf(stderr, "no EGL pbuffer surface/context -> SKIP\n");
        return 77;
    }

    // Resolve the FBO entrypoints (not in the legacy GL 1.1 <GL/gl.h>) via eglGetProcAddress.
    using PFNGenFB = void (*)(GLsizei, GLuint*);
    using PFNBindFB = void (*)(GLenum, GLuint);
    using PFNGenRB = void (*)(GLsizei, GLuint*);
    using PFNBindRB = void (*)(GLenum, GLuint);
    using PFNRBStorage = void (*)(GLenum, GLenum, GLsizei, GLsizei);
    using PFNFBRB = void (*)(GLenum, GLenum, GLenum, GLuint);
    using PFNCheckFB = GLenum (*)(GLenum);
    auto P = [](const char* nm) { return eglGetProcAddress(nm); };
    auto glGenFramebuffers = reinterpret_cast<PFNGenFB>(P("glGenFramebuffers"));
    auto glBindFramebuffer = reinterpret_cast<PFNBindFB>(P("glBindFramebuffer"));
    auto glGenRenderbuffers = reinterpret_cast<PFNGenRB>(P("glGenRenderbuffers"));
    auto glBindRenderbuffer = reinterpret_cast<PFNBindRB>(P("glBindRenderbuffer"));
    auto glRenderbufferStorage = reinterpret_cast<PFNRBStorage>(P("glRenderbufferStorage"));
    auto glFramebufferRenderbuffer = reinterpret_cast<PFNFBRB>(P("glFramebufferRenderbuffer"));
    auto glCheckFramebufferStatus = reinterpret_cast<PFNCheckFB>(P("glCheckFramebufferStatus"));
    if (!glGenFramebuffers || !glBindFramebuffer || !glGenRenderbuffers ||
        !glBindRenderbuffer || !glRenderbufferStorage || !glFramebufferRenderbuffer ||
        !glCheckFramebufferStatus) {
        fprintf(stderr, "FBO entrypoints unavailable -> SKIP\n");
        return 77;
    }
    constexpr GLenum kFramebuffer = 0x8D40, kRenderbuffer = 0x8D41;
    constexpr GLenum kColorAttachment0 = 0x8CE0, kFramebufferComplete = 0x8CD5;

    // --- offscreen RGBA8 FBO cleared to a known background ---------------------------
    GLuint fbo = 0, rb = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(kFramebuffer, fbo);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(kRenderbuffer, rb);
    glRenderbufferStorage(kRenderbuffer, GL_RGBA8, W, H);
    glFramebufferRenderbuffer(kFramebuffer, kColorAttachment0, kRenderbuffer, rb);
    if (glCheckFramebufferStatus(kFramebuffer) != kFramebufferComplete) {
        fprintf(stderr, "FBO incomplete -> SKIP\n");
        return 77;
    }
    glViewport(0, 0, W, H);
    const float BG[3] = {0.10f, 0.20f, 0.40f};  // app "blue", like the Vulkan goldens
    glClearColor(BG[0], BG[1], BG[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // --- avatar file on disk, in the EXACT format read_avatar_rgba expects ----------
    // (magic "CHAV" | u32 LE w | u32 LE h | w*h*4 RGBA8) — write via the host's own
    // writer so the framing can never drift. A solid opaque RED square, so the avatar
    // pixels are unambiguously not the background and don't depend on the font atlas.
    const uint32_t AW = 64, AH = 64;
    std::vector<uint8_t> avatar(size_t(AW) * AH * 4);
    for (size_t i = 0; i < size_t(AW) * AH; ++i) {
        avatar[i * 4 + 0] = 230;  // R
        avatar[i * 4 + 1] = 40;   // G
        avatar[i * 4 + 2] = 40;   // B
        avatar[i * 4 + 3] = 255;  // A (opaque)
    }
    const std::string avatar_path = std::string(P_tmpdir) + "/choir_gl_golden_avatar.chav";
    if (!choir::write_avatar_rgba(avatar_path, AW, AH, avatar.data())) {
        fprintf(stderr, "could not write avatar file\n");
        return 1;
    }

    // --- synthetic snapshot: in voice, one participant (fields per src/ipc/state.hpp) -
    choir::Snapshot snap;
    snap.in_voice = true;  // visibility gate
    snap.channel_name = "test";
    choir::Participant p;
    p.user_id = "1";
    p.display_name = "Tester";
    p.avatar_hash = "avatarhash";
    p.speaking = true;  // also draws the green speaking ring
    snap.participants.push_back(p);
    // Default AppearanceConfig (anchor = CenterLeft, scale = 1.0) is what we sample for.

    // --- render through the GL backend ----------------------------------------------
    choir::GlAvatarTextures textures;
    // Pre-load the avatar so overlay_ui's lookup(hash) hits without needing the host:
    // resolve_avatar() first tries textures.lookup(hash), which finds this entry.
    choir::AvatarReq req;
    req.hash = p.avatar_hash;
    req.path = avatar_path;
    req.w = AW;
    req.h = AH;
    const ImTextureID tex = textures.get_or_load(req);
    if (tex == ImTextureID_Invalid) {
        fprintf(stderr, "avatar texture upload failed (read_avatar_rgba/GL)\n");
        return 1;
    }

    choir::GlRenderer renderer;
    if (!renderer.init()) {
        fprintf(stderr, "GlRenderer init failed\n");
        return 1;
    }
    // StateClient::instance() is the process singleton; overlay_ui only calls
    // textures.lookup()/avatar_for() on it, and lookup already hit, so it isn't queried
    // for our pre-loaded avatar. now_ms = 0.
    renderer.draw(&snap, textures, choir::StateClient::instance(),
                  choir::Extent2D{uint32_t(W), uint32_t(H)}, /*now_ms*/ 0);
    glFinish();

    // --- read back + assert ----------------------------------------------------------
    std::vector<uint8_t> px(size_t(W) * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

    // glReadPixels has a bottom-left origin; ImGui draws with a top-left origin. Sample
    // by ImGui (x, y) and flip the row to index the readback buffer.
    auto at = [&](int x, int y) -> const uint8_t* {
        const int ry = H - 1 - y;  // top-left ImGui y -> bottom-left readback row
        return &px[(size_t(ry) * W + x) * 4];
    };
    auto is_bg = [&](const uint8_t* q) {
        return std::abs(int(q[0]) - int(BG[0] * 255)) < 16 &&
               std::abs(int(q[1]) - int(BG[1] * 255)) < 16 &&
               std::abs(int(q[2]) - int(BG[2] * 255)) < 16;
    };

    // Panel geometry for the DEFAULT CenterLeft anchor at W=640,H=360,scale=1:
    //   panel size    = (kPanelWidth=220, pad*2 + kRowHeight = 20 + 32 = 52)
    //   anchored pos   = (margin=16, (H-52)/2 = 154)
    //   row left       = pos.x + pad = 26 ; avatar diameter = 32
    //   avatar center  = (26 + 16, 154 + pad + 16) = (42, 170)
    // (42,170) lands on the avatar disc (here: our solid red avatar image), which is
    // painted by the draw list independent of the font atlas -> robust.
    const uint8_t* panel = at(42, 170);
    const uint8_t* far = at(W - 4, 4);  // top-right corner: far from the CenterLeft panel
    const bool panel_drawn = !is_bg(panel);
    const bool far_is_bg = is_bg(far);
    fprintf(stderr, "panel rgba=%d,%d,%d,%d far rgba=%d,%d,%d,%d\n", panel[0], panel[1],
            panel[2], panel[3], far[0], far[1], far[2], far[3]);
    fprintf(stderr, "panel_drawn=%d far_is_bg=%d\n", panel_drawn, far_is_bg);

    renderer.shutdown();
    textures.shutdown();
    std::remove(avatar_path.c_str());

    if (!panel_drawn || !far_is_bg) {
        fprintf(stderr, "GL golden FAIL\n");
        return 1;
    }
    fprintf(stderr, "GL golden OK\n");
    return 0;
}
