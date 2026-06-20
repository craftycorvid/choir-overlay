# Choir

Choir is **an overlay for Discord** — a Wayland-only, display-only voice panel and
notification overlay that renders *inside* Vulkan games (native, DXVK, VKD3D) via an
injected Vulkan implicit layer.

A Qt6 host (`choir`, a tray app) owns the connection to the Discord desktop client's
local RPC, the state model, and the avatar cache, and serves read-only state to a Vulkan
layer (`libchoir_overlay.so`) injected into each game. The overlay is display-only: it is
click-through and read-only, carries its own private copy of Dear ImGui (it exports only
the three Vulkan loader entrypoints, so it can't interfere with a game's own ImGui), and
is designed never to crash the game.

> **Not affiliated with, endorsed by, or sponsored by Discord Inc.** "Discord" is a
> trademark of Discord Inc.; Choir is an independent, unofficial overlay.

## Requirements

- A **Wayland** session.
- **Vulkan** (the loader + a working ICD). The overlay renders only in Vulkan games —
  native Vulkan or DXVK/VKD3D (Proton) titles. SDR and HDR (scRGB / HDR10) are both
  supported.
- The **Discord desktop client** running (the official client, or **Vesktop**/Vencord —
  anything that exposes the local `discord-ipc-*` socket).
- **Qt6** (Core, Gui, Widgets, Network) for the host.
- 64-bit (`x86_64`).

## Install

### Per-user (any distro, no root)

```sh
bash packaging/install-user.sh
```

Installs everything under `$HOME` and registers the Vulkan layer:

| Path | What |
| --- | --- |
| `~/.local/bin/choir` | the Qt host (tray app) |
| `~/.local/lib/choir/libchoir_overlay.so` | the injected Vulkan layer |
| `~/.local/share/vulkan/implicit_layer.d/choir_overlay.x86_64.json` | the implicit-layer manifest |

Make sure `~/.local/bin` is on your `PATH` (the installer warns you if it isn't).

**Autostart (opt-in):** the host does *not* start on login by default.

```sh
bash packaging/install-user.sh --autostart
```

### Arch (pacman)

```sh
cd packaging && makepkg -si
```

Builds and installs `choir` system-wide via pacman (`sudo pacman -R choir` removes it).

## Usage

Once installed, the overlay is active on **all** Vulkan games by default (minus the
denylist).

1. Run the host: `choir`. A tray icon appears, and it connects to your running Discord
   client.
2. On first run, **approve the one-time Discord authorization prompt** (the consent dialog
   from your Discord client). Use the tray's **Reconnect** if you ever need to
   re-establish the connection.
3. **Join a voice channel.** The voice panel (participants, active-speaker highlight,
   mute/deaf glyphs) appears in-game; Discord notifications show as toasts. Leaving the
   channel hides the overlay.

### Settings

Open the settings window from the tray to configure the panel **anchor**, **scale**,
**HDR brightness** (paper-white nits, for HDR displays), whether to show all members or
only active speakers, the toast anchor/duration, and the **denylist**.

### Disabling the overlay

- **Per game (one launch):** set `DISABLE_CHOIR_OVERLAY=1`. For Steam, put it in the
  game's **Launch Options**:

  ```
  DISABLE_CHOIR_OVERLAY=1 %command%
  ```

- **Denylist:** processes matching the denylist never get the overlay. Defaults include
  the Discord client, Steam (`steamwebhelper`), `gamescope`, OBS, and browsers. Edit it
  (case-insensitive globs matched against the process name) in the settings window.

### Verify the layer is visible

```sh
vulkaninfo | grep -i choir
# -> VK_LAYER_choir_overlay_x86_64 ... (implicit layer)
```

## Build from source

```sh
meson setup build
meson compile -C build
```

With the test suite:

```sh
meson setup build -Dbuild_tests=true
meson test -C build
```

Dear ImGui and nlohmann/json are vendored under `subprojects/`, so the build runs offline.
See [`CLAUDE.md`](CLAUDE.md) for the architecture overview and contributor notes.

## Limitations

- **Vulkan only** — no OpenGL games.
- **Wayland only**, **64-bit only** (`x86_64`).
- **Display-only** — the overlay is click-through and non-interactive: you read it, you
  don't click it.

## Uninstall

Per-user install:

```sh
bash packaging/uninstall-user.sh
```

Removes the manifest, the `.so`, the `choir` binary, and the autostart entry (all under
`$HOME`). Add `--purge` to also remove your config and avatar cache (`~/.config/choir`,
`~/.cache/choir`). For the pacman package: `sudo pacman -R choir`.

## License

MIT — see [LICENSE](LICENSE).
