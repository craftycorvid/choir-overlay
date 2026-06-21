// Choir OpenGL overlay interposer. LD_PRELOAD'd into a GL game; our exported symbols
// shadow the real eglSwapBuffers/glXSwapBuffers (and dlsym, so toolkits that resolve the
// swap dynamically still hit us). On each swap we draw the shared overlay into the back
// buffer, then call the real swap. Everything is resolved at runtime — we link no
// windowing lib — and every failure degrades to "present the game's frame untouched".
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // dlvsym / RTLD_NEXT (usually set by the build's command line too)
#endif
#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "gl_api.hpp"
#include "gl_renderer.hpp"
#include "gl_avatar_textures.hpp"
#include "gating.hpp"
#include "state_client.hpp"
#include "iavatar_textures.hpp"   // Extent2D
#include "ipc/state.hpp"          // Snapshot

#define CHOIR_EXPORT extern "C" __attribute__((visibility("default")))

namespace {
using namespace choir;

// The genuine libc dlsym, fetched ONCE via dlvsym (which we do NOT interpose), so our own
// internal lookups never bounce through our exported dlsym hook below — which for a hooked
// name would return our own hook and infinitely recurse. The versioned "dlsym@GLIBC_2.2.5"
// is the canonical glibc dlsym.
using DlsymFn = void* (*)(void*, const char*);
DlsymFn real_dlsym() {
    static DlsymFn fn = reinterpret_cast<DlsymFn>(dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5"));
    return fn;
}

// Resolve+cache the real NEXT implementation of `name` (signature T) ONCE PER CALL SITE.
// The lambda gives each textual use its own static, so two entrypoints that share a C
// signature don't collide. Resolved via real_dlsym() + RTLD_NEXT so a hooked name (e.g.
// eglSwapBuffers) resolves to the genuine next function, never back into our own hook.
#define CHOIR_REAL(T, name) ([]() -> T {                                       \
    static auto _fn = real_dlsym() ? reinterpret_cast<T>(real_dlsym()(RTLD_NEXT, name)) \
                                   : static_cast<T>(nullptr);                   \
    return _fn; }())

// --- Opt-in debug logging: CHOIR_GL_DEBUG=1. Off (and near-zero overhead) otherwise. The
// GL interposer is silent by default; this is the GL counterpart to the layer's CHOIR_DEBUG_*
// toggles, for diagnosing "no overlay in <game>" (is the lib injected? what comm does the host
// see for gating? which gate blocked drawing? did the per-context renderer init fail?). ---
bool gl_debug() {
    static const bool on = [] {
        const char* v = std::getenv("CHOIR_GL_DEBUG");
        return v && v[0] == '1';
    }();
    return on;
}
#define CHOIR_GL_LOG(...) do { if (gl_debug()) {                 \
    std::fprintf(stderr, "[choir-gl] " __VA_ARGS__);             \
    std::fputc('\n', stderr); } } while (0)

// Read /proc/self/comm (the name the host uses for denylist gating) into `buf`.
void read_comm(char* buf, size_t n) {
    buf[0] = '\0';
    if (FILE* f = std::fopen("/proc/self/comm", "re")) {
        if (std::fgets(buf, n, f)) buf[strcspn(buf, "\n")] = '\0';
        std::fclose(f);
    }
}

// Fires the instant the lib is mapped — for an LD_PRELOAD that's at process startup, before
// the program's main(). Under CHOIR_GL_DEBUG this is the ground-truth signal that (a) our
// code runs in the target process and (b) its stderr reaches the log you're reading. If you
// see this line but no "first swap" line, we're injected but the swap isn't routing through us.
__attribute__((constructor)) void on_load() {
    if (!gl_debug()) return;
    char comm[64]; read_comm(comm, sizeof comm);
    // The JVM fork+execs jspawnhelper for every subprocess; it never does GL, so skip the
    // log spam (LD_PRELOAD is inherited by all children).
    if (std::strcmp(comm, "jspawnhelper") == 0) return;
    CHOIR_GL_LOG("loaded into pid=%ld comm=\"%s\" (CHOIR_GL_DEBUG on)",
                 static_cast<long>(getpid()), comm);
}

// Log once, on the first swap of any API: proves a hooked swap is actually reaching us.
void log_banner_once(const char* api) {
    static std::atomic<bool> done{false};
    if (!gl_debug() || done.exchange(true)) return;
    char comm[64]; read_comm(comm, sizeof comm);
    CHOIR_GL_LOG("first swap via %s (pid=%ld comm=\"%s\")",
                 api, static_cast<long>(getpid()), comm);
}

// Log when the game asks us to resolve one of our swap names through a resolver hook (dlsym /
// *GetProcAddress). Seeing this but no "first swap" => the game took our pointer but never
// called it; never seeing it => the game resolves the swap by a path we don't intercept.
void log_resolve(const char* via, const char* name) {
    CHOIR_GL_LOG("game resolved \"%s\" via %s -> returning our hook", name, via);
}

// Log the gating decision only when it CHANGES (so the hot path doesn't spam per frame).
enum Gate { kDrawing, kEnvDisabled, kHostDisabled, kNoSnapshot, kNotInVoice, kNullCtx };
void log_gate(Gate g) {
    static std::atomic<int> last{-1};
    if (!gl_debug() || last.exchange(g) == g) return;
    static const char* const names[] = {
        "DRAWING", "DISABLE_CHOIR_OVERLAY is set", "host sent Disabled (denylisted exe?)",
        "no host snapshot yet (is `choir` running + connected?)",
        "not in a voice channel", "no current GL context",
    };
    CHOIR_GL_LOG("gate -> %s", names[g]);
}

// Last-resort surface extent from the GL viewport (reflects the app's last glViewport, i.e.
// the drawable size). Used when the windowing query returns 0 — common for raw-Window GLX
// drawables, where glXQueryDrawable(GLX_WIDTH/HEIGHT) is not guaranteed to report geometry.
// Safe: only called from a swap hook, where a context is current.
void viewport_extent(uint32_t& w, uint32_t& h) {
    using GetIntegerv = void (*)(GLenum, GLint*);
    static auto fn = reinterpret_cast<GetIntegerv>(glapi::get_proc("glGetIntegerv"));
    if (!fn) return;
    GLint vp[4] = {0, 0, 0, 0};
    fn(0x0BA2 /* GL_VIEWPORT */, vp);
    if (vp[2] > 0 && vp[3] > 0) { w = uint32_t(vp[2]); h = uint32_t(vp[3]); }
}

// Per-GL-context overlay state. Created lazily on first swap for a context; freed by the
// destroy hooks. `skip` latches a failed init so we stop retrying for that context.
struct CtxState {
    GlRenderer renderer;
    GlAvatarTextures textures;
    bool skip = false;
};
std::mutex g_mutex;
// GL context handle -> state. A context the game never explicitly destroys leaks its
// CtxState until process exit; that is deliberate — running GL shutdown() at library
// unload is unsafe (no current context guaranteed), and process teardown reclaims it all.
std::unordered_map<void*, CtxState*> g_ctxs;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Shared draw path for EGL and GLX. `ctx_key` = current GL context handle; (w,h) = extent.
void draw_overlay_for(void* ctx_key, uint32_t w, uint32_t h) {
    if (ctx_key == nullptr) { log_gate(kNullCtx); return; }
    if (overlay_disabled()) { log_gate(kEnvDisabled); return; }
    StateClient& client = StateClient::instance();
    if (client.disabled()) { log_gate(kHostDisabled); return; }
    std::shared_ptr<const Snapshot> snap = client.latest();
    if (!snap) { log_gate(kNoSnapshot); return; }
    if (!snap->in_voice) { log_gate(kNotInVoice); return; }   // skip all GL work when idle
    log_gate(kDrawing);

    CtxState* st = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_ctxs.find(ctx_key);
        if (it == g_ctxs.end()) {
            st = new CtxState();
            g_ctxs.emplace(ctx_key, st);
            CHOIR_GL_LOG("new GL context %p", ctx_key);
        } else {
            st = it->second;
        }
    }
    if (st->skip) return;
    if (!st->renderer.ready() && !st->renderer.init()) {
        st->skip = true;
        CHOIR_GL_LOG("context %p: GlRenderer init FAILED; overlay disabled for it", ctx_key);
        return;
    }
    static std::atomic<bool> drew{false};
    if (!drew.exchange(true))
        CHOIR_GL_LOG("drawing overlay: ctx=%p extent=%ux%u", ctx_key, w, h);
    st->renderer.draw(snap.get(), st->textures, client, Extent2D{w, h}, now_ms());
}

void destroy_ctx(void* ctx_key) {
    CtxState* st = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_ctxs.find(ctx_key);
        if (it == g_ctxs.end()) return;
        st = it->second; g_ctxs.erase(it);
    }
    // GL teardown is only safe when the context being destroyed is the one CURRENT on this
    // thread: glvnd dispatches GL through the current context, so glDeleteTextures /
    // ImGui_ImplOpenGL3_Shutdown on a non-current (or no) current context segfaults.
    // egl/glXDestroyContext are frequently called with the context not current — then the
    // driver frees the context's GL objects on destroy anyway, so we drop the map entry
    // (done above) and LEAK the small CtxState wrapper rather than run its GL-touching
    // destructors. Bounded, rare, and crash-free.
    auto egl_cur = CHOIR_REAL(EGLContext (*)(), "eglGetCurrentContext");
    auto glx_cur = CHOIR_REAL(GLXContext (*)(), "glXGetCurrentContext");
    const bool is_current = (egl_cur && egl_cur() == ctx_key) ||
                            (glx_cur && glx_cur() == ctx_key);
    if (!is_current) return;   // safe fallback: leak the wrapper; GL objects die with the context
    st->textures.shutdown();
    st->renderer.shutdown();
    delete st;
}

// EGL helpers (EGL_WIDTH=0x3057, EGL_HEIGHT=0x3056).
void egl_extent(EGLDisplay dpy, EGLSurface surf, uint32_t& w, uint32_t& h) {
    auto q = CHOIR_REAL(EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint*), "eglQuerySurface");
    EGLint ww = 0, hh = 0;
    if (q) { q(dpy, surf, 0x3057, &ww); q(dpy, surf, 0x3056, &hh); }
    w = ww > 0 ? uint32_t(ww) : 0u;
    h = hh > 0 ? uint32_t(hh) : 0u;
    if (w == 0 || h == 0) viewport_extent(w, h);   // fallback when the surface query is unset
}
void* egl_current_ctx() {
    auto f = CHOIR_REAL(EGLContext (*)(), "eglGetCurrentContext");
    return f ? f() : nullptr;
}
}  // namespace

// ---- EGL -------------------------------------------------------------------------------
// Every hook resolves the real next function through CHOIR_REAL, which can return null if
// the symbol genuinely can't be resolved. Calling a null pointer would crash the game, so
// each hook guards the call and degrades (EGL_FALSE / no-op) instead — fail-safe by design.
CHOIR_EXPORT EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    log_banner_once("eglSwapBuffers");
    uint32_t w, h; egl_extent(dpy, surf, w, h);
    draw_overlay_for(egl_current_ctx(), w, h);
    auto real = CHOIR_REAL(EGLBoolean (*)(EGLDisplay, EGLSurface), "eglSwapBuffers");
    return real ? real(dpy, surf) : EGLBoolean(0);   // 0 == EGL_FALSE
}
CHOIR_EXPORT EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surf, EGLint* r, EGLint n) {
    log_banner_once("eglSwapBuffersWithDamageKHR");
    uint32_t w, h; egl_extent(dpy, surf, w, h);
    draw_overlay_for(egl_current_ctx(), w, h);
    auto real = CHOIR_REAL(EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint*, EGLint),
                           "eglSwapBuffersWithDamageKHR");
    return real ? real(dpy, surf, r, n) : EGLBoolean(0);
}
CHOIR_EXPORT EGLBoolean eglSwapBuffersWithDamageEXT(EGLDisplay dpy, EGLSurface surf, EGLint* r, EGLint n) {
    log_banner_once("eglSwapBuffersWithDamageEXT");
    uint32_t w, h; egl_extent(dpy, surf, w, h);
    draw_overlay_for(egl_current_ctx(), w, h);
    auto real = CHOIR_REAL(EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint*, EGLint),
                           "eglSwapBuffersWithDamageEXT");
    return real ? real(dpy, surf, r, n) : EGLBoolean(0);
}
CHOIR_EXPORT EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    destroy_ctx(ctx);
    auto real = CHOIR_REAL(EGLBoolean (*)(EGLDisplay, EGLContext), "eglDestroyContext");
    return real ? real(dpy, ctx) : EGLBoolean(0);
}

// ---- GLX -------------------------------------------------------------------------------
CHOIR_EXPORT void glXSwapBuffers(Display* dpy, GLXDrawable drawable) {
    log_banner_once("glXSwapBuffers");
    auto q = CHOIR_REAL(void (*)(Display*, GLXDrawable, int, unsigned int*), "glXQueryDrawable");
    unsigned int qw = 0, qh = 0;
    if (q) { q(dpy, drawable, 0x801D, &qw); q(dpy, drawable, 0x801E, &qh); }  // GLX_WIDTH/HEIGHT
    uint32_t w = qw, h = qh;
    if (w == 0 || h == 0) viewport_extent(w, h);   // raw-Window drawables don't report geometry
    auto cur = CHOIR_REAL(GLXContext (*)(), "glXGetCurrentContext");
    draw_overlay_for(cur ? cur() : nullptr, w, h);
    auto real = CHOIR_REAL(void (*)(Display*, GLXDrawable), "glXSwapBuffers");
    if (real) real(dpy, drawable);
}
CHOIR_EXPORT void glXDestroyContext(Display* dpy, GLXContext ctx) {
    destroy_ctx(ctx);
    auto real = CHOIR_REAL(void (*)(Display*, GLXContext), "glXDestroyContext");
    if (real) real(dpy, ctx);
}

// ---- GetProcAddress + dlsym interception ----------------------------------------------
// Some toolkits (GLFW, SDL) fetch the swap function via eglGetProcAddress / glXGetProcAddress
// / dlsym and call THAT pointer, bypassing PLT symbol interposition. Return our hook for the
// names we own; otherwise defer to the real resolver.

// Forward decls so hook_for can hand out our resolver hooks (defined just below).
CHOIR_EXPORT void* eglGetProcAddress(const char* name);
CHOIR_EXPORT void* glXGetProcAddress(const unsigned char* name);
CHOIR_EXPORT void* glXGetProcAddressARB(const unsigned char* name);

namespace {
void* hook_for(const char* name) {
    if (!name) return nullptr;
    // Swap functions — the actual draw points.
    if (!std::strcmp(name, "eglSwapBuffers")) return reinterpret_cast<void*>(&eglSwapBuffers);
    if (!std::strcmp(name, "eglSwapBuffersWithDamageKHR")) return reinterpret_cast<void*>(&eglSwapBuffersWithDamageKHR);
    if (!std::strcmp(name, "eglSwapBuffersWithDamageEXT")) return reinterpret_cast<void*>(&eglSwapBuffersWithDamageEXT);
    if (!std::strcmp(name, "glXSwapBuffers")) return reinterpret_cast<void*>(&glXSwapBuffers);
    // Resolver functions — hand out OURS too. Toolkits (GLFW) fetch a GetProcAddress via
    // dlsym and resolve the swap *through it*; if they got the real resolver they'd get the
    // real swap and bypass us. Returning our resolver keeps the whole chain pointed at us.
    if (!std::strcmp(name, "eglGetProcAddress")) return reinterpret_cast<void*>(&eglGetProcAddress);
    if (!std::strcmp(name, "glXGetProcAddress")) return reinterpret_cast<void*>(&glXGetProcAddress);
    if (!std::strcmp(name, "glXGetProcAddressARB")) return reinterpret_cast<void*>(&glXGetProcAddressARB);
    return nullptr;
}
}  // namespace

CHOIR_EXPORT void* eglGetProcAddress(const char* name) {
    if (void* h = hook_for(name)) { log_resolve("eglGetProcAddress", name); return h; }
    auto f = CHOIR_REAL(void* (*)(const char*), "eglGetProcAddress");
    return f ? f(name) : nullptr;
}
CHOIR_EXPORT void* glXGetProcAddressARB(const unsigned char* name) {
    const char* nm = reinterpret_cast<const char*>(name);
    if (void* h = hook_for(nm)) { log_resolve("glXGetProcAddressARB", nm); return h; }
    auto f = CHOIR_REAL(void* (*)(const unsigned char*), "glXGetProcAddressARB");
    return f ? f(name) : nullptr;
}
CHOIR_EXPORT void* glXGetProcAddress(const unsigned char* name) {
    const char* nm = reinterpret_cast<const char*>(name);
    if (void* h = hook_for(nm)) { log_resolve("glXGetProcAddress", nm); return h; }
    auto f = CHOIR_REAL(void* (*)(const unsigned char*), "glXGetProcAddress");
    return f ? f(name) : nullptr;
}
CHOIR_EXPORT void* dlsym(void* handle, const char* name) {
    if (void* h = hook_for(name)) { log_resolve("dlsym", name); return h; }
    DlsymFn rd = real_dlsym();
    return rd ? rd(handle, name) : nullptr;
}
