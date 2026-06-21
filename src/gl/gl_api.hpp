// Minimal GL/EGL/GLX surface for the interposer + GL avatar upload. We declare only the
// handles/enums/signatures we use and resolve every function pointer at runtime, so
// libchoir_gl.so depends on NO windowing lib (libEGL/libGLX) — it loads cleanly into an
// EGL-only or a GLX-only game alike. Core GL entrypoints are resolved through whichever
// GetProcAddress the game already provides (glvnd dispatches them to the current context).
#pragma once
#include <cstdint>

extern "C" {
// --- opaque handles (sizes match the real ABI: pointers, or XID = unsigned long) ---
typedef void* EGLDisplay;  typedef void* EGLSurface;  typedef void* EGLContext;
typedef unsigned int EGLBoolean;  typedef int32_t EGLint;
typedef void* GLXContext;  typedef unsigned long GLXDrawable;  typedef void Display;
// --- core GL types we touch ---
typedef unsigned int GLenum;  typedef unsigned int GLuint;  typedef int GLint;
typedef int GLsizei;  typedef unsigned char GLubyte;  typedef void GLvoid;
}

namespace choir::glapi {
// Resolve a GL/EGL/GLX function by name: try eglGetProcAddress, then glXGetProcAddressARB,
// then dlsym(RTLD_NEXT). Returns nullptr if unavailable. Cached per name by the caller.
void* get_proc(const char* name);
}  // namespace choir::glapi
