# Native Arch packaging — design

**Date:** 2026-06-20
**Status:** Approved (brainstorming) → ready for implementation plan

## Context

Choir currently installs per-user via `packaging/install-user.sh`, which runs `meson
install` (binary + layer `.so`) and then **hand-writes** the Vulkan implicit-layer manifest
with an absolute `library_path`. We want a proper distribution path.

**Why not Flatpak:** Choir's Vulkan implicit layer must load *inside each game's process*,
discovered by the Vulkan loader in the game's environment, with a host-ABI-compatible `.so`.
For **native games** (the target: native Steam at `~/.local/share/Steam` + pressure-vessel,
which imports layers from host Vulkan paths) the manifest + `.so` must live on host paths
and be linked against the host runtime — which a Flatpak sandbox cannot provide. The
sanctioned Flatpak layer mechanism (`org.freedesktop.Platform.VulkanLayer.*` extension) only
reaches Freedesktop-runtime consumers (Flatpak Steam), not native Steam. So for this
project's audience, **native packaging is the correct path**; we ship an Arch (pacman/AUR)
package. (Flatpak could be added later as a separate track for the Flatpak-Steam/Steam-Deck
audience: host as a Flatpak app + a `VulkanLayer.Choir` extension — explicitly out of scope.)

## Goal / success criteria

- `makepkg -si` from `packaging/PKGBUILD` builds Choir offline and installs it via pacman to
  `/usr`, with clean `pacman -R` removal.
- After install, the overlay is discoverable by native Vulkan games (`vulkaninfo | grep -i
  choir`) and `choir` launches from the app menu / `/usr/bin/choir`.
- `meson install --prefix=X` (any prefix) produces a *complete, correct* tree — so the
  pacman package and the per-user script share one install definition.
- The full test suite still passes.

Non-goals: Flatpak; AUR submission itself (needs a public repo — flagged as a follow-up);
release tagging / versioned tarballs (the package builds from the source tree / project
version for now).

## Approach

**Centralize all install placement in meson; the PKGBUILD just consumes `meson install`.**
Rejected alternative: have the PKGBUILD place the manifest/desktop itself (duplicates the
logic that the per-user script also needs — not DRY).

## Components

### 1. meson install completeness

**`src/layer/meson.build`** — add a second `configure_file` for the **install** manifest
(the existing one stays; it builds the relative-path manifest the tests load via
`VK_LAYER_PATH`):
- Input: the existing `manifest/choir_overlay.json.in` (`@ARCH@`, `@LIBPATH@`).
- `@LIBPATH@` = absolute installed path: `get_option('prefix') / get_option('libdir') /
  'choir' / 'libchoir_overlay.so'` (handles distros where `libdir` is `lib64`; on Arch it's
  `lib`).
- `install: true`, `install_dir: get_option('datadir') / 'vulkan' / 'implicit_layer.d'`,
  output `choir_overlay.@ARCH@.json` (i.e. `choir_overlay.x86_64.json`).
- The configured `library_path` reflects the final runtime prefix, not `DESTDIR` (meson
  substitutes `get_option('prefix')`), so it is correct under packaging (`/usr/...`) and the
  per-user prefix (`~/.local/...`) alike.

**top-level `meson.build`** — install the app-menu entry (project-level data; path is
simplest from the root):
- `install_data('packaging/choir.desktop', install_dir: get_option('datadir') /
  'applications')` with `Exec=choir` (resolved on `PATH`). Confirm/normalize the
  `choir.desktop` `Exec=` line to `choir` during implementation.

Net `DESTDIR=$pkgdir meson install --prefix=/usr` tree:
```
/usr/bin/choir
/usr/lib/choir/libchoir_overlay.so
/usr/share/vulkan/implicit_layer.d/choir_overlay.x86_64.json   (library_path → /usr/lib/...)
/usr/share/applications/choir.desktop
```

### 2. `packaging/PKGBUILD`

```
pkgname=choir
pkgver=0.0.1            # from project() version; -git variant uses git describe later
pkgrel=1
arch=('x86_64')
depends=('qt6-base' 'vulkan-icd-loader')
makedepends=('meson' 'vulkan-headers')
# build() : meson setup "$srcdir/build" "$startdir/.." --prefix=/usr --buildtype=release
#           meson compile -C "$srcdir/build"
# package(): DESTDIR="$pkgdir" meson install -C "$srcdir/build"
```
- Builds **offline**: imgui + nlohmann_json are vendored under `subprojects/` (no wrap
  download needed).
- `source=()` builds from the local tree now. Switching to a public git URL (for AUR) is a
  one-line change later (flagged below).
- Shaders are pre-compiled (`*.spv.h` checked in) — no glslang at build time.

### 3. Simplify `packaging/install-user.sh`

- Remove the hand-written manifest block (step 3). `meson install --prefix="$HOME/.local"`
  now emits `~/.local/share/vulkan/implicit_layer.d/choir_overlay.x86_64.json` with the
  correct absolute `~/.local/lib/choir/...` `library_path`.
- The script keeps: release configure + compile + install + the `--autostart` opt-in.
- `uninstall-user.sh` stays for the per-user path; pacman handles package removal.

## Data flow / interaction

`meson install` is the single source of truth for *where files go*. The PKGBUILD invokes it
with `--prefix=/usr` + `DESTDIR=$pkgdir`; `install-user.sh` invokes it with
`--prefix=~/.local`. The manifest's `library_path` is computed from the prefix at configure
time, so both produce a self-consistent install. The loader (in the game process) reads the
manifest from `<datadir>/vulkan/implicit_layer.d/` and loads the `.so` from the absolute
`library_path`.

## Testing / verification

- `meson test -C build` — full suite still green (the install changes don't touch runtime
  code; the build-tree relative manifest used by tests is unchanged).
- `meson install --destdir=/tmp/choir-pkg --prefix=/usr` into a staging dir → assert the four
  files exist at the expected paths and the manifest's `library_path` is
  `/usr/lib/choir/libchoir_overlay.so` (a small shell check).
- `makepkg -f` in `packaging/` builds a `.pkg.tar.zst`; `pacman -Qpl` lists the expected
  files. (Manual: `makepkg -si` then `vulkaninfo | grep -i choir` and launch a game.)
- Per-user path unchanged in behavior: `install-user.sh` still yields a working
  `vulkaninfo | grep -i choir`.

## Risks / notes

- **AUR publish prerequisite (flagged):** AUR PKGBUILDs source from a public URL; this repo
  is local-only. The PKGBUILD builds locally now; AUR submission is a follow-up once the repo
  is public (then a standard `choir-git` package + `.SRCINFO`).
- **libdir portability:** use `get_option('libdir')` (not a hardcoded `lib`) so the manifest
  path is right on lib64 distros.
- **desktop Exec:** ensure `choir.desktop` `Exec=choir` for the system entry (the per-user
  autostart path already templates an absolute Exec separately).
