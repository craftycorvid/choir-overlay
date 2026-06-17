# Choir

Choir is **an overlay for Discord** — a Wayland-only, display-only voice panel and
notification overlay that renders *inside* Vulkan games (native, DXVK, VKD3D) via
an injected Vulkan implicit layer.

A Qt6 host (`choir`, a tray app) owns the connection to the Discord desktop
client's local RPC, the state model, and the avatar cache, and serves read-only
state to a Vulkan layer (`libchoir_overlay.so`) injected into each game. The
overlay is display-only: it is click-through and read-only, carries its own
private copy of Dear ImGui (it exports only the three Vulkan loader entrypoints,
so it can't interfere with a game's own ImGui), and is designed never to crash
the game.

> **Not affiliated with, endorsed by, or sponsored by Discord Inc.** "Discord" is
> a trademark of Discord Inc.; Choir is an independent, unofficial overlay.

## Requirements

- **Wayland** session (Choir is Wayland-only for v1).
- **Vulkan** (the Vulkan loader + a working ICD; the overlay only renders in
  Vulkan games — native Vulkan or DXVK/VKD3D-Proton titles).
- The **Discord desktop client** running (the official client, or
  **Vesktop**/Vencord — anything that exposes the local `discord-ipc-*` socket).
- **Qt6** (Core, Gui, Widgets, Network) for the host.
- 64-bit. C++20 compiler, Meson, Ninja to build.

## Build

```sh
meson setup build
meson compile -C build
```

With the test suite:

```sh
meson setup build -Dbuild_tests=true
meson compile -C build
meson test -C build
```

## Install (per-user, no root)

```sh
bash packaging/install-user.sh
```

This configures a release build, installs everything under your `$HOME` (nothing
needs root), and registers the Vulkan layer:

| Path | What |
| --- | --- |
| `~/.local/bin/choir` | the Qt host (tray app) |
| `~/.local/lib/choir/libchoir_overlay.so` | the injected Vulkan layer |
| `~/.local/share/vulkan/implicit_layer.d/choir_overlay.x86_64.json` | the implicit-layer manifest (absolute `library_path`) |

Make sure `~/.local/bin` is on your `PATH` (the installer warns you if it isn't).

**Autostart (opt-in):** the host does *not* start on login by default. To enable
it:

```sh
bash packaging/install-user.sh --autostart   # installs ~/.config/autostart/choir.desktop (enabled)
```

(Or copy `packaging/choir.desktop` into `~/.config/autostart/` yourself and set
`X-GNOME-Autostart-enabled=true`.)

## Enable / usage

The overlay uses a **global-with-denylist** model: once installed, it is active
on **all** Vulkan games by default (minus the denylist below).

1. Run the host: `choir`. A tray icon appears.
2. From the tray, **authorize Discord** — a one-time Discord consent dialog
   appears.
3. **Join a voice channel.** The voice panel (participants, active-speaker
   highlight, mute/deaf glyphs) appears in-game; Discord notifications show as
   toasts. Leaving the channel hides the overlay.

### Disabling the overlay

- **Per game (one launch):** set `DISABLE_CHOIR_OVERLAY=1`. For Steam, put this
  in the game's **Launch Options**:

  ```
  DISABLE_CHOIR_OVERLAY=1 %command%
  ```

- **Denylist:** processes matching the denylist never get the overlay. Defaults
  include the Discord client, Steam (`steamwebhelper`), `gamescope`, OBS, and
  browsers. Edit the denylist (glob patterns, case-insensitive, matched against
  the process name) in the **Choir settings window**.

### Verify the layer is visible

```sh
vulkaninfo | grep -i choir
# -> VK_LAYER_choir_overlay_x86_64 ... (implicit layer)
```

## Discord auth: the Streamkit caveat

By default Choir authenticates the local Discord RPC using **Discord's Streamkit
`client_id`** — the same client id the official "Discover"/Streamkit browser
overlay uses. This is the pragmatic path (no app registration needed), but it is
a **gray-area dependency**: it relies on a Discord-owned client id and Discord
could restrict it at any time.

The fully-sanctioned alternative is **own-app mode**: register your own
application in the [Discord Developer Portal](https://discord.com/developers/applications),
then configure Choir to use it (in `~/.config/choir/config.json`, or the settings
window):

```json
{
  "auth_mode": "own-app",
  "client_id": "<your application's client id>",
  "client_secret": "<your application's client secret>"
}
```

In own-app mode Choir exchanges the authorization code against Discord's standard
`https://discord.com/api/oauth2/token` endpoint instead of the Streamkit endpoint.

### M0 auth status

The auth flow is validated by the spike binary `./build/tests/auth_spike` (built
with `-Dbuild_tests=true`). Run it **once** with the Discord client running and
yourself in a voice channel — it completes the AUTHORIZE → AUTHENTICATE handshake,
prints live participants + `SPEAKING_START/STOP`, and **confirms (or corrects)
the Streamkit token URL** in one place. If Streamkit ever stops working, switch to
own-app mode as above.

## v1 limitations

- **Vulkan only** — no OpenGL games yet (an OpenGL shim is future work).
- **64-bit only** (`x86_64`).
- **No gamescope / Steam Deck path** (gamescope is denylisted by default).
- **Display-only** — the overlay is click-through and non-interactive; you read
  it, you don't click it.
- **Wayland only.**

## Uninstall

```sh
bash packaging/uninstall-user.sh
```

Removes the manifest, the installed `.so`, the `choir` binary, and the autostart
entry (all under `$HOME`). Add `--purge` to also remove your config and avatar
cache (`~/.config/choir`, `~/.cache/choir`).

## License

MIT — see [LICENSE](LICENSE).
