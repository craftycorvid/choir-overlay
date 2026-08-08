# Choir

**An overlay for Discord that works on Linux.** Discord ships no in-game overlay for Linux
at all, so Choir draws one itself: while you're in a voice channel, your games show a panel
of who's in the call — avatars, who's talking, who's muted or deafened — plus toasts for
incoming Discord notifications.

It works in **Vulkan** games (native, and Windows games through Proton/DXVK/VKD3D) and in
**OpenGL** games. The overlay is display-only — you read it, you don't click it.

> **Not affiliated with, endorsed by, or sponsored by Discord Inc.** "Discord" is a
> trademark of Discord Inc.; Choir is an independent, unofficial overlay.

## Install

**Arch (AUR)** — [`choir-overlay-git`](https://aur.archlinux.org/packages/choir-overlay-git):

```sh
yay -S choir-overlay-git      # or: paru -S choir-overlay-git
```

**Any distro — AppImage.** Download `Choir-<version>-x86_64.AppImage` from the
[latest release](https://github.com/craftycorvid/choir-overlay/releases/latest):

```sh
chmod +x Choir-*-x86_64.AppImage
./Choir-*-x86_64.AppImage
```

The first launch asks permission to install Choir's overlay libraries to `~/.local`, and
you should say yes: games load the overlay from a fixed path on disk, which an AppImage
can't provide — its contents only exist while Choir is running. Decline and the tray and
settings still work, but no overlay appears in games; you can install them later from the
settings window. After that, a newer AppImage keeps them in sync on its own.

Requires glibc 2.39 or newer (Ubuntu 24.04+, Debian 13+, current Fedora, SteamOS).

**Any distro, from source** — installs under `$HOME`, no root:

```sh
bash packaging/install-user.sh
```

Make sure `~/.local/bin` is on your `PATH` (the installer warns you if it isn't). To
uninstall: `bash packaging/uninstall-user.sh` (add `--purge` to drop your settings too).

## Usage

1. Run `choir`. A tray icon appears and connects to your running Discord client.
2. On first run, **approve the Discord authorization prompt**. This happens once.
3. **Join a voice channel** and launch your game.

**Vulkan games: nothing to do.** The overlay is active automatically.

**OpenGL games: launch them with `choir-run`.** OpenGL gives us no way to hook in
automatically, so you opt in per game:

```sh
choir-run <game>       # directly
choir-run %command%    # Steam → game Properties → Launch Options
```

Open **Settings** from the tray to change where the panel sits, its size and brightness,
notification behaviour, whether Choir starts on login, and which programs to skip.

### Environment variables

| Variable                   | What it does                                              |
| -------------------------- | --------------------------------------------------------- |
| `DISABLE_CHOIR_OVERLAY=1`  | Turn the overlay off for one launch                        |
| `CHOIR_HDR_NITS=<80..1000>`| Overlay brightness on HDR displays (also in Settings)      |
| `CHOIR_FONT=<path.ttf>`    | Use a different overlay font                               |
| `CHOIR_GL_DEBUG=1`         | Log what the OpenGL overlay is doing, when it isn't showing |

In Steam, put these in **Launch Options**, e.g. `DISABLE_CHOIR_OVERLAY=1 %command%`.

## Build from source

```sh
meson setup build --buildtype=release
meson compile -C build
```

Dependencies: Qt6 (Core, Gui, Widgets, Network, Svg), the Vulkan loader, and libglvnd.
Dear ImGui and nlohmann/json are fetched automatically by Meson on first setup, so the
first build needs network access.

To build and run the tests (some render on a real GPU):

```sh
meson setup build -Dbuild_tests=true
meson test -C build
```

See [`CLAUDE.md`](CLAUDE.md) for the architecture and contributor notes.

## Limitations

- **Display-only.** The overlay is click-through: you can't click members or type in it.
- **64-bit only** (`x86_64`).
- **HDR in Vulkan only.** The OpenGL overlay is SDR.
- **Tested on Wayland only.** Nothing in Choir is Wayland-specific, but X11 is untested.
- **No LIVE or watching indicators.** Discord's local API doesn't report streaming or video
  state, so Choir can't show it.

## License

MIT — see [LICENSE](LICENSE).
