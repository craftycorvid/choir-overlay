# Choir

A Discord voice overlay for Linux: draws an ImGui overlay on top of games, never on the
desktop. Vulkan (incl. DXVK/VKD3D) via an implicit layer — automatic; OpenGL via an
`LD_PRELOAD` interposer — opt-in per launch. Not affiliated with Discord.

We hook the game's **present call**, not the window system: there is no Wayland/X11 code or
build dep anywhere (the only `WAYLAND` string is gamescope's env var). Wayland is the target
and the only tested session — it's why Choir exists, since Discord's own overlay fails there
— but don't add a Wayland assumption; nothing currently justifies one.

VERIFY_LEVEL=tdd
<!-- powers verify-gate: source changes without test changes are blocked. -->

## Build & test

- Build: `meson compile -C build`
  (first time: `meson setup build . --buildtype=release -Dbuild_tests=true` — `build_tests`
  defaults to **false**, so a plain setup builds no tests)
- Test: `meson test -C build` (26 tests; the golden layer/GL tests render on the real GPU)
- Verify (what the powers gate runs): `scripts/verify.sh`
- Per-user install (both backends + host → ~/.local): `bash packaging/install-user.sh`
  - Backend changes (`src/layer/`, `src/gl/`, `src/overlay/`) are injected into the game
    process → **relaunch the game** to pick them up. Host-only changes just need `choir`
    restarted.
- Pacman package: `cd packaging && makepkg -si` (uses `-Dbuild_tests=false`)
- AppImage (cross-distro): built by `.github/workflows/appimage.yml` on a `v*` tag or
  manual dispatch — never locally. Only the **host** is bundled; the two backends ride
  along as payload at `usr/lib/choir` and the host copies them to `~/.local` on first run
  (`src/host/config/backends.cpp`), because nothing can be dlopened out of an AppImage
  mount that exists only while the host runs. The workflow deletes meson's layer manifest
  and `choir-run` from the AppDir — both bake the `/usr` prefix, and the host regenerates
  them against `~/.local`. See "Cross-distro packaging" below.
- Release: bump `version:` in `meson.build` + `pkgver` in `packaging/PKGBUILD`, tag `vX.Y.Z`,
  `gh release create` (source-only — GitHub attaches the tarball itself).
- AUR (two packages; `packaging/aur/{git,stable}/` are the source of truth, the AUR repos are
  separate git remotes). Edit the PKGBUILD, then in that dir:
  `makepkg --printsrcinfo > .SRCINFO && makepkg -f` (always build before publishing), then
  `bash packaging/aur/publish.sh [git|stable|both]` — it clones each AUR repo into a temp
  dir, copies both files, commits and pushes (creating the package on first import), and
  refuses to publish a `.SRCINFO` that disagrees with its PKGBUILD. Keeps no state, so it is
  safe to re-run. `stable/` needs the new tarball's `sha256sum` after the GitHub release exists.
- Confirm the layer loads: `vulkaninfo | grep -i choir`; for GL, `CHOIR_GL_DEBUG=1 choir-run <game>`

## Architecture

A host and two injected backends, talking over an **abstract unix socket** (shared netns →
reaches inside Steam pressure-vessel containers):

- **Host** (`src/host/`) — Qt6 tray + settings window + Discord RPC client + IPC state server
  (the `choir` binary). Qt-free logic lives in `libchoir_host_core` so it unit-tests without Qt;
  Qt-using TUs (`ui/`, `server/`, `discord/qt_http.cpp`) compile straight into the executable.
- **Vulkan implicit layer** (`src/layer/`) — `libchoir_overlay.so`, an implicit GLOBAL layer
  loaded into every Vulkan game; hooks `vkQueuePresentKHR` and renders ImGui into the swapchain
  image before present.
- **OpenGL interposer** (`src/gl/`) — `libchoir_gl.so`, an `LD_PRELOAD` lib (opt-in via the
  `choir-run` wrapper, since GL has no implicit-layer mechanism) that hooks
  `eglSwapBuffers`/`glXSwapBuffers` and renders ImGui before present. SDR-only.
- **Shared overlay core** (`src/overlay/`) — backend-agnostic drawing (`overlay_ui`,
  `state_client`, `gating`, `fade`) behind `IAvatarTextures` + `Extent2D`; linked by BOTH
  backends, so the panel/toasts are identical in Vulkan and GL.
- **IPC** (`src/ipc/`) — shared `Snapshot`/`AppearanceConfig` + JSON framing, the XDG path
  helpers (`paths.hpp`: config, cache, autostart, data home, Vulkan manifest,
  abstract-socket name), avatar-file decoding, and `emoji.hpp` (splits notification text
  into text/emoji runs).
- `tests/`, `packaging/` (install script + PKGBUILD + icons + desktop entry),
  `docs/specs/` (design specs) + `docs/plans/` (the implementation plans built from them).

The host's icons (`packaging/icons/choir.svg` on a blurple disc, `choir-symbolic.svg` bare)
are compiled in via `choir.qrc` AND installed to hicolor. The tray glyph is recoloured
white/black at runtime from `QStyleHints::colorScheme()`. Rendering them needs Qt's SVG
plugins, so the host links `Qt6::Svg` — without it `QIcon` yields a blank pixmap silently.

Dear ImGui comes from a meson **wrap** (`subprojects/imgui.wrap` — only the .wrap files are
committed, so a fresh clone downloads on first `meson setup`; both PKGBUILDs pre-seed
`subprojects/packagecache/` to keep the package build offline), built **static** into each
backend with its own
renderer backend TU (`imgui_impl_vulkan_unity.cpp` / `imgui_impl_opengl3_unity.cpp`); the
Vulkan layer feeds ImGui function pointers via its own dispatch, never the global loader.

## Cross-distro packaging (why AppImage and not Flatpak)

The constraint that decides everything: **the injected backends cannot live inside a
bundle.** The Vulkan loader reads `library_path` from the manifest inside a *game*
process, and `choir-run` `LD_PRELOAD`s a path into a game — both long after an AppImage's
`/tmp/.mount_XXXXXX` is unmounted. So the `.so` files must sit on the real filesystem.

That costs almost nothing here, because only the host is heavy. The backends link
`libstdc++ libgcc_s libm libc` and nothing else (ImGui static, Vulkan via the layer's own
dispatch, EGL/GLX via `dlsym`); `-Dstatic_backend_cxx=true` drops that to `libm libc`.
The host links ~70 libraries including Qt6, and **must** be bundled regardless — it needs
`QStyleHints::colorScheme()` (Qt 6.5+) and Ubuntu 24.04 LTS ships Qt 6.4.

- `static_backend_cxx` applies **only to the two `shared_library` targets**, never the
  host: a statically-linked libstdc++ in the host would coexist with the one Qt6 links
  dynamically, putting two C++ runtimes either side of every Qt call. Off by default so
  the AUR/pacman packages keep linking the system C++ runtime.
- Verify after touching it: `ldd` shows only `libm`/`libc`, and `nm -CD` still shows
  exactly the 3 Vulkan entrypoints for the layer and **unversioned** hooks for GL (the
  version scripts do the hiding; static linking must not leak past them).
- **Flatpak can't do this**, and not because of the sandbox — the copied-out `.so` would
  inherit the *runtime's* glibc (`org.kde.Platform//6.8` = 2.40, `//25.08` = 2.42), a
  floor you don't control and that rises at every runtime bump. Flathub would also reject
  an app registering a GLOBAL Vulkan layer, and `XDG_DATA_HOME` redirection would force a
  sandbox special-case into `paths.hpp` — code both backends link. Don't revisit this.

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
- **Gamescope rewrites the swapchain color space.** Its WSI layer (`VK_LAYER_FROG_gamescope_wsi`)
  advertises HDR color spaces to the game but forces `imageColorSpace` to `SRGB_NONLINEAR` before
  the create-info reaches us (HDR moves over its private Wayland protocol). Under gamescope
  (`GAMESCOPE_WAYLAND_DISPLAY`) infer the transfer from the FORMAT: FP16 → scRGB; A2*10 → PQ only
  when `DXVK_HDR` is also set (10-bit sRGB is a legit SDR config). See `swapchain_color.hpp`.
- **Never submit our graphics command buffers to a non-graphics present queue.** Gamescope is
  itself a Vulkan app this GLOBAL layer loads into, and it composites/presents on a
  **compute-only** queue (while also creating a graphics queue, so state building succeeds). A
  render pass submitted there intermittently hangs the GPU → device lost takes down gamescope AND
  every game inside it. The present hook checks the present queue's family (all queues captured at
  `vkCreateDevice`) against our graphics family and forwards otherwise.
- **Submit nothing when there is nothing to draw.** Host-disabled (denylisted) processes, games
  never/not in voice, and empty frames are pure forwards — no fence wait, no empty LOAD/STORE
  pass, untouched present wait-semaphores (`RecordResult::Skipped`). Gamescope's present_wait-based
  frame pacing must not carry overlay submits; this also keeps the overlay at zero GPU cost
  outside voice.
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

`DISABLE_CHOIR_OVERLAY=1` (off for one launch; honoured by BOTH backends) ·
`CHOIR_DEBUG_FORMAT=1` (log swapchain format/colorspace/transfer/nits) ·
`CHOIR_HDR_NITS=<80..1000>` · `CHOIR_DEBUG_LAZY_INIT=1` · `CHOIR_DEBUG_AVATARS=1` ·
`CHOIR_SOCKET=<name>` (abstract-socket override; tests use it for unique names) ·
`CHOIR_DEBUG_DUMP=<path>` (write the first received snapshot as JSON — golden-test hook) ·
`CHOIR_FONT=<path.ttf>` (overlay font override; default is the first system DejaVu/Noto/
Liberation found — see `load_overlay_font`) ·
`CHOIR_GL_DEBUG=1` (GL interposer: log injection + comm-name + gating decision + per-context init) ·
`CHOIR_GL_LIB=<path.so>` (read by the `choir-run` wrapper to override the preloaded lib).
