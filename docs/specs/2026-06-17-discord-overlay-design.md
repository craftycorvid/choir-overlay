# Choir — a Linux-native, Wayland-only in-game overlay for Discord

- **Date:** 2026-06-17
- **Status:** Approved (design); implementation plan to follow
- **Codename:** `choir` (installed binaries/library; repo dir stays `discord-overlay`)
- **Not affiliated with, endorsed by, or sponsored by Discord Inc.** "for Discord" is used descriptively.

---

## 1. Summary

A Linux-native, **Wayland-only** Discord overlay that renders **inside the frame of Vulkan
games** (native Vulkan, DXVK = DX9–11→Vulkan, and VKD3D-Proton = DX12→Vulkan) by injecting a
**Vulkan implicit layer** that hooks the game's `vkQueuePresentKHR` and draws with **Dear ImGui** —
the same mechanism MangoHud uses. Because the overlay only ever exists *within a game's swapchain*,
it **never draws on the desktop** and works regardless of compositor (GNOME, KDE, wlroots).

It is **display-only**: it shows who is in your current voice channel, who is speaking,
mute/deafen state, and Discord notification toasts. It captures **no input** from the game, which
deliberately sidesteps the one genuinely unsolved problem on Wayland (a non-compositor client
cannot grab keyboard/mouse away from a focused game).

Voice/notification data comes from the **Discord desktop client's local RPC** over OAuth2, using the
pre-approved **Streamkit `client_id`** (the path the existing "Discover" overlay uses).

---

## 2. Goals / Non-goals

### Goals (v1)
- Render a voice-participant panel + notification toasts **inside Vulkan games** on Wayland.
- Cover native Vulkan, DXVK, and VKD3D-Proton with a **single Vulkan layer**.
- Never render on the desktop; never appear on apps the user excludes.
- Never destabilize or crash the host game; the overlay is strictly additive and click-through.
- Keep all networking, OAuth, and secrets **out of the game process**.

### Non-goals (v1 — deferred to Phase 2+)
- **Interactivity** (clicking members, typing in chat). Wayland forbids input-grab for
  non-compositor clients; explicitly out of scope.
- **OpenGL** games (and `PROTON_USE_WINED3D` D3D→GL). Phase 2 LD_PRELOAD shim.
- **32-bit** games. Phase 2 multilib build.
- **gamescope / Steam Deck** sessions (nested compositor owns the swapchain; needs a separate
  `mangoapp`-style path).
- X11 support — out of scope entirely, by request.

---

## 3. Decisions (locked)

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| 1 | Interactivity | **Display-only** | Avoids Wayland input-grab, the only unsolved hard problem. |
| 2 | Discord data source | **Discord desktop local RPC** | Sanctioned OAuth2 path; gives voice + speaking + notifications. |
| 3 | Render targets | **Vulkan first** (native + DXVK + VKD3D) | One layer covers all three; OpenGL is a separate Phase-2 mechanism. |
| 4 | In-game UI toolkit | **Dear ImGui** (in-game) + **Qt6** (config window) | ImGui composites into a live swapchain; Qt is the desktop settings UI. |
| 5 | v1 panels | **Voice participant panel** + **notification toasts** | Core overlay + pings. No text-chat feed / own-status line in v1. |
| 6 | Activation | **Global with denylist** | Active on all Vulkan apps; a default denylist excludes launchers/clients. |
| 7 | Visibility | **Auto when in a voice channel** | No hotkey → zero input handling required. |
| 8 | OAuth credentials | **Streamkit `client_id`** (gray-area), with **own-app** fallback | Zero-setup; #1 risk, de-risked in milestone M0. |
| 9 | Arch/API scope (v1) | **64-bit, Vulkan only** | Simplest shippable surface; 32-bit/GL are Phase 2. |
| 10 | Language / build / license | **C++17/20 + Meson + Qt6**, **MIT** | Matches the MangoHud/ImGui ecosystem we reuse (all MIT). |

---

## 4. Approach comparison

| Approach | Mechanism | Verdict |
|---|---|---|
| **A. In-game injection** *(chosen)* | Vulkan implicit layer hooks `vkQueuePresentKHR`, draws into the game's swapchain | Draws inside the game frame → survives true-fullscreen, **compositor-agnostic**, and renders **only on Vulkan apps, never the desktop**. Matches every requirement. |
| B. Desktop-surface overlay | Transparent always-on-top window / `wlr-layer-shell` | What all existing Linux Discord overlays do (Discover, Overlayed). Occluded by fullscreen; **no GNOME support** (Mutter lacks layer-shell); draws over the desktop. Rejected. |
| C. gamescope nested compositor | Run each game inside gamescope, use its overlay | Forces wrapping every game; changes the session model. Rejected for v1. |

Research confirmed **no existing Linux Discord overlay uses injection** (Discover's author lists
"OpenGL & Vulkan injectors" as unbuilt). MangoHud (MIT) is a directly reusable reference.

---

## 5. Architecture

Two deliverables plus a shared static library.

```
   Discord desktop client (or Vesktop/Vencord/ArmCord) ── MUST be running
        │  local RPC  (unix socket $XDG_RUNTIME_DIR/discord-ipc-{0..9}
        │              or ws://127.0.0.1:6463–6472, Streamkit client_id)
        ▼
┌──────────────────────────────────────────────────────┐
│  HOST APP  `choir`  (Qt6 tray, 64-bit user process)   │  ← all networking, OAuth, secrets here
│  • RPC connector + OAuth (Streamkit flow)             │
│  • voice/notification STATE MODEL                     │
│  • avatar fetch + decode → disk cache (RGBA)          │
│  • settings UI + denylist editor + tray + autostart   │
│  • STATE SERVER (unix socket)                         │
└───────────────────┬──────────────────────────────────┘
                    │  local IPC: small JSON snapshots/deltas
                    │  + "avatar ready: <hash> <path> <w> <h>"
                    │  (avatar pixels via disk cache, never streamed into the game)
   ┌────────────────┼─────────────────────────┬─────────────────────────┐
   ▼                                           ▼                         ▼
INJECTED LAYER                            INJECTED LAYER            INJECTED LAYER   ← one per game
`libchoir_overlay.so` (Vulkan implicit layer, loaded into each Vulkan game)
   • state-server client (background thread, non-blocking, double-buffered snapshot)
   • hooks vkCreateInstance / vkCreateDevice / vkCreateSwapchainKHR / vkQueuePresentKHR
   • Dear ImGui Vulkan backend → draws voice panel + toasts into the swapchain image
   • CLICK-THROUGH, no input. Visible only while you are in a voice channel.
   • never crashes the game: every failure path disables the overlay; game renders normally
```

**Why the host/layer split:** OAuth, HTTPS, secrets, and avatar networking stay *out of the game
process*. The injected code is minimal, read-only, and stateless beyond a cached snapshot — far less
likely to destabilize a game, and a single host serves all running games.

### Components

- **`choir` host app** — Qt6 tray application. Owns the Discord RPC connection, OAuth, the state
  model, the avatar cache, and the state server. Provides settings + denylist editing + tray + the
  one-time OAuth consent.
- **`libchoir_overlay.so` + manifest** — the injected Vulkan implicit layer. Renders the overlay
  with ImGui; a read-only client of the host.
- **`libchoir_ipc` (shared static lib)** — local-IPC message types + framing + (de)serialization,
  linked by both host and layer. Single source of truth for the wire format.

---

## 6. Discord RPC integration

- **Transport:** the Discord desktop client exposes both a unix socket
  (`$XDG_RUNTIME_DIR/discord-ipc-{0..9}`, 8-byte little-endian `[opcode][len]` header + JSON) and a
  localhost WebSocket (`127.0.0.1:6463–6472`, requires an `Origin` header). v1 targets the **unix
  socket** primarily (native framing, no origin spoofing), probing indices 0–9. If M0 shows the
  Streamkit `AUTHORIZE`/token exchange requires the localhost `Origin` (as the web Streamkit flow
  does), the connector falls back to the **WebSocket transport** (`127.0.0.1:6463–6472`,
  `Origin: https://streamkit.discord.com`) for that handshake.
- **Opcodes:** `0 HANDSHAKE`, `1 FRAME`, `2 CLOSE`, `3 PING`, `4 PONG`. Handshake payload
  `{"v":1,"client_id":"207646673902501888"}` → `READY`.
- **Auth (Streamkit flow):** `AUTHORIZE` with scopes `[rpc, rpc.voice.read,
  rpc.notifications.read, messages.read]` → obtain an access token → `AUTHENTICATE`. The access
  token is cached (consent appears once). **The exact token-exchange step (Streamkit holds the
  client secret; Discover obtains a token via Streamkit's hosted endpoint) is verified in milestone
  M0 — this is the project's #1 risk.** Fallback: "own-app" mode where the user registers their own
  Discord application (they are the owner, so the scopes work without Discord approval).
- **Subscriptions / events:** `VOICE_CHANNEL_SELECT`, `VOICE_STATE_CREATE/UPDATE/DELETE` (channel-
  scoped), `SPEAKING_START` / `SPEAKING_STOP`, `NOTIFICATION_CREATE`. Voice-state objects carry
  `mute`, `deaf`, `self_mute`, `self_deaf`, `suppress`, plus user id/name/avatar.
- **Requirement:** the Discord desktop client (or a fork that bundles the RPC stack:
  Vesktop/Vencord/ArmCord) must be running. The browser client does **not** expose the socket.

---

## 7. Local IPC protocol (host ↔ layer)

- **Socket:** `$XDG_RUNTIME_DIR/choir.sock` (host listens; layers connect). Length-prefixed frames.
- **Control messages (small, JSON for debuggability):**
  - `Hello` (layer→host): protocol version, pid, game exe name.
  - `Snapshot` (host→layer): full current state on connect — visibility flag, voice channel name,
    ordered participant list (id, display name, avatar hash, `speaking`, `mute`, `deaf`,
    `self_mute`, `self_deaf`), active notification toasts, and the current appearance config.
  - `Delta` (host→layer): participant added/updated/removed, speaking changed, visibility changed,
    notification added/expired, config changed.
  - `AvatarReady` (host→layer): `{ hash, path, width, height, format=RGBA8 }`.
- **Avatars:** decoded RGBA written to `$XDG_CACHE_HOME/choir/avatars/<hash>.rgba`; the layer reads
  the file and uploads a texture once per hash. Pixels are **never streamed over the socket** and
  the cache is shared across game instances.
- **Robustness:** the layer's socket I/O is fully non-blocking on a background thread; the render
  thread reads the latest **double-buffered** snapshot lock-free. Host unavailable → empty snapshot
  → overlay draws nothing.

---

## 8. Vulkan layer

- **Manifest:** per-user `~/.local/share/vulkan/implicit_layer.d/choir_overlay.<arch>.json`
  (no root). Arch-suffixed layer name (`VK_LAYER_choir_overlay_x86_64`), **low `api_version`**
  (avoid the pre-1.3.228 loader suppression bug), `vkNegotiateLoaderLayerInterfaceVersion` pinned to
  v2, and a `disable_environment` key for `DISABLE_CHOIR_OVERLAY=1`.
- **Hook points:** `vkCreateInstance`, `vkCreateDevice` (chain wiring via
  `VkLayer*CreateInfo`), `vkCreateSwapchainKHR` (build/track per-swapchain ImGui render targets),
  `vkQueuePresentKHR` (the draw point). Handle swapchain recreation (resize) and multiple
  swapchains. `vkAcquireNextImageKHR` is **not** hooked.
- **Rendering:** Dear ImGui Vulkan backend; the overlay is composited into the acquired swapchain
  image just before present, in its own command buffer/render pass.
- **Safety:** every entry point wraps its work; on any internal error the layer disables itself for
  that device and forwards calls untouched so the game keeps rendering. No input is ever read.

---

## 9. UI / rendering

- **Voice panel:** a compact column of participants — circular avatar, display name, mute/deafen
  glyphs; **speaking** = bright highlight ring + slight brighten. Anchorable to any screen corner;
  configurable scale, opacity, and "show all members" vs "active speakers only". Styling is our own
  (Discord brand rules: no Clyde logo/wordmark recoloring or embedding).
- **Notification toasts:** transient corner pop-ups (DM/ping/mention), auto-expiring after a few
  seconds, stacked. Corner and duration configurable.
- All appearance settings live in the Qt window and reach the layer via the `config` field of
  `Snapshot`/`Delta`.

---

## 10. Activation, gating, visibility

- **Global with denylist:** the implicit layer is installed for all the user's Vulkan apps. A
  shipped **default denylist** (process basename / glob) excludes the Discord client itself,
  `steam`/`steamwebhelper`, `gamescope`, common browsers, `obs`, and known launchers; the user can
  edit it in the Qt UI. `DISABLE_CHOIR_OVERLAY=1` is a per-process override.
- **Safety by construction:** even if the layer loads into an unwanted app, it draws **nothing**
  unless (a) not denylisted, (b) the host is reachable, and (c) the user is currently in a voice
  channel. The desktop is never touched because there is no desktop surface.
- **Visibility = voice state:** the overlay appears on `VOICE_CHANNEL_SELECT` (join) and hides on
  leave. No hotkey in v1. A future global toggle could use the xdg-desktop-portal
  `GlobalShortcuts` interface (detection-only; KDE / GNOME ≥48 today) — Phase 2.

---

## 11. Error handling & known limitations

- Host/socket absent → layer draws nothing; game unaffected.
- Discord client absent → host retries connecting; overlay shows nothing.
- OAuth token expiry → host refreshes; on hard failure, prompts re-auth in the GUI.
- Swapchain recreation / multiple swapchains handled; **fullscreen-exclusive present** can freeze
  overlays (known MangoHud-class risk) — degrade gracefully.
- **Documented v1 limitations:** OpenGL / `PROTON_USE_WINED3D` games (Phase 2 GL shim); 32-bit
  games (Phase 2 multilib); **gamescope/Steam Deck** (separate path); draw order vs other co-
  resident implicit layers (MangoHud/OBS) is nondeterministic — acceptable for an info HUD.

---

## 12. Security / ToS / privacy

- Sanctioned RPC + OAuth path only. **Never** a user token or self-bot (account-ban risk).
- Networking and secrets confined to the **host** process; the injected layer does no network I/O.
- Message/voice data kept **ephemeral in memory**; the OAuth token is stored with `0600`
  permissions (or via libsecret). No scraping, no AI-training use, no off-platform export.
- **#1 risk:** reliance on the **Streamkit `client_id`** and its hosted token exchange — a gray-area
  dependency Discord could restrict. Mitigation: the "own-app" fallback; de-risked in M0.
- **Branding:** product/codename `choir` contains no "Discord"; "for Discord" is descriptive;
  not-affiliated disclaimer shipped.

---

## 13. Tech stack, build, packaging

- **C++17/20**, **Meson** build, **Qt6** (host GUI + networking; or libcurl for OAuth/avatars),
  vendored **Dear ImGui** (MIT), Vulkan loader + Vulkan-Headers. **MIT** license (notice
  preservation for any MangoHud-derived layer-setup code, which is Intel/Mesa MIT).
- **Install (per-user, no root):** layer `.so` to a user lib dir, manifest to
  `~/.local/share/vulkan/implicit_layer.d/`, host binary to `~/.local/bin`, optional XDG-autostart /
  systemd-user unit for the host.
- **v1 scope:** 64-bit, Vulkan only.

---

## 14. Testing strategy

- **Unit (TDD)** for all pure logic, with no Vulkan/Discord needed:
  - RPC parser + connection state machine, driven by a **mock IPC server replaying canned frames**.
  - `libchoir_ipc` (de)serialization round-trips.
  - Denylist matcher; config load/save.
  - State-model reducers (apply voice/speaking/notification events → expected snapshots).
- **Layer harness:** a **"fake host"** that feeds synthetic snapshots/deltas + avatars, so the layer
  can be loaded into `vkcube` / a minimal Vulkan present app and verified via **offscreen golden-
  image pixel checks** and **no-crash smoke tests** (incl. swapchain resize).
- **Integration (manual):** real Discord desktop client in a voice channel, against a real DXVK /
  native-Vulkan game.

---

## 15. Phasing / milestones

> Detailed, bite-sized tasks are produced by the writing-plans phase. High-level order:

- **M0 — Auth spike (de-risk FIRST):** prove the Streamkit RPC
  `AUTHORIZE → token → AUTHENTICATE → voice SUBSCRIBE` flow end-to-end against a real client. If it
  fails, switch the default to "own-app" mode. *Load-bearing.*
- **M1** — connector + state model (mock-tested).
- **M2** — `libchoir_ipc` + state server + fake-host harness.
- **M3** — Vulkan layer skeleton (loads, draws a test rectangle into a real Vulkan/DXVK game).
- **M4** — ImGui voice panel rendered from the fake host (golden-image tests).
- **M5** — wire host ↔ layer end-to-end with real Discord + avatars.
- **M6** — notification toasts.
- **M7** — Qt settings UI + denylist editor + tray + per-user install/packaging.
- **Phase 2** — OpenGL shim, 32-bit build, gamescope/`mangoapp` path, GlobalShortcuts toggle.

---

## 16. Open risks (carry into the plan)

1. **Streamkit auth path** — verify in M0; own-app fallback ready.
2. **Draw-order nondeterminism** vs other implicit layers — accept for an info HUD.
3. **Fullscreen-exclusive present** quirks — degrade gracefully, document.
4. **LD_PRELOAD/GL fragility** — deferred to Phase 2, but informs the GL design.

---

## 17. References (verified during research)

- Discord RPC / OAuth2 docs — https://docs.discord.com/developers/topics/rpc ,
  https://docs.discord.com/developers/topics/oauth2
- `rpc` scope is approved-partners-only; unapproved apps limited to owner + 50 testers
  (e.g. `discord/discord-rpc` issue #297 → `invalid_scope`).
- Streamkit `client_id 207646673902501888`; reuse pattern from **trigg/Discover**
  (`discover_overlay/discord_connector.py`).
- MangoHud (MIT) — Vulkan layer `src/vulkan.cpp` (hooks `vkQueuePresentKHR`),
  GL shim `src/gl/inject_glx.cpp` / `inject_egl.cpp`, manifest `src/mangohud.json.in`,
  Khronos `LoaderLayerInterface.md`. DXVK present (`dxvk_presenter.cpp`); VKD3D-Proton shares
  DXVK's present path.
- Dear ImGui (MIT); vkBasalt (zlib).
- Wayland input: no client-side keyboard grab by design
  (`xwayland-keyboard-grab-unstable-v1` is Xwayland-only); xdg-desktop-portal `GlobalShortcuts`
  (detection-only; KDE / GNOME ≥48).
- Discord Brand Guidelines (no "Discord" in product/app/domain names; "for Discord" descriptive ok).
