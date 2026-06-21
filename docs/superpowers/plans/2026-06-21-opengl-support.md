# OpenGL Game Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Draw the Choir voice overlay on top of OpenGL games (EGL + GLX) at feature parity with the Vulkan layer, via an `LD_PRELOAD` interposer that shares the existing overlay-drawing code.

**Architecture:** Extract the backend-agnostic overlay code (`overlay_ui`, `state_client`, `gating`, `fade`) into a `src/overlay/` static lib behind a 2-method `IAvatarTextures` interface and a plain `Extent2D`. The Vulkan layer (`libchoir_overlay.so`) and a new GL interposer (`libchoir_gl.so`) each link that core and add their own ImGui renderer backend + avatar-texture upload. The GL lib hooks `eglSwapBuffers`/`glXSwapBuffers` (plus `dlsym`/`*GetProcAddress`), resolving all GL/EGL/GLX functions dynamically so it hard-links nothing but `libdl`. A `choir-run` wrapper injects it. A headless EGL pbuffer golden test verifies the GL render path on the real GPU.

**Tech Stack:** C++20, meson, Dear ImGui (vendored, static) + `imgui_impl_opengl3`, EGL/GLX/GL (dynamically resolved), libdl. Mirrors the existing Vulkan-layer build patterns.

**Source spec:** `docs/superpowers/specs/2026-06-21-opengl-support-design.md`

---

## File structure

**New — shared core (`src/overlay/`):**
- `meson.build` — `overlay_inc = include_directories('.')`; `static_library('choir_overlay_shared', …)`.
- `iavatar_textures.hpp` — `struct Extent2D` + `class IAvatarTextures` (interface).
- `overlay_ui.{cpp,hpp}`, `state_client.{cpp,hpp}`, `gating.{cpp,hpp}`, `fade.hpp` — **moved** from `src/layer/`.

**New — GL backend (`src/gl/`):**
- `meson.build` — `imgui_impl_opengl3` unity backend static lib + `libchoir_gl.so`.
- `gl_api.hpp` — minimal EGL/GLX/GL typedefs + a `gl_get_proc(name)` resolver (no system GL/EGL/GLX dev headers needed).
- `imgui_impl_opengl3_unity.cpp` — thin TU that `#include`s the vendored `imgui_impl_opengl3.cpp`.
- `gl_renderer.{cpp,hpp}` — per-context ImGui-GL frame driver.
- `gl_avatar_textures.{cpp,hpp}` — `GlAvatarTextures : IAvatarTextures` (GL texture upload).
- `interposer.cpp` — the `eglSwapBuffers`/`glXSwapBuffers`/`dlsym`/`*GetProcAddress`/destroy hooks.
- `choir_gl.version` — export only the hook symbols; hide ImGui/everything else.

**Modified:**
- `src/layer/avatar_textures.hpp` — `class AvatarTextures : public IAvatarTextures` (+`override`).
- `src/layer/imgui_renderer.cpp` — pass `Extent2D{w,h}` to `draw_overlay`.
- `src/layer/meson.build` — drop moved sources; `link_with` the shared lib; add `overlay_inc`.
- `meson.build` (root) — `subdir('src/overlay')` before `src/layer`; `subdir('src/gl')`; configure `choir-run`; install it.
- `tests/meson.build` — `t_fade` gains `overlay_inc`; add the GL golden test.
- `packaging/{install-user.sh,PKGBUILD,choir-run.in}` — install/package the GL lib + wrapper.

**Naming note (deliberate deviation from the spec):** the spec called the Vulkan class `VkAvatarTextures`. We keep the name `AvatarTextures` and only add the interface base — the rename was cosmetic and renaming touches `imgui_renderer`/`swapchain` for no behavioral gain. The GL class is `GlAvatarTextures`.

---

### Task 6: Extract shared overlay core (`src/overlay/`)

**Goal:** Move the backend-agnostic overlay code into a static lib behind `IAvatarTextures`/`Extent2D`, with the Vulkan layer rebuilt on top and all existing tests still passing.

**Files:**
- Create: `src/overlay/iavatar_textures.hpp`
- Create: `src/overlay/meson.build`
- Move: `src/layer/overlay_ui.{cpp,hpp}` → `src/overlay/overlay_ui.{cpp,hpp}`
- Move: `src/layer/state_client.{cpp,hpp}` → `src/overlay/state_client.{cpp,hpp}`
- Move: `src/layer/gating.{cpp,hpp}` → `src/overlay/gating.{cpp,hpp}`
- Move: `src/layer/fade.hpp` → `src/overlay/fade.hpp`
- Modify: `src/overlay/overlay_ui.{cpp,hpp}` (Vulkan→interface), `src/layer/avatar_textures.hpp`, `src/layer/imgui_renderer.cpp`, `src/layer/meson.build`, `meson.build`, `tests/meson.build`

**Acceptance Criteria:**
- [ ] `src/overlay/overlay_ui.{cpp,hpp}` contain **no** Vulkan include or `Vk*` type (`grep -r Vulkan\\\|Vk src/overlay` is empty).
- [ ] `IAvatarTextures` declares exactly the two methods `overlay_ui` calls: `lookup` and `get_or_load`.
- [ ] The Vulkan layer builds and **all 22 existing tests pass** unchanged.
- [ ] `libchoir_overlay.so` still exports only the loader entrypoints (`nm -CD` shows 3 `vk*` globals, no ImGui symbols).

**Verify:** `meson compile -C build && meson test -C build` → all pass; `nm -CD build/src/layer/libchoir_overlay.so | grep -i ' T '` → only `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr`, `vkNegotiateLoaderLayerInterfaceVersion`.

**Steps:**

- [ ] **Step 1: Create the interface header.** `src/overlay/iavatar_textures.hpp`:

```cpp
// Backend-agnostic seam between the shared overlay drawing (overlay_ui.cpp) and a
// concrete GPU texture cache. The Vulkan layer (AvatarTextures) and the GL interposer
// (GlAvatarTextures) each implement this; overlay_ui only needs to resolve an avatar
// hash to an opaque ImTextureID, so the interface is exactly those two calls.
#pragma once

#include <cstdint>
#include <string>

#include "imgui.h"          // ImTextureID
#include "state_client.hpp" // AvatarReq

namespace choir {

// Framebuffer extent in pixels. Replaces VkExtent2D in the shared overlay code so it
// carries no Vulkan dependency. Field names match VkExtent2D for drop-in call sites.
struct Extent2D {
    uint32_t width = 0;
    uint32_t height = 0;
};

// Resolve avatar hashes to textures. RENDER-THREAD ONLY (touches the GPU/ImGui backend).
class IAvatarTextures {
public:
    virtual ~IAvatarTextures() = default;
    // Load (or return cached) texture for `req`; ImTextureID_Invalid on any failure.
    virtual ImTextureID get_or_load(const AvatarReq& req) = 0;
    // Already-loaded texture for `hash`, or ImTextureID_Invalid if not loaded.
    virtual ImTextureID lookup(const std::string& hash) const = 0;
};

}  // namespace choir
```

- [ ] **Step 2: Move the four units with git (preserve history).**

```bash
git mv src/layer/overlay_ui.cpp  src/overlay/overlay_ui.cpp
git mv src/layer/overlay_ui.hpp  src/overlay/overlay_ui.hpp
git mv src/layer/state_client.cpp src/overlay/state_client.cpp
git mv src/layer/state_client.hpp src/overlay/state_client.hpp
git mv src/layer/gating.cpp src/overlay/gating.cpp
git mv src/layer/gating.hpp src/overlay/gating.hpp
git mv src/layer/fade.hpp   src/overlay/fade.hpp
```

- [ ] **Step 3: De-Vulkanize `src/overlay/overlay_ui.hpp`.** Replace the Vulkan include + forward decl + signature. Change:

```cpp
#include <vulkan/vulkan.h>
...
class AvatarTextures;  // src/layer/avatar_textures.hpp
...
void draw_overlay(const Snapshot& snap, AvatarTextures& textures, StateClient& client,
                  VkExtent2D extent, int64_t now_ms);
```
to:
```cpp
#include "iavatar_textures.hpp"   // IAvatarTextures, Extent2D
...
// (delete the AvatarTextures forward declaration)
...
void draw_overlay(const Snapshot& snap, IAvatarTextures& textures, StateClient& client,
                  Extent2D extent, int64_t now_ms);
```
Keep the existing `class AvatarTextures;` line deleted; `StateClient` forward decl stays.

- [ ] **Step 4: De-Vulkanize `src/overlay/overlay_ui.cpp`.** Two edits:
  1. Replace `#include "avatar_textures.hpp"` with `#include "iavatar_textures.hpp"`.
  2. Replace every `VkExtent2D` with `Extent2D` (4 sites: `anchored_pos`, `draw_voice_panel`, `draw_toasts`, `draw_overlay`) and every `AvatarTextures&` with `IAvatarTextures&` (5 sites: `resolve_icon`, `resolve_avatar`, `draw_voice_panel`, `draw_toasts`, `draw_overlay`).

```bash
sed -i 's/VkExtent2D/Extent2D/g; s/AvatarTextures&/IAvatarTextures\&/g' src/overlay/overlay_ui.cpp
```
Then verify no `Vk`/`Vulkan` token remains:
```bash
grep -n 'Vk\|vulkan' src/overlay/overlay_ui.cpp src/overlay/overlay_ui.hpp   # expect: no output
```

- [ ] **Step 5: Make the Vulkan class implement the interface.** In `src/layer/avatar_textures.hpp`, add the include and base class, and mark the two overrides:

```cpp
#include "iavatar_textures.hpp"   // IAvatarTextures (add near the imgui.h / state_client.hpp includes)
...
class AvatarTextures : public IAvatarTextures {
...
    ImTextureID get_or_load(const AvatarReq& req) override;
    ImTextureID lookup(const std::string& hash) const override;
```
(The existing method signatures already match the interface; only add `: public IAvatarTextures` and `override`.) Its existing `#include "state_client.hpp"` now resolves via `overlay_inc` (Step 8).

- [ ] **Step 6: Fix the one Vulkan call site that now needs `Extent2D`.** In `src/layer/imgui_renderer.cpp`, `begin_frame` calls `draw_overlay(*snap, textures, client, extent, now_ms)` where `extent` is a `VkExtent2D`. Change that call to bridge the type:

```cpp
    if (snap) draw_overlay(*snap, textures, client,
                           choir::Extent2D{extent.width, extent.height}, now_ms);
```
`textures` is an `AvatarTextures&` which now implicitly upcasts to `IAvatarTextures&`. No other change to `imgui_renderer.{hpp,cpp}` is needed.

- [ ] **Step 7: Create `src/overlay/meson.build`.**

```meson
# libchoir_overlay_shared: backend-agnostic overlay code linked by BOTH the Vulkan layer
# and the GL interposer. ImGui CORE only (imgui_dep); no renderer backend lives here —
# each .so adds its own (imgui_impl_vulkan / imgui_impl_opengl3).
overlay_inc = include_directories('.')

libchoir_overlay_shared = static_library('choir_overlay_shared',
  files('overlay_ui.cpp', 'state_client.cpp', 'gating.cpp'),
  include_directories: [overlay_inc, ipc_inc],
  link_with: [libchoir_ipc],
  dependencies: [imgui_dep, json_dep, dependency('threads')],
)
```

- [ ] **Step 8: Wire it into the root + layer builds.**
  - In `meson.build` (root), add `subdir('src/overlay')` **immediately before** `subdir('src/layer')` (the layer links the shared lib):

```meson
subdir('src/ipc')
subdir('src/overlay')
subdir('src/host')
subdir('src/layer')
```
  - In `src/layer/meson.build`, remove the three moved files from `layer_sources` (leaving `layer_entry.cpp`, `dispatch.cpp`, `swapchain.cpp`, `imgui_renderer.cpp`, `avatar_textures.cpp`), add `overlay_inc` to the `shared_library` `include_directories`, and add the shared lib to `link_with`:

```meson
layer_sources = files(
  'layer_entry.cpp',
  'dispatch.cpp',
  'swapchain.cpp',
  'imgui_renderer.cpp',
  'avatar_textures.cpp',
)
...
  include_directories: [ipc_inc, overlay_inc, imgui_backends_inc],
  link_with: [libchoir_ipc, libchoir_overlay_shared, imgui_backend_lib],
```

- [ ] **Step 9: Fix the `t_fade` test include** (fade.hpp moved out of `layer_inc`). In `tests/meson.build`, the `fade` test's executable: change `include_directories: layer_inc` to `include_directories: overlay_inc`.

- [ ] **Step 10: Build + run the full suite (the regression net for this refactor).**

Run: `meson setup --reconfigure build . --buildtype=release && meson compile -C build && meson test -C build`
Expected: compile clean; all tests PASS (the GPU golden tests `layer_golden`/`layer_state`/`layer_faults` may report SKIP if no headless surface — that is the existing behavior, not a failure).

- [ ] **Step 11: Confirm exports are unchanged.**

Run: `nm -CD build/src/layer/libchoir_overlay.so | grep ' T '`
Expected: only `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr`, `vkNegotiateLoaderLayerInterfaceVersion` (no `ImGui`/`choir::` globals).

- [ ] **Step 12: Commit.**

```bash
git add -A
git commit -m "refactor: extract backend-agnostic overlay core into src/overlay/

Move overlay_ui/state_client/gating/fade behind IAvatarTextures + Extent2D so a
second (OpenGL) backend can reuse the drawing. Vulkan layer unchanged behaviorally.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 8: GL renderer + `GlAvatarTextures` + vendored `imgui_impl_opengl3`

**Goal:** A per-context ImGui-OpenGL frame driver and a GL avatar-texture cache implementing `IAvatarTextures`, plus the vendored GL backend compiled into a static lib — all reusing the shared `draw_overlay`. (Built and verified by Task 10's golden test; this task delivers the code + a clean compile.)

**Files:**
- Create: `src/gl/gl_api.hpp`, `src/gl/imgui_impl_opengl3_unity.cpp`
- Create: `src/gl/gl_renderer.{cpp,hpp}`, `src/gl/gl_avatar_textures.{cpp,hpp}`
- Create: `src/gl/meson.build` (backend static lib only in this task; the `.so` is added in Task 7)
- Modify: `meson.build` (root) — `subdir('src/gl')`

**Acceptance Criteria:**
- [ ] `GlAvatarTextures : public IAvatarTextures` uploads RGBA8 via GL and caches by hash; `get_or_load`/`lookup` return a `GLuint` cast to `ImTextureID`; `shutdown()` deletes the textures.
- [ ] `gl_renderer` drives `imgui_impl_opengl3_NewFrame → ImGui::NewFrame → draw_overlay → ImGui::Render → imgui_impl_opengl3_RenderDrawData`, reusing the `sane()` DisplaySize clamp from the Vulkan renderer.
- [ ] The GL backend + classes compile into `static_library('choir_gl_objs', …)` with no system GL/EGL/GLX dev-header dependency (only `imgui_dep` + the vendored backend include dir).
- [ ] No `#include` of `<GL/gl.h>`, `<EGL/egl.h>`, or `<GLX/glx.h>` anywhere in `src/gl/` (types are declared in `gl_api.hpp`).

**Verify:** `meson compile -C build` builds the `choir_gl_objs` static lib clean. (Functional verification is Task 10.)

**Steps:**

- [ ] **Step 1: Vendor-confirm the GL backend files exist.**

Run: `ls subprojects/imgui-1.92.5/backends/imgui_impl_opengl3*.{cpp,h}`
Expected: `imgui_impl_opengl3.cpp`, `imgui_impl_opengl3.h`, `imgui_impl_opengl3_loader.h` present. (If the subproject dir version differs, use the actual `subprojects/imgui-*/backends` path; `imgui_backends_inc` in the root `meson.build` already points at it.)

- [ ] **Step 2: `src/gl/gl_api.hpp`** — minimal type + entrypoint resolution, so the lib needs no GL/EGL/GLX dev packages and hard-links nothing but `libdl`:

```cpp
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
```
And `src/gl/gl_api.cpp` (small):
```cpp
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
```
(Add `gl_api.cpp` to the Create list / the `choir_gl_objs` sources.)

- [ ] **Step 3: `src/gl/imgui_impl_opengl3_unity.cpp`** — compile the vendored backend into our lib (mirrors `imgui_impl_vulkan_unity.cpp`). The backend uses its own embedded loader (`imgui_impl_opengl3_loader.h`), which `dlopen`s libGL — so no GL dev headers, only `-ldl` at link:

```cpp
// Compile the vendored Dear ImGui OpenGL3 backend INTO libchoir_gl.so. Like the Vulkan
// unity TU, this #includes the .cpp directly so it sidesteps the meson source sandbox and
// is built with our flags. imgui_impl_opengl3 carries its own GL loader (it dlopens
// libGL), so we add no GL header/lib dependency here.
#include "imgui_impl_opengl3.cpp"
```

- [ ] **Step 4: `src/gl/gl_renderer.hpp`.**

```cpp
// Per-GL-context ImGui frame driver for the Choir GL interposer. Owns the ImGui context
// + the imgui_impl_opengl3 backend for one GL context; draws the shared overlay each
// present. RENDER-THREAD ONLY (the thread that owns the GL context). No-op when not ready.
#pragma once
#include "iavatar_textures.hpp"   // Extent2D
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
```

- [ ] **Step 5: `src/gl/gl_renderer.cpp`** — mirror `ImguiRenderer::begin_frame` (including the `sane()` clamp), GL-flavored:

```cpp
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
```

- [ ] **Step 6: `src/gl/gl_avatar_textures.hpp`.**

```cpp
// GL avatar-texture cache: turns the host's AvatarReady RGBA8 files into GL textures
// usable as ImTextureID, keyed by avatar hash. RENDER-THREAD ONLY (touches GL). The GL
// analogue of src/layer/avatar_textures.* — far simpler (no staging buffer/fence/descr).
#pragma once
#include "iavatar_textures.hpp"
#include <string>
#include <unordered_map>
namespace choir {
class GlAvatarTextures : public IAvatarTextures {
public:
    GlAvatarTextures() = default;
    ~GlAvatarTextures();
    GlAvatarTextures(const GlAvatarTextures&) = delete;
    GlAvatarTextures& operator=(const GlAvatarTextures&) = delete;
    ImTextureID get_or_load(const AvatarReq& req) override;
    ImTextureID lookup(const std::string& hash) const override;
    size_t size() const { return textures_.size(); }
    void shutdown();   // glDeleteTextures all; safe to call repeatedly
private:
    std::unordered_map<std::string, GLuint> textures_;   // hash -> GL texture name
};
}  // namespace choir
```

- [ ] **Step 7: `src/gl/gl_avatar_textures.cpp`** — resolve the handful of GL funcs via `gl_api`, upload `GL_RGBA8`:

```cpp
#include "gl_avatar_textures.hpp"
#include "gl_api.hpp"
#include "ipc/avatar_file.hpp"   // read_avatar_rgba
#include <cstdint>
#include <vector>

namespace choir {
namespace {
// GL constants we need (avoid pulling <GL/gl.h>).
constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_RGBA = 0x1908, GL_RGBA8 = 0x8058;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401, GL_LINEAR = 0x2601;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803;
constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;

using PFNGenTextures   = void (*)(GLsizei, GLuint*);
using PFNBindTexture   = void (*)(GLenum, GLuint);
using PFNTexParameteri = void (*)(GLenum, GLenum, GLint);
using PFNTexImage2D    = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
using PFNDeleteTextures= void (*)(GLsizei, const GLuint*);
using PFNPixelStorei   = void (*)(GLenum, GLint);

struct GL {
    PFNGenTextures   GenTextures   = reinterpret_cast<PFNGenTextures>(glapi::get_proc("glGenTextures"));
    PFNBindTexture   BindTexture   = reinterpret_cast<PFNBindTexture>(glapi::get_proc("glBindTexture"));
    PFNTexParameteri TexParameteri = reinterpret_cast<PFNTexParameteri>(glapi::get_proc("glTexParameteri"));
    PFNTexImage2D    TexImage2D    = reinterpret_cast<PFNTexImage2D>(glapi::get_proc("glTexImage2D"));
    PFNDeleteTextures DeleteTextures = reinterpret_cast<PFNDeleteTextures>(glapi::get_proc("glDeleteTextures"));
    PFNPixelStorei   PixelStorei   = reinterpret_cast<PFNPixelStorei>(glapi::get_proc("glPixelStorei"));
    bool ok() const { return GenTextures && BindTexture && TexParameteri && TexImage2D
                          && DeleteTextures && PixelStorei; }
};
const GL& gl() { static GL g; return g; }
}  // namespace

GlAvatarTextures::~GlAvatarTextures() { shutdown(); }

ImTextureID GlAvatarTextures::lookup(const std::string& hash) const {
    auto it = textures_.find(hash);
    return it == textures_.end() ? ImTextureID_Invalid
                                 : static_cast<ImTextureID>(it->second);
}

ImTextureID GlAvatarTextures::get_or_load(const AvatarReq& req) {
    if (GLuint cached = 0; (cached = [&]{ auto it = textures_.find(req.hash);
            return it == textures_.end() ? 0u : it->second; }()) != 0)
        return static_cast<ImTextureID>(cached);
    if (!gl().ok()) return ImTextureID_Invalid;
    uint32_t w = 0, h = 0; std::vector<uint8_t> rgba;
    if (!read_avatar_rgba(req.path, w, h, rgba) || w == 0 || h == 0
        || rgba.size() < size_t(w) * h * 4)
        return ImTextureID_Invalid;
    GLuint tex = 0;
    gl().GenTextures(1, &tex);
    if (tex == 0) return ImTextureID_Invalid;
    gl().BindTexture(GL_TEXTURE_2D, tex);
    gl().PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl().TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(w), GLsizei(h), 0,
                    GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    textures_.emplace(req.hash, tex);
    return static_cast<ImTextureID>(tex);
}

void GlAvatarTextures::shutdown() {
    if (textures_.empty()) return;
    if (gl().ok())
        for (auto& [hash, tex] : textures_) gl().DeleteTextures(1, &tex);
    textures_.clear();
}
}  // namespace choir
```

- [ ] **Step 8: `src/gl/meson.build`** — backend static lib + the object lib (the `.so` is added in Task 7, which appends to this file):

```meson
# GL backend objects (shared by libchoir_gl.so and the GL golden test). The vendored
# imgui_impl_opengl3 brings its own GL loader (dlopens libGL), so we link only libdl.
gl_backend_lib = static_library('choir_imgui_gl_backend',
  files('imgui_impl_opengl3_unity.cpp'),
  include_directories: [imgui_backends_inc],
  dependencies: [imgui_dep, dependency('dl')],
  cpp_args: ['-Wno-missing-field-initializers', '-Wno-unused-parameter'],
)

choir_gl_objs = static_library('choir_gl_objs',
  files('gl_api.cpp', 'gl_renderer.cpp', 'gl_avatar_textures.cpp'),
  include_directories: [overlay_inc, ipc_inc, imgui_backends_inc],
  link_with: [gl_backend_lib, libchoir_overlay_shared],
  dependencies: [imgui_dep, json_dep, dependency('threads'), dependency('dl')],
  cpp_args: ['-Wno-missing-field-initializers'],
)
```

- [ ] **Step 9: Add `subdir('src/gl')` to the root `meson.build`** after `subdir('src/layer')`:

```meson
subdir('src/layer')
subdir('src/gl')
```

- [ ] **Step 10: Build the GL objects.**

Run: `meson setup --reconfigure build . --buildtype=release && meson compile -C build`
Expected: `choir_imgui_gl_backend` and `choir_gl_objs` build with no error and no GL/EGL header dependency.

- [ ] **Step 11: Commit.**

```bash
git add -A
git commit -m "feat(gl): ImGui-OpenGL renderer + GL avatar textures + vendored backend

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: GL interposer (`libchoir_gl.so`)

**Goal:** The `LD_PRELOAD` shared library: hook `eglSwapBuffers`/`glXSwapBuffers` (+ damage variants, destroy, and `dlsym`/`*GetProcAddress`), gate via the shared `gating`/`StateClient`, lazy-init a `GlRenderer`+`GlAvatarTextures` per GL context, draw, then call the real swap — never crashing the game.

**Files:**
- Create: `src/gl/interposer.cpp`, `src/gl/choir_gl.version`
- Modify: `src/gl/meson.build` (append the `shared_library`), `meson.build` (install the `.so` already handled by `src/gl/meson.build`)

**Acceptance Criteria:**
- [ ] Real functions resolved via `dlsym(RTLD_NEXT, …)`; the `.so` lists **no** `libEGL`/`libGLX` in `ldd` (only libdl/libc/libstdc++/libimgui-statics).
- [ ] Exports exactly the hook symbols (per `choir_gl.version`); `nm -CD` shows no `ImGui`/`choir::` globals.
- [ ] The `dlsym` hook returns our swap hooks for the hooked names and otherwise chains to the real `dlsym` (resolved via `dlvsym(RTLD_NEXT,"dlsym","GLIBC_2.2.5")`) without infinite recursion.
- [ ] Per-swap: resolve context+extent → gate (`overlay_disabled()` / `StateClient::disabled()` / `snapshot.in_voice`) → lazy per-context init → draw → real swap. Not drawing → straight to real swap.
- [ ] Any init/resolve failure marks the context "skip"; the real swap is always called.
- [ ] The context→state map is mutex-guarded; destroy hooks tear down the matching state.

**Verify:** `meson compile -C build` builds `libchoir_gl.so`; `ldd build/src/gl/libchoir_gl.so | grep -E 'libEGL|libGLX'` → empty; `nm -CD build/src/gl/libchoir_gl.so | grep ' T '` → only the hook names.

**Steps:**

- [ ] **Step 1: `src/gl/choir_gl.version`** — export only the interposer hooks (everything else local, so our static ImGui never interposes a game's ImGui):

```
CHOIR_GL_1 {
  global:
    eglSwapBuffers;
    eglSwapBuffersWithDamageKHR;
    eglSwapBuffersWithDamageEXT;
    glXSwapBuffers;
    eglDestroyContext;
    glXDestroyContext;
    eglGetProcAddress;
    glXGetProcAddress;
    glXGetProcAddressARB;
    dlsym;
  local:
    *;
};
```

- [ ] **Step 2: `src/gl/interposer.cpp`** — the hooks. Structure (full skeleton; fill the per-API extent query as shown):

```cpp
// Choir OpenGL overlay interposer. LD_PRELOAD'd into a GL game; our exported symbols
// shadow the real eglSwapBuffers/glXSwapBuffers (and dlsym, so toolkits that resolve the
// swap dynamically still hit us). On each swap we draw the shared overlay into the back
// buffer, then call the real swap. Everything is resolved at runtime — we link no
// windowing lib — and every failure degrades to "present the game's frame untouched".
#include "gl_api.hpp"
#include "gl_renderer.hpp"
#include "gl_avatar_textures.hpp"
#include "gating.hpp"
#include "state_client.hpp"
#include "ipc/state.hpp"

#include <dlfcn.h>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>

#define CHOIR_EXPORT extern "C" __attribute__((visibility("default")))

namespace {
using namespace choir;

// Per-GL-context overlay state. Created lazily on first swap for a context; freed by the
// destroy hooks. `skip` latches a failed init so we stop retrying for that context.
struct CtxState {
    GlRenderer renderer;
    GlAvatarTextures textures;
    bool skip = false;
};
std::mutex g_mutex;
std::unordered_map<void*, CtxState*> g_ctxs;   // GL context handle -> state

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Resolve+cache a real function by name via the next object in the chain.
template <class Fn> Fn real(const char* name) {
    static Fn fn = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
    return fn;
}

// Shared draw path for both EGL and GLX. `ctx_key` is the current GL context handle;
// (w,h) the surface extent. Returns having drawn (or not) — caller then calls real swap.
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
        if (it == g_ctxs.end()) {
            st = new CtxState();
            g_ctxs.emplace(ctx_key, st);
        } else {
            st = it->second;
        }
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
        st = it->second;
        g_ctxs.erase(it);
    }
    st->textures.shutdown();   // delete GL textures while the context is still current
    st->renderer.shutdown();
    delete st;
}
}  // namespace

// ---- EGL -------------------------------------------------------------------------------
CHOIR_EXPORT EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    auto egl_ctx = real<EGLContext (*)()>("eglGetCurrentContext");
    auto egl_query = real<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint*)>("eglQuerySurface");
    void* ctx = egl_ctx ? egl_ctx() : nullptr;
    EGLint w = 0, h = 0;
    if (egl_query) { egl_query(dpy, surf, /*EGL_WIDTH*/0x3057, &w);
                     egl_query(dpy, surf, /*EGL_HEIGHT*/0x3056, &h); }
    draw_overlay_for(ctx, uint32_t(w > 0 ? w : 0), uint32_t(h > 0 ? h : 0));
    return real<EGLBoolean (*)(EGLDisplay, EGLSurface)>("eglSwapBuffers")(dpy, surf);
}
CHOIR_EXPORT EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surf, EGLint* rects, EGLint n) {
    // Draw once, then forward to the real damage variant. (Reuse the plain-swap draw path;
    // the overlay region is presented because we draw into the back buffer before swap.)
    auto egl_ctx = real<EGLContext (*)()>("eglGetCurrentContext");
    auto egl_query = real<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint*)>("eglQuerySurface");
    void* ctx = egl_ctx ? egl_ctx() : nullptr;
    EGLint w = 0, h = 0;
    if (egl_query) { egl_query(dpy, surf, 0x3057, &w); egl_query(dpy, surf, 0x3056, &h); }
    draw_overlay_for(ctx, uint32_t(w > 0 ? w : 0), uint32_t(h > 0 ? h : 0));
    return real<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint*, EGLint)>(
        "eglSwapBuffersWithDamageKHR")(dpy, surf, rects, n);
}
CHOIR_EXPORT EGLBoolean eglSwapBuffersWithDamageEXT(EGLDisplay dpy, EGLSurface surf, EGLint* rects, EGLint n) {
    auto egl_ctx = real<EGLContext (*)()>("eglGetCurrentContext");
    auto egl_query = real<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint*)>("eglQuerySurface");
    void* ctx = egl_ctx ? egl_ctx() : nullptr;
    EGLint w = 0, h = 0;
    if (egl_query) { egl_query(dpy, surf, 0x3057, &w); egl_query(dpy, surf, 0x3056, &h); }
    draw_overlay_for(ctx, uint32_t(w > 0 ? w : 0), uint32_t(h > 0 ? h : 0));
    return real<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint*, EGLint)>(
        "eglSwapBuffersWithDamageEXT")(dpy, surf, rects, n);
}
CHOIR_EXPORT EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    destroy_ctx(ctx);
    return real<EGLBoolean (*)(EGLDisplay, EGLContext)>("eglDestroyContext")(dpy, ctx);
}

// ---- GLX -------------------------------------------------------------------------------
CHOIR_EXPORT void glXSwapBuffers(Display* dpy, GLXDrawable drawable) {
    auto glx_ctx = real<GLXContext (*)()>("glXGetCurrentContext");
    auto glx_query = real<void (*)(Display*, GLXDrawable, int, unsigned int*)>("glXQueryDrawable");
    void* ctx = glx_ctx ? glx_ctx() : nullptr;
    unsigned int w = 0, h = 0;
    if (glx_query) { glx_query(dpy, drawable, /*GLX_WIDTH*/0x801D, &w);
                     glx_query(dpy, drawable, /*GLX_HEIGHT*/0x801E, &h); }
    draw_overlay_for(ctx, w, h);
    real<void (*)(Display*, GLXDrawable)>("glXSwapBuffers")(dpy, drawable);
}
CHOIR_EXPORT void glXDestroyContext(Display* dpy, GLXContext ctx) {
    destroy_ctx(ctx);
    real<void (*)(Display*, GLXContext)>("glXDestroyContext")(dpy, ctx);
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
    auto realfn = real<void* (*)(const char*)>("eglGetProcAddress");
    return realfn ? realfn(name) : nullptr;
}
CHOIR_EXPORT void* glXGetProcAddressARB(const unsigned char* name) {
    if (void* h = hook_for(reinterpret_cast<const char*>(name))) return h;
    auto realfn = real<void* (*)(const unsigned char*)>("glXGetProcAddressARB");
    return realfn ? realfn(name) : nullptr;
}
CHOIR_EXPORT void* glXGetProcAddress(const unsigned char* name) {
    if (void* h = hook_for(reinterpret_cast<const char*>(name))) return h;
    auto realfn = real<void* (*)(const unsigned char*)>("glXGetProcAddress");
    return realfn ? realfn(name) : nullptr;
}
CHOIR_EXPORT void* dlsym(void* handle, const char* name) {
    if (void* h = hook_for(name)) return h;
    // Chain to the REAL dlsym (versioned symbol), resolved once. Using dlvsym for the
    // canonical glibc dlsym avoids recursing into this very function.
    static auto real_dlsym = reinterpret_cast<void* (*)(void*, const char*)>(
        dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5"));
    return real_dlsym ? real_dlsym(handle, name) : nullptr;
}
```

Notes baked into the code above (do not skip):
- `dlvsym` requires `#define _GNU_SOURCE` before `<dlfcn.h>`; add it at the very top of the file.
- `real<Fn>()` caches via a function-local `static`, so each hook resolves its target once.
- `eglGetCurrentContext`/`glXGetCurrentContext` are resolved via `RTLD_NEXT` (the real ones), never our copies.

- [ ] **Step 3: Append the `shared_library` to `src/gl/meson.build`.**

```meson
gl_version_script = meson.current_source_dir() / 'choir_gl.version'
libchoir_gl = shared_library('choir_gl',
  files('interposer.cpp'),
  name_prefix: 'lib',
  include_directories: [overlay_inc, ipc_inc, imgui_backends_inc],
  link_with: [choir_gl_objs, libchoir_overlay_shared, libchoir_ipc],
  dependencies: [imgui_dep, json_dep, dependency('threads'), dependency('dl')],
  cpp_args: ['-D_GNU_SOURCE', '-Wno-missing-field-initializers'],
  gnu_symbol_visibility: 'hidden',
  link_args: [
    '-Wl,--version-script=@0@'.format(gl_version_script),
    '-Wl,--exclude-libs,ALL',
  ],
  link_depends: gl_version_script,
  install: true,
  install_dir: get_option('libdir') / 'choir',   # beside libchoir_overlay.so
)
```

- [ ] **Step 4: Build + check the export surface and link deps.**

Run:
```bash
meson compile -C build
ldd build/src/gl/libchoir_gl.so | grep -E 'libEGL|libGLX' ; echo "egl/glx links: $?"
nm -CD build/src/gl/libchoir_gl.so | grep ' T '
```
Expected: the `grep` prints nothing (exit 1 → no EGL/GLX link); `nm` lists only the 10 hook symbols, no `ImGui`/`choir::`.

- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(gl): LD_PRELOAD interposer hooking eglSwapBuffers/glXSwapBuffers

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 10: Headless EGL golden test

**Goal:** An automated test that creates an offscreen EGL pbuffer GL context, renders the overlay (with one real avatar texture) through `GlRenderer`+`GlAvatarTextures`, reads back the pixels, and asserts the overlay drew — verifying the GL render path on the real GPU.

**Files:**
- Create: `tests/gl/test_gl_golden.cpp`
- Modify: `tests/meson.build`

**Acceptance Criteria:**
- [ ] Creates an EGL **pbuffer** context (surfaceless fallback) + an RGBA8 FBO; exits **77 (SKIP)** if no EGL device/context is available (mirrors `vk_min_present`).
- [ ] Builds a synthetic `Snapshot` (in voice, ≥1 participant) and a solid-color avatar file on disk; pre-loads it via `GlAvatarTextures::get_or_load` so `lookup` succeeds.
- [ ] Renders one frame via `GlRenderer::draw`, `glReadPixels` the FBO, and asserts: a pixel in the panel region is **not** the cleared background color (overlay drew), and a far-corner pixel **is** the background.
- [ ] Registered under the existing `build_tests` option and passes via `meson test -C build`.

**Verify:** `meson test -C build gl_golden -v` → PASS (or SKIP where no EGL surface).

**Steps:**

- [ ] **Step 1: `tests/gl/test_gl_golden.cpp`.** Self-contained; links real EGL+GL for context/readback only. Skeleton:

```cpp
// Headless GL golden: render the Choir overlay through the GL backend into an offscreen
// pbuffer FBO and assert it drew. Real-GPU test (like the Vulkan goldens). Exits 77 if no
// EGL context can be created (treated as SKIP by meson).
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <GLES3/gl3.h>          // core GL3/GLES3 entrypoints for the test harness only
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "gl_renderer.hpp"
#include "gl_avatar_textures.hpp"
#include "state_client.hpp"
#include "ipc/state.hpp"
#include "ipc/avatar_file.hpp"   // (write a raw RGBA file directly below; reader is in the lib)

static const int W = 640, H = 360;

int main() {
    // --- EGL surfaceless/pbuffer context -------------------------------------------------
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) { return 77; }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,8, EGL_GREEN_SIZE,8, EGL_BLUE_SIZE,8, EGL_ALPHA_SIZE,8, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) { return 77; }
    const EGLint pb_attr[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
    const EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT
        || !eglMakeCurrent(dpy, surf, surf, ctx)) { return 77; }

    // --- offscreen RGBA8 FBO, cleared to a known background ------------------------------
    GLuint fbo = 0, rb = 0;
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { return 77; }
    glViewport(0, 0, W, H);
    const float BG[3] = {0.10f, 0.20f, 0.40f};            // app "blue"
    glClearColor(BG[0], BG[1], BG[2], 1.0f); glClear(GL_COLOR_BUFFER_BIT);

    // --- a solid-red avatar file on disk -------------------------------------------------
    std::string apath = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp")
                      + "/choir_gl_test_avatar.rgba";
    { std::vector<uint8_t> px(32*32*4); for (size_t i=0;i<px.size();i+=4){px[i]=230;px[i+1]=40;px[i+2]=40;px[i+3]=255;}
      FILE* f = fopen(apath.c_str(), "wb"); if(!f) return 1; fwrite(px.data(),1,px.size(),f); fclose(f); }

    // --- synthetic snapshot: in voice, one participant with that avatar -----------------
    choir::Snapshot snap; snap.in_voice = true;
    choir::Participant p; p.name = "Tester"; p.avatar_hash = "deadbeef"; p.speaking = true;
    snap.participants.push_back(p);                        // (match the real Snapshot/Participant fields)

    // --- render the overlay through the GL backend --------------------------------------
    choir::GlAvatarTextures textures;
    choir::AvatarReq req{ "deadbeef", apath, 32, 32 };
    textures.get_or_load(req);                             // pre-load so lookup() hits
    choir::GlRenderer renderer;
    if (!renderer.init()) { fprintf(stderr, "GlRenderer init failed\n"); return 1; }
    renderer.draw(&snap, textures, choir::StateClient::instance(),
                  choir::Extent2D{uint32_t(W), uint32_t(H)}, /*now_ms*/0);

    // --- read back + assert --------------------------------------------------------------
    std::vector<uint8_t> pixels(size_t(W)*H*4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    auto at = [&](int x, int y){ return &pixels[(size_t(y)*W + x)*4]; };
    auto is_bg = [&](const uint8_t* px){
        return abs(px[0]-int(BG[0]*255))<16 && abs(px[1]-int(BG[1]*255))<16 && abs(px[2]-int(BG[2]*255))<16; };
    // The default panel anchor is top-left; sample inside it and at the far corner.
    bool panel_drawn = !is_bg(at(40, 40));
    bool far_is_bg   =  is_bg(at(W-4, H-4));
    fprintf(stderr, "panel_drawn=%d far_is_bg=%d\n", panel_drawn, far_is_bg);
    if (!panel_drawn || !far_is_bg) return 1;
    fprintf(stderr, "GL golden OK\n");
    return 0;
}
```

> **Implementer note:** the exact `Snapshot`/`Participant` field names (`in_voice`, `participants`, `name`, `avatar_hash`, `speaking`, …) and the panel anchor/region come from `src/ipc/state.hpp`, `src/host/config/config.hpp` (default `Anchor`), and `src/overlay/overlay_ui.cpp`. Read those before finalizing the synthetic snapshot and the sampled pixel coordinates; adjust the sample point to land inside the panel for the default anchor. If GLES3 headers are unavailable, switch the test include to desktop `<GL/gl.h>` + `EGL_OPENGL_BIT`/`EGL_OPENGL_API` and request a 3.x core context — keep everything else identical.

- [ ] **Step 2: Register the test in `tests/meson.build`** (append near the other layer tests):

```meson
# GL backend golden: render the overlay through GlRenderer into an offscreen EGL pbuffer
# FBO and assert it drew. Real-GPU test; exit 77 (no EGL context) is propagated as SKIP.
egl_dep = dependency('egl', required: false)
glesv2_dep = dependency('glesv2', required: false)
if egl_dep.found() and glesv2_dep.found()
  test('gl_golden',
    executable('test_gl_golden',
      'gl/test_gl_golden.cpp',
      link_with: [choir_gl_objs, libchoir_overlay_shared, libchoir_ipc],
      include_directories: [overlay_inc, ipc_inc],
      dependencies: [imgui_dep, json_dep, egl_dep, glesv2_dep, dependency('threads'), dependency('dl')],
    ),
  )
endif
```

- [ ] **Step 3: Build + run.**

Run: `meson setup --reconfigure build . --buildtype=release && meson test -C build gl_golden -v`
Expected: PASS (`GL golden OK`), or SKIP if the CI/host has no EGL device. A FAIL prints `panel_drawn=/far_is_bg=` for diagnosis.

- [ ] **Step 4: Commit.**

```bash
git add -A
git commit -m "test(gl): headless EGL pbuffer golden for the GL overlay render path

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 9: `choir-run` wrapper + packaging

**Goal:** Ship a per-launch injector (`choir-run`) that `LD_PRELOAD`s `libchoir_gl.so`, plus install/package the GL lib and wrapper.

**Files:**
- Create: `packaging/choir-run.in`
- Modify: `meson.build` (root — `configure_file` + install the wrapper), `packaging/install-user.sh`, `packaging/PKGBUILD`

**Acceptance Criteria:**
- [ ] `choir-run <cmd…>` sets `LD_PRELOAD` to the installed `libchoir_gl.so` (absolute baked path), appending to any existing `LD_PRELOAD`, then `exec`s the command.
- [ ] `CHOIR_GL_LIB` env var overrides the baked path.
- [ ] `meson install` lays down `<libdir>/choir/libchoir_gl.so` and `<bindir>/choir-run`; `install-user.sh` reports both.
- [ ] `PKGBUILD` packages the wrapper + GL lib; `pkgdesc` mentions OpenGL.

**Verify:** `meson install --destdir /tmp/choir-stage -C build && /tmp/choir-stage/usr/local/bin/choir-run env | grep LD_PRELOAD` (against a staged prefix) shows the lib path; manual: `choir-run glxgears` injects (overlay draws when in voice).

**Steps:**

- [ ] **Step 1: `packaging/choir-run.in`** — `@CHOIR_GL_LIB@` is substituted with the absolute installed path by meson:

```sh
#!/bin/sh
# Inject the Choir GL overlay into an OpenGL game:
#   choir-run <game…>            # direct
#   choir-run %command%          # Steam launch option
# Gating (denylist + voice state) happens inside the lib, so a denylisted/idle game
# injects but draws nothing. Override the lib path with CHOIR_GL_LIB if needed.
# (v1 is 64-bit only; 32-bit support is a later add via a lib32 build + $LIB selection.)
lib="${CHOIR_GL_LIB:-@CHOIR_GL_LIB@}"
if [ ! -e "$lib" ]; then
  echo "choir-run: GL overlay library not found: $lib" >&2
  echo "           set CHOIR_GL_LIB or reinstall Choir." >&2
fi
export LD_PRELOAD="${LD_PRELOAD:+$LD_PRELOAD:}$lib"
exec "$@"
```

- [ ] **Step 2: Generate + install `choir-run` from the root `meson.build`** (after the layer manifest block; the GL lib install dir is `<prefix>/<libdir>/choir`):

```meson
configure_file(
  input: 'packaging/choir-run.in',
  output: 'choir-run',
  configuration: {
    'CHOIR_GL_LIB': get_option('prefix') / get_option('libdir') / 'choir' / 'libchoir_gl.so',
  },
  install: true,
  install_dir: get_option('bindir'),
  install_mode: 'rwxr-xr-x',
)
```

- [ ] **Step 3: Update `packaging/install-user.sh` messaging.** Add the GL artifacts to the install banner + next steps. After the existing `INSTALLED_SO` line (~47), add:

```bash
INSTALLED_GL_SO="${PREFIX}/lib/choir/libchoir_gl.so"
INSTALLED_CHOIR_RUN="${PREFIX}/bin/choir-run"
```
and in the "Next steps" block (after the Vulkan note, ~117), add:

```bash
echo "  - For OpenGL games (Minecraft, etc.), the overlay is opt-in per launch:"
echo "        choir-run %command%        (Steam launch options)"
echo "        choir-run <game>           (direct)"
```
(`meson install` already places `libchoir_gl.so` + `choir-run`; no extra copy logic is needed — the script just reports them.)

- [ ] **Step 4: Update `packaging/PKGBUILD`.** Change the description and note the GL runtime dep (libGL is provided by `vulkan-icd-loader`'s sibling stack but make it explicit via `libglvnd`):

```bash
pkgdesc="Wayland Discord voice overlay for Vulkan + OpenGL games (not affiliated with Discord Inc.)"
depends=('qt6-base' 'vulkan-icd-loader' 'libglvnd')
```
(The `package()` step already runs `meson install`, which now lays down the GL lib + `choir-run`. No other PKGBUILD change required.)

- [ ] **Step 5: Stage-install and verify placement.**

Run:
```bash
meson install --destdir "$PWD/stage" -C build
find stage -name 'libchoir_gl.so' -o -name 'choir-run'
grep CHOIR_GL_LIB -n stage/usr/local/bin/choir-run 2>/dev/null || sed -n '1,20p' stage/*/bin/choir-run
```
Expected: both artifacts present; `choir-run` has an absolute `lib=...libchoir_gl.so` default (no literal `@CHOIR_GL_LIB@` left).

- [ ] **Step 6: Commit.**

```bash
git add -A
git commit -m "packaging: choir-run GL injector + install/package libchoir_gl.so

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-review

**Spec coverage:** Both surfaces (EGL+GLX) → Task 7 hooks. `choir-run` opt-in → Task 9. Headless EGL golden → Task 10. SDR-only/default pipeline → Task 8 (no HDR path). Approach A structure (shared core + separate `.so`) → Tasks 6–8. 64-bit only → Task 9 (baked path; `$LIB`/lib32 deferred, documented). `ImTextureID` opacity → Task 6 interface + Task 8 GL cast. No-hard-EGL/GLX-link → Task 7 (`gl_api` + `ldd` check). All spec sections map to a task.

**Type consistency:** `IAvatarTextures::{get_or_load(const AvatarReq&), lookup(const std::string&) const}` is defined identically in Task 6 (interface), implemented by `AvatarTextures` (Task 6) and `GlAvatarTextures` (Task 8). `Extent2D{width,height}` used consistently in `overlay_ui` (Task 6), `imgui_renderer` bridge (Task 6 Step 6), `GlRenderer::draw` (Task 8), interposer (Task 7), test (Task 10). `draw_overlay(const Snapshot&, IAvatarTextures&, StateClient&, Extent2D, int64_t)` — same signature at every call site. `choir_gl_objs`/`libchoir_overlay_shared`/`overlay_inc` names consistent across `src/gl/meson.build`, `src/overlay/meson.build`, and `tests/meson.build`.

**Deviations from spec (intentional, noted in-plan):** (1) Vulkan class keeps the name `AvatarTextures` (not `VkAvatarTextures`) to minimize churn. (2) `choir-run` bakes an absolute path via `configure_file` instead of the spec's illustrative `$LIB` literal — correct on Arch's merged `/usr/lib` and matches the 64-bit-only v1 scope; multi-ABI `$LIB`/lib32 is the documented later enhancement.

**Placeholder scan:** No TBD/TODO; every code step shows complete code. The one explicit "read these headers to confirm field names" note (Task 10) points at concrete files for the synthetic-`Snapshot` construction, which depends on `ipc/state.hpp` types not fully quoted here.
