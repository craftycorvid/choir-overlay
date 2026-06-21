#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // RTLD_NEXT; self-define so this TU is correct regardless of build flags (guard avoids a redefine warning when the toolchain predefines it)
#endif
#include "gl_api.hpp"
#include <dlfcn.h>
namespace choir::glapi {
namespace {
using GetProcEGL = void* (*)(const char*);
using GetProcGLX = void* (*)(const unsigned char*);
using DlsymFn = void* (*)(void*, const char*);
// The genuine libc dlsym, fetched via dlvsym (which libchoir_gl does NOT interpose). The
// interposer EXPORTS dlsym, so a bare dlsym() call from this object would bind to that hook;
// routing through real_dlsym keeps our own GL lookups off the hook entirely.
DlsymFn real_dlsym() {
    static DlsymFn fn = reinterpret_cast<DlsymFn>(dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5"));
    return fn;
}

// Resolve `name` from the real loaders without going through our own interposed dlsym.
// RTLD_NEXT only sees globally-visible objects, but toolkits like GLFW dlopen libGL/libGLX/
// libEGL with RTLD_LOCAL — so RTLD_NEXT misses them and the GL entrypoints come back null
// (=> avatar textures never upload, placeholder shown). Fall back to dlopening the already-
// mapped vendor libs (RTLD_NOLOAD) and resolving from their handles. Never RTLD_DEFAULT —
// that would find the interposer's own exported hooks first.
void* vendor_sym(const char* name) {
    DlsymFn rd = real_dlsym();
    if (!rd) return nullptr;
    if (void* p = rd(RTLD_NEXT, name)) return p;
    static const char* const libs[] = {
        "libGLX.so.0", "libGL.so.1", "libEGL.so.1", "libOpenGL.so.0", "libGLESv2.so.2",
    };
    for (const char* lib : libs) {
        if (void* h = dlopen(lib, RTLD_LAZY | RTLD_NOLOAD)) {
            if (void* p = rd(h, name)) return p;
        }
    }
    return nullptr;
}
}  // namespace
void* get_proc(const char* name) {
    // Prefer the GL loaders' own GetProcAddress (returns the correct glvnd dispatch stub),
    // then fall back to resolving the symbol directly from the vendor libs.
    static auto egl = reinterpret_cast<GetProcEGL>(vendor_sym("eglGetProcAddress"));
    static auto glx = reinterpret_cast<GetProcGLX>(vendor_sym("glXGetProcAddressARB"));
    if (egl) { if (void* p = egl(name)) return p; }
    if (glx) { if (void* p = glx(reinterpret_cast<const unsigned char*>(name))) return p; }
    return vendor_sym(name);
}
}  // namespace choir::glapi
