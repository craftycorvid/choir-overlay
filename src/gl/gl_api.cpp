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
}  // namespace
void* get_proc(const char* name) {
    DlsymFn rd = real_dlsym();
    if (!rd) return nullptr;
    // eglGetProcAddress / glXGetProcAddressARB resolved via the genuine next loader
    // (RTLD_NEXT through real dlsym), never our own interposed copies.
    static auto egl = reinterpret_cast<GetProcEGL>(rd(RTLD_NEXT, "eglGetProcAddress"));
    static auto glx = reinterpret_cast<GetProcGLX>(rd(RTLD_NEXT, "glXGetProcAddressARB"));
    if (egl) { if (void* p = egl(name)) return p; }
    if (glx) { if (void* p = glx(reinterpret_cast<const unsigned char*>(name))) return p; }
    return rd(RTLD_NEXT, name);
}
}  // namespace choir::glapi
