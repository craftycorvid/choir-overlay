// Choir OpenGL overlay interposer. LD_PRELOAD'd into a GL game; our exported symbols
// shadow the real eglSwapBuffers/glXSwapBuffers (and dlsym, so toolkits that resolve the
// swap dynamically still hit us). On each swap we draw the shared overlay into the back
// buffer, then call the real swap. Everything is resolved at runtime — we link no
// windowing lib — and every failure degrades to "present the game's frame untouched".
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // dlvsym / RTLD_NEXT (usually set by the build's command line too)
#endif
#include <dlfcn.h>

#include <chrono>
#include <cstdint>
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
    if (ctx_key == nullptr || overlay_disabled()) return;
    StateClient& client = StateClient::instance();
    if (client.disabled()) return;
    std::shared_ptr<const Snapshot> snap = client.latest();
    if (!snap || !snap->in_voice) return;   // nothing to draw -> skip all GL work

    CtxState* st = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_ctxs.find(ctx_key);
        if (it == g_ctxs.end()) { st = new CtxState(); g_ctxs.emplace(ctx_key, st); }
        else st = it->second;
    }
    if (st->skip) return;
    if (!st->renderer.ready() && !st->renderer.init()) { st->skip = true; return; }
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
    st->textures.shutdown();   // delete GL textures while the context is still current
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
}
void* egl_current_ctx() {
    auto f = CHOIR_REAL(EGLContext (*)(), "eglGetCurrentContext");
    return f ? f() : nullptr;
}
}  // namespace

// ---- EGL -------------------------------------------------------------------------------
CHOIR_EXPORT EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    uint32_t w, h; egl_extent(dpy, surf, w, h);
    draw_overlay_for(egl_current_ctx(), w, h);
    return CHOIR_REAL(EGLBoolean (*)(EGLDisplay, EGLSurface), "eglSwapBuffers")(dpy, surf);
}
CHOIR_EXPORT EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surf, EGLint* r, EGLint n) {
    uint32_t w, h; egl_extent(dpy, surf, w, h);
    draw_overlay_for(egl_current_ctx(), w, h);
    return CHOIR_REAL(EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint*, EGLint),
                      "eglSwapBuffersWithDamageKHR")(dpy, surf, r, n);
}
CHOIR_EXPORT EGLBoolean eglSwapBuffersWithDamageEXT(EGLDisplay dpy, EGLSurface surf, EGLint* r, EGLint n) {
    uint32_t w, h; egl_extent(dpy, surf, w, h);
    draw_overlay_for(egl_current_ctx(), w, h);
    return CHOIR_REAL(EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint*, EGLint),
                      "eglSwapBuffersWithDamageEXT")(dpy, surf, r, n);
}
CHOIR_EXPORT EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    destroy_ctx(ctx);
    return CHOIR_REAL(EGLBoolean (*)(EGLDisplay, EGLContext), "eglDestroyContext")(dpy, ctx);
}

// ---- GLX -------------------------------------------------------------------------------
CHOIR_EXPORT void glXSwapBuffers(Display* dpy, GLXDrawable drawable) {
    auto q = CHOIR_REAL(void (*)(Display*, GLXDrawable, int, unsigned int*), "glXQueryDrawable");
    unsigned int w = 0, h = 0;
    if (q) { q(dpy, drawable, 0x801D, &w); q(dpy, drawable, 0x801E, &h); }  // GLX_WIDTH/HEIGHT
    auto cur = CHOIR_REAL(GLXContext (*)(), "glXGetCurrentContext");
    draw_overlay_for(cur ? cur() : nullptr, w, h);
    CHOIR_REAL(void (*)(Display*, GLXDrawable), "glXSwapBuffers")(dpy, drawable);
}
CHOIR_EXPORT void glXDestroyContext(Display* dpy, GLXContext ctx) {
    destroy_ctx(ctx);
    CHOIR_REAL(void (*)(Display*, GLXContext), "glXDestroyContext")(dpy, ctx);
}

// ---- GetProcAddress + dlsym interception ----------------------------------------------
// Some toolkits (GLFW, SDL) fetch the swap function via eglGetProcAddress / glXGetProcAddress
// / dlsym and call THAT pointer, bypassing PLT symbol interposition. Return our hook for the
// names we own; otherwise defer to the real resolver.
namespace {
void* hook_for(const char* name) {
    if (!name) return nullptr;
    if (!std::strcmp(name, "eglSwapBuffers")) return reinterpret_cast<void*>(&eglSwapBuffers);
    if (!std::strcmp(name, "eglSwapBuffersWithDamageKHR")) return reinterpret_cast<void*>(&eglSwapBuffersWithDamageKHR);
    if (!std::strcmp(name, "eglSwapBuffersWithDamageEXT")) return reinterpret_cast<void*>(&eglSwapBuffersWithDamageEXT);
    if (!std::strcmp(name, "glXSwapBuffers")) return reinterpret_cast<void*>(&glXSwapBuffers);
    return nullptr;
}
}  // namespace

CHOIR_EXPORT void* eglGetProcAddress(const char* name) {
    if (void* h = hook_for(name)) return h;
    auto f = CHOIR_REAL(void* (*)(const char*), "eglGetProcAddress");
    return f ? f(name) : nullptr;
}
CHOIR_EXPORT void* glXGetProcAddressARB(const unsigned char* name) {
    if (void* h = hook_for(reinterpret_cast<const char*>(name))) return h;
    auto f = CHOIR_REAL(void* (*)(const unsigned char*), "glXGetProcAddressARB");
    return f ? f(name) : nullptr;
}
CHOIR_EXPORT void* glXGetProcAddress(const unsigned char* name) {
    if (void* h = hook_for(reinterpret_cast<const char*>(name))) return h;
    auto f = CHOIR_REAL(void* (*)(const unsigned char*), "glXGetProcAddress");
    return f ? f(name) : nullptr;
}
CHOIR_EXPORT void* dlsym(void* handle, const char* name) {
    if (void* h = hook_for(name)) return h;
    DlsymFn rd = real_dlsym();
    return rd ? rd(handle, name) : nullptr;
}
