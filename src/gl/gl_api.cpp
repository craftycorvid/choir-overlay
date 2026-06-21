#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // RTLD_NEXT; self-define so this TU is correct regardless of build flags (guard avoids a redefine warning when the toolchain predefines it)
#endif
#include "gl_api.hpp"
#include <dlfcn.h>
namespace choir::glapi {
namespace {
using GetProcEGL = void* (*)(const char*);
using GetProcGLX = void* (*)(const unsigned char*);
}  // namespace
void* get_proc(const char* name) {
    // eglGetProcAddress / glXGetProcAddressARB are themselves resolved via the real
    // loader (RTLD_NEXT), never our own interposed copies.
    static auto egl = reinterpret_cast<GetProcEGL>(dlsym(RTLD_NEXT, "eglGetProcAddress"));
    static auto glx = reinterpret_cast<GetProcGLX>(dlsym(RTLD_NEXT, "glXGetProcAddressARB"));
    if (egl) { if (void* p = egl(name)) return p; }
    if (glx) { if (void* p = glx(reinterpret_cast<const unsigned char*>(name))) return p; }
    return dlsym(RTLD_NEXT, name);
}
}  // namespace choir::glapi
