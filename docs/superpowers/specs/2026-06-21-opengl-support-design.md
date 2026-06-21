# OpenGL game support — design

**Date:** 2026-06-21
**Status:** Approved (design); implementation pending
**Topic:** Add a Choir voice-overlay backend for OpenGL games (EGL + GLX) alongside the existing Vulkan implicit layer.

## Goal

Draw the same Choir voice overlay (participant panel + notification toasts + avatars)
on top of **OpenGL** games on Linux, at feature parity with the Vulkan path, without
touching the GPU-corruption-sensitive parts of the Vulkan layer.

OpenGL has no loader-layer mechanism like Vulkan implicit layers, so the GL overlay is
an `LD_PRELOAD` interposer rather than an auto-loaded layer. It is **opt-in per launch**.

## Decisions

| Question | Decision |
| --- | --- |
| Which present path(s) to hook | **Both** `eglSwapBuffers` (native-Wayland EGL) and `glXSwapBuffers` (X11/GLX via XWayland). MangoHud-style broad coverage. |
| How users opt in | A `choir-run` wrapper that sets `LD_PRELOAD`; reuses the existing denylist + voice gating once injected. |
| Test depth | Headless EGL **golden** test (pbuffer + RGBA8 FBO), matching the Vulkan golden rigor. |
| Color | **SDR-only**, default `imgui_impl_opengl3` pipeline — no HDR shader (no HDR GL swapchain exists on Linux). |
| Code structure | Separate `libchoir_gl.so` + an extracted backend-agnostic shared static lib (Approach A). |
| ABI | **64-bit only** for v1; the wrapper already supports `lib32` via `$LIB` for a later add. |

## Architecture

Three build targets. A new shared static lib holds the backend-agnostic code; each
backend `.so` links it and adds its own ImGui renderer backend + texture upload.

```
src/overlay/   (new) → static lib choir_overlay_shared  — linked by BOTH backends
    overlay_ui.{cpp,hpp}     moved from src/layer; VkExtent2D → choir::Extent2D{width,height}
    iavatar_textures.hpp     (new) interface: lookup(hash), get_or_load(req) → ImTextureID
    state_client.{cpp,hpp}   moved from src/layer (pure IPC + AvatarReq queue, no GPU)
    gating.{cpp,hpp}         moved from src/layer (denylist + voice gating, no GPU)
    fade.hpp                 moved from src/layer
    + vendored ImGui core (compiled once here)

src/layer/   (Vulkan) → libchoir_overlay.so   — unchanged in spirit
    layer_entry, dispatch, swapchain, swapchain_color/_usage, srgb,
    imgui_renderer, imgui_impl_vulkan_unity, shaders/, manifest/
    avatar_textures → class renamed VkAvatarTextures : IAvatarTextures

src/gl/      (new) → libchoir_gl.so
    interposer.cpp           hooks eglSwapBuffers / glXSwapBuffers (+ GetProcAddress/dlsym, destroy)
    gl_renderer.{cpp,hpp}    NewFrame → draw_overlay → imgui_impl_opengl3_RenderDrawData
    gl_avatar_textures.{cpp,hpp}   GlAvatarTextures : IAvatarTextures (glTexImage2D)
    + vendored imgui_impl_opengl3 (self-contained GL loader, no new dependency)

packaging/   choir-run wrapper + install-user.sh / PKGBUILD updates
```

The Vulkan layer's only changes: the shared `overlay_ui` takes `Extent2D` instead of
`VkExtent2D`, and `VkAvatarTextures` gains the `IAvatarTextures` base. Its
corruption-sensitive files (`swapchain.cpp`, render pass, barriers) are untouched.

### Why `ImTextureID` works across both backends

The shared lib compiles ImGui **core** + `overlay_ui` once. Each `.so` links its own
ImGui *backend* (`imgui_impl_vulkan` vs `imgui_impl_opengl3`). `overlay_ui` treats
`ImTextureID` as opaque and never interprets it — the Vulkan backend reads it as a
`VkDescriptorSet`, the GL backend as a `GLuint`. Both fit the pointer-sized
`ImTextureID`, so there is no divergence in shared code.

## Component: GL interposer (`src/gl/interposer.cpp`)

The load-bearing part. `LD_PRELOAD`ed into the game; its exported symbols shadow the
real GL ones.

**Exported hooks (symbol interposition):**
- `eglSwapBuffers`, `eglSwapBuffersWithDamageKHR`, `eglSwapBuffersWithDamageEXT`,
  `glXSwapBuffers` — the draw points.
- `eglDestroyContext`, `glXDestroyContext` — tear down per-context state + GL objects.
- `eglGetProcAddress`, `glXGetProcAddress`, `glXGetProcAddressARB`, and **`dlsym`** —
  apps/toolkits (GLFW, SDL) often resolve swap functions dynamically, bypassing the PLT
  and our symbol shadowing; intercepting these returns *our* hook for the swap names
  above. The `dlsym` hook is essential for GLFW/SDL coverage.

**No hard EGL/GLX link.** Every real function is resolved lazily via
`dlsym(RTLD_NEXT, …)` and cached. The lib deliberately does **not** link `libEGL` or
`libGLX`, so `LD_PRELOAD`ing it into a GLX-only game (or one lacking EGL) cannot fail to
load. Each game pulls in only the GL libs it already uses.

**Per-swap flow** (same shape for EGL and GLX):
1. Resolve the current context (`eglGetCurrentContext` / `glXGetCurrentContext`) and the
   surface extent (`eglQuerySurface` W/H, or `glXQueryDrawable` / GL viewport for GLX).
2. **Gate** via the shared `gating` + `StateClient` snapshot — identical denylist +
   `in_voice` logic as the Vulkan layer. Not drawing → straight to the real swap.
3. **Lazy-init per GL context** (keyed by context handle, mutex-guarded): create an
   ImGui context, `imgui_impl_opengl3_Init`, bind a `GlAvatarTextures`.
4. Make our ImGui context current → draw via `gl_renderer` into FBO 0
   (`imgui_impl_opengl3` saves/restores the game's GL state around the draw).
5. Call the cached real swap.

**Thread model:** a GL context is current on one thread at a time and swaps happen on
that thread, so each context's ImGui state is single-threaded by construction; the
context→state map is mutex-guarded. Destroy hooks free that context's backend + GL
objects.

**Fail-safe:** any init/resolve failure for a context marks it "skip"; we present the
game's frame untouched. A broken overlay never crashes the game.

## Component: GL renderer + avatar textures (`src/gl/`)

**`gl_renderer`** — owns one context's ImGui-GL backend lifecycle, keeping the
interposer focused on hooking:
- `init()` → `imgui_impl_opengl3_Init(nullptr)` (auto-detects desktop GL vs GLES).
- `draw(snap, textures, client, extent, now_ms)` → `imgui_impl_opengl3_NewFrame` →
  `ImGui::NewFrame` → `draw_overlay(...)` → `ImGui::Render` →
  `imgui_impl_opengl3_RenderDrawData`.
- `shutdown()` → `imgui_impl_opengl3_Shutdown`.

**`GlAvatarTextures : IAvatarTextures`** — reuses the existing `read_avatar_rgba`
(`ipc/avatar_file.hpp`), uploads `GL_RGBA8` via `glGenTextures`/`glTexImage2D`, caches by
hash, returns the `GLuint` (cast to `ImTextureID`); `glDeleteTextures` on shutdown. No
staging buffer, no fence, no descriptor set — roughly a third of `VkAvatarTextures`.

**Color: SDR, default pipeline.** Avatars upload as plain `GL_RGBA8` and the stock
`imgui_impl_opengl3` shader samples them directly — the standard SDR look, matching the
Vulkan path's non-HDR case.

## Component: `choir-run` wrapper + packaging

`packaging/choir-run` (POSIX `sh`):
```sh
#!/bin/sh
# Inject the Choir GL overlay: choir-run <game…>  /  Steam: choir-run %command%
lib="${CHOIR_GL_LIB:-/usr/\$LIB/libchoir_gl.so}"   # literal $LIB — expanded by ld.so, not the shell
export LD_PRELOAD="${LD_PRELOAD:+$LD_PRELOAD:}$lib"
exec "$@"
```
- **`$LIB` ABI auto-select:** the dynamic loader expands `$LIB` to `lib`/`lib32`/`lib64`
  per process, so one entry picks the matching ABI. 64-bit ships now; a later 32-bit
  `libchoir_gl.so` in `lib32` works with no wrapper change.
- Co-exists with an existing `LD_PRELOAD` (e.g. `mangohud`) via the `:+` append.
- Gating runs inside the lib, so wrapping a denylisted game injects but draws nothing —
  consistent with the Vulkan path.

**Install/packaging:**
- `install-user.sh`: install `libchoir_gl.so` → `~/.local/lib`, `choir-run` →
  `~/.local/bin`, with `CHOIR_GL_LIB` defaulting to the `~/.local/$LIB` path.
- `PKGBUILD`: package `libchoir_gl.so` + `choir-run`. `meson` builds both `.so` targets
  by default (no new opt-in flag).

## Testing

**Headless EGL golden test** (`tests/`, runs on the real GPU like the Vulkan goldens):
- Create an offscreen GL context — a **pbuffer** EGL surface (broadest driver support,
  incl. NVIDIA) with an `RGBA8` FBO render target.
- Reuse the existing golden-test **Snapshot fixtures** + image-compare helper; render
  the overlay through `gl_renderer` + `GlAvatarTextures`, `glReadPixels`, compare to a
  **new GL golden PNG** (its own baseline — GL's rasterizer/shader differ slightly from
  the Vulkan path).
- Scope: exercises the **render + avatar path**, not the `LD_PRELOAD`/`dlsym` hooking
  (verified manually in a real game; not meaningfully unit-testable).
- Wired under the existing `build_tests` option.

## Error handling / robustness

- Every interposer/renderer failure path degrades to "present the game's frame
  untouched." Unlike the Vulkan layer there is no manual barrier/queue surgery, so the
  GPU-corruption class of bugs largely does not exist here.
- Per-context state is mutex-guarded; ImGui is touched single-threaded per context by
  construction.
- The `dlsym` hook chains to the real `dlsym` via `RTLD_NEXT` and must avoid infinite
  recursion (the fiddliest correctness point in the interposer).

## Limitations / out of scope (v1)

- **SDR only.** No HDR GL path (none exists on Linux GL today). Documented in `docs/`.
- **64-bit only.** 32-bit GL games come later via a `lib32` build; the wrapper already
  supports it.
- **No input.** Same as the Vulkan path — the overlay is pure-draw, click-through by
  construction.
- Interposer hooking correctness is verified manually, not by automated test.

## Reused as-is (no changes)

`src/ipc/*` (framing, protocol, state, avatar_file, paths), the host (`src/host/*`),
and the abstract-socket transport — all backend-agnostic. The GL lib talks to the same
host over the same abstract unix socket via the shared `StateClient`.
