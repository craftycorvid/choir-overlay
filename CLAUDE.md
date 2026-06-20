# Choir

A Wayland-only, Vulkan-only Discord voice overlay for Linux: draws an ImGui overlay on top
of Vulkan games (incl. DXVK/VKD3D), never on the desktop. Not affiliated with Discord.

## Build & test
- Build:  `meson compile -C build`  (first time: `meson setup build . --buildtype=release`)
- Test:   `meson test -C build`  (22 tests; the golden layer tests render on the real GPU)
- Per-user install (layer + host → ~/.local): `bash packaging/install-user.sh`
  - This is a Vulkan **layer** change → **relaunch the game** to pick it up.
- Pacman package: `cd packaging && makepkg -si`  (uses `-Dbuild_tests=false`)
- Confirm the layer loads: `vulkaninfo | grep -i choir`

## Architecture
Two components talking over an **abstract unix socket** (shared netns → reaches inside Steam
pressure-vessel containers):
- **Host** (`src/host/`) — Qt6 tray + Discord RPC client + IPC state server (the `choir` binary).
- **Vulkan implicit layer** (`src/layer/`) — `libchoir_overlay.so`, an implicit GLOBAL layer
  loaded into every Vulkan game; hooks `vkQueuePresentKHR` and renders ImGui into the swapchain
  image before present.
- **IPC** (`src/ipc/`) — shared `Snapshot`/`AppearanceConfig` + JSON framing.
- `tests/`, `packaging/` (install script + PKGBUILD), `docs/superpowers/` (design specs/plans).

Dear ImGui is vendored (`subprojects/`), built **static** into the layer with
`VK_NO_PROTOTYPES`; the layer feeds it Vulkan function pointers via its own dispatch
(`imgui_impl_vulkan_unity.cpp`), never the global loader.

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
- Golden tests can't exercise true HDR (no HDR headless surface) and run on the **real NVIDIA GPU**
  — make GPU-corrupting fixes **safe-by-construction** so a regression fails the test, not the GPU.

## Debug env vars
`DISABLE_CHOIR_OVERLAY=1` (off for one launch) · `CHOIR_DEBUG_FORMAT=1` (log swapchain
format/colorspace/transfer/nits) · `CHOIR_HDR_NITS=<80..1000>` · `CHOIR_DEBUG_LAZY_INIT=1` ·
`CHOIR_DEBUG_AVATARS=1` · `CHOIR_SOCKET=<name>` (abstract-socket override).
