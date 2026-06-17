# Choir

Choir is an overlay for Discord — a Wayland-only, display-only voice panel and
notification overlay that renders *inside* Vulkan games (native, DXVK, VKD3D)
via an injected Vulkan implicit layer.

A Qt6 host (`choir`) owns the connection to the Discord desktop client's local
RPC, the state model, and the avatar cache, and serves read-only state to a
Vulkan layer (`libchoir_overlay.so`) injected into each game. The overlay is
display-only: click-through, read-only, and designed never to crash the game.

**Not affiliated with Discord Inc.** "Discord" is a trademark of Discord Inc.;
this project is an independent, unofficial overlay and is not endorsed by or
associated with Discord Inc.

## Building

Requires Meson, Ninja, a C++20 compiler, Qt6, and the Vulkan loader/headers.

```sh
meson setup build
meson compile -C build
```

To build and run the test suite:

```sh
meson setup build -Dbuild_tests=true
meson compile -C build
meson test -C build
```

## License

MIT — see [LICENSE](LICENSE).
