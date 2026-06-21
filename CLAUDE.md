# Choir

A Wayland-only, Vulkan-only Discord voice overlay for Linux: draws an ImGui overlay on top
of Vulkan games (incl. DXVK/VKD3D), never on the desktop. Not affiliated with Discord.

## Build & test

- Build: `meson compile -C build` (first time: `meson setup build . --buildtype=release`)
- Test: `meson test -C build` (22 tests; the golden layer tests render on the real GPU)
- Per-user install (layer + host → ~/.local): `bash packaging/install-user.sh`
  - This is a Vulkan **layer** change → **relaunch the game** to pick it up.
- Pacman package: `cd packaging && makepkg -si` (uses `-Dbuild_tests=false`)
- Confirm the layer loads: `vulkaninfo | grep -i choir`

## Architecture

Two components talking over an **abstract unix socket** (shared netns → reaches inside Steam
pressure-vessel containers):

- **Host** (`src/host/`) — Qt6 tray + Discord RPC client + IPC state server (the `choir` binary).
- **Vulkan implicit layer** (`src/layer/`) — `libchoir_overlay.so`, an implicit GLOBAL layer
  loaded into every Vulkan game; hooks `vkQueuePresentKHR` and renders ImGui into the swapchain
  image before present.
- **OpenGL interposer** (`src/gl/`) — `libchoir_gl.so`, an `LD_PRELOAD` lib (opt-in via the
  `choir-run` wrapper, since GL has no implicit-layer mechanism) that hooks
  `eglSwapBuffers`/`glXSwapBuffers` and renders ImGui before present. SDR-only.
- **Shared overlay core** (`src/overlay/`) — backend-agnostic drawing (`overlay_ui`,
  `state_client`, `gating`, `fade`) behind `IAvatarTextures` + `Extent2D`; linked by BOTH backends.
- **IPC** (`src/ipc/`) — shared `Snapshot`/`AppearanceConfig` + JSON framing.
- `tests/`, `packaging/` (install script + PKGBUILD), `docs/superpowers/` (design specs/plans).

Dear ImGui is vendored (`subprojects/`), built **static** into each backend with its own
renderer backend TU (`imgui_impl_vulkan_unity.cpp` / `imgui_impl_opengl3_unity.cpp`); the
Vulkan layer feeds ImGui function pointers via its own dispatch, never the global loader.

## Layer gotchas (hard-won — read before touching `src/layer/`)

- **Mimic MangoHud** for layer correctness; don't blindly copy a fragment (e.g. a render-pass
  layout) without its accompanying barrier.
- **Never `vkDeviceWaitIdle`/`vkQueueWaitIdle` from a layer entrypoint** — DXVK drives the queue
  from another thread, so a device-wide wait races it → GPU corruption (NVRM Xid 13/32 → device
  lost). Wait your own per-image fences (`wait_overlay_idle`).
- **Add `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`** to the swapchain in `CreateSwapchainKHR`
  (`swapchain_usage.hpp`) — DXVK's D3D11 path omits it; rendering into such images faults the GPU.
- **Render pass** is `PRESENT_SRC_KHR` in/out (self-contained). Do NOT switch initialLayout to
  `COLOR_ATTACHMENT_OPTIMAL` without ALSO adding the explicit pre-pass image barrier.
- **HDR is the common case here.** The overlay draws through a custom HDR fragment-shader pipeline
  (`swapchain_color.hpp` transfer model + `shaders/overlay_hdr.frag`, mode+nits as specialization
  constants). DXVK/VKD3D HDR swapchains are FP16 `PASS_THROUGH`/scRGB or HDR10 PQ.
- Golden tests can't exercise true HDR (no HDR headless surface) and run on the **real GPU**
  — make GPU-corrupting fixes **safe-by-construction** so a regression fails the test, not the GPU.

## GL interposer gotchas (hard-won — read before touching `src/gl/`)

These are all real LWJGL/Minecraft+Iris failures, in the order they bit us:

- **Export the hooks UNVERSIONED** (`choir_gl.version` uses an *anonymous* `{ … }` node, not a
  named one). A named node stamps `sym@@CHOIR_GL_1`; toolkits resolve the swap via
  `dlsym(handle, "glXSwapBuffers")` and their `dlsym` reference is the versioned
  `dlsym@GLIBC_2.2.5`/`@GLIBC_2.34`, which a versioned definition won't satisfy → falls through
  to libc and bypasses us. Unversioned defs satisfy both versioned and unversioned refs.
- **Resolve real GL/GLX/EGL via vendor-lib `dlopen`, not just `RTLD_NEXT`** (`real_gl_proc`,
  `glapi::vendor_sym`). GLFW `dlopen`s `libGL`/`libGLX`/`libEGL` with `RTLD_LOCAL`, so their
  symbols aren't global; `RTLD_NEXT` returns null for the real swap (→ black screen, frames
  never present) and `glXGetCurrentContext` (→ no context, overlay never draws). Fall back to
  `dlopen(lib, RTLD_NOLOAD)` + `dlsym(handle, …)`. Never `RTLD_DEFAULT` — it finds our own hook.
- **Only redirect the SWAP names** in `hook_for`, never the `*GetProcAddress` resolvers. Handing
  the game our resolver routes its context setup (`glXCreateContextAttribsARB`, …) through us and
  breaks 3.3 context creation. The swap is already caught via our `dlsym` hook + PLT interposition.
- **Neutralize pixel-unpack state around texture uploads** (`ScopedCleanUnpack` in
  `gl_renderer.cpp`). Iris leaves a `GL_PIXEL_UNPACK_BUFFER` bound + non-default unpack params, so
  `glTexImage2D`/`glTexSubImage2D` read from the PBO → garbled/missing glyphs + avatars. Save,
  reset to defaults, restore.
- **Never call bare `dlsym` from our code** — we EXPORT `dlsym`, so it'd recurse. Use the genuine
  one via `dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5")` (`real_dlsym`).
- The headless EGL golden test runs on the **real GPU** but can't reproduce the LWJGL-specific
  load quirks above (RTLD_LOCAL, PBOs) — those need manual testing in a real GL game.

## Debug env vars

`DISABLE_CHOIR_OVERLAY=1` (off for one launch) · `CHOIR_DEBUG_FORMAT=1` (log swapchain
format/colorspace/transfer/nits) · `CHOIR_HDR_NITS=<80..1000>` · `CHOIR_DEBUG_LAZY_INIT=1` ·
`CHOIR_DEBUG_AVATARS=1` · `CHOIR_SOCKET=<name>` (abstract-socket override) ·
`CHOIR_GL_DEBUG=1` (GL interposer: log injection + comm-name + gating decision + per-context init).
