# Native Arch Packaging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship Choir as a native Arch (pacman/AUR) package by making `meson install` place every file correctly for any prefix, then adding a `PKGBUILD` and simplifying the per-user script to consume it.

**Architecture:** `meson install` becomes the single source of truth for install placement — it now also emits the install-time Vulkan layer manifest (absolute `library_path` derived from the prefix) and the desktop entry. The `PKGBUILD` (`--prefix=/usr`) and `install-user.sh` (`--prefix=~/.local`) both just run `meson install`.

**Tech Stack:** Meson, Vulkan implicit-layer manifest, Arch `makepkg`/PKGBUILD, bash.

**Spec:** `docs/superpowers/specs/2026-06-20-native-arch-packaging-design.md`

---

### Task 1: meson installs the layer manifest + desktop entry

**Goal:** `meson install --prefix=X` produces a complete tree — host binary, layer `.so`, the install-time layer manifest (absolute `library_path` = `X/lib*/choir/libchoir_overlay.so`), and the desktop entry — for any prefix.

**Files:**
- Modify: `meson.build` (top-level — add the install-manifest `configure_file` + desktop `install_data`)

**Acceptance Criteria:**
- [ ] `meson install --prefix=/usr --destdir=DIR` writes `DIR/usr/bin/choir`, `DIR/usr/lib/choir/libchoir_overlay.so`, `DIR/usr/share/vulkan/implicit_layer.d/choir_overlay.x86_64.json`, `DIR/usr/share/applications/choir.desktop`.
- [ ] That manifest's `library_path` is exactly `/usr/lib/choir/libchoir_overlay.so`.
- [ ] The install-manifest is generated in `build/` (top), NOT `build/src/layer/`, so tests (which scan `build/src/layer/` via `VK_LAYER_PATH`) don't pick it up.
- [ ] `meson test -C build` still passes (22/22).

**Verify:**
```
rm -rf /tmp/choir-stage; meson setup --reconfigure build . --prefix=/usr >/dev/null && meson install -C build --destdir=/tmp/choir-stage >/dev/null && \
test -f /tmp/choir-stage/usr/bin/choir && \
test -f /tmp/choir-stage/usr/lib/choir/libchoir_overlay.so && \
grep -q '"/usr/lib/choir/libchoir_overlay.so"' /tmp/choir-stage/usr/share/vulkan/implicit_layer.d/choir_overlay.x86_64.json && \
test -f /tmp/choir-stage/usr/share/applications/choir.desktop && echo STAGE_OK
```
→ prints `STAGE_OK`. Then `meson test -C build` → `Fail: 0`.

**Steps:**

- [ ] **Step 1: Add the install-manifest + desktop install to the TOP-LEVEL `meson.build`.** Insert just before the `if get_option('build_tests')` block at the end of `meson.build`:
```meson
# --- Install-time artifacts (packaging). The layer .so + host binary are installed by
# their own meson.build (install: true). Here we add the install-time implicit-layer
# manifest (ABSOLUTE library_path, derived from the prefix) and the desktop entry, so
# `meson install --prefix=X` is a complete, correct install for any X (e.g. /usr for the
# pacman package, ~/.local for install-user.sh). This manifest is generated in the TOP
# build dir (build/), NOT build/src/layer/, so the tests' VK_LAYER_PATH=build/src/layer
# only ever sees the relative build-tree manifest.
configure_file(
  input: 'src/layer/manifest/choir_overlay.json.in',
  output: 'choir_overlay.x86_64.json',
  configuration: {
    'ARCH': 'x86_64',
    'LIBPATH': get_option('prefix') / get_option('libdir') / 'choir' / 'libchoir_overlay.so',
  },
  install: true,
  install_dir: get_option('datadir') / 'vulkan' / 'implicit_layer.d',
)

# App-menu entry. Exec=choir (resolved on PATH). The XDG-autostart key in the file is
# ignored for menu entries; the per-user autostart path templates an absolute Exec
# separately in install-user.sh.
install_data('packaging/choir.desktop',
  install_dir: get_option('datadir') / 'applications')
```
  Note: `choir.desktop` already has `Exec=choir` and `Icon=choir` — no change needed there. (`Icon=choir` with no installed icon simply falls back to a generic icon; an icon asset is out of scope.)

- [ ] **Step 2: Run the staging install verify** (the Verify block above) → `STAGE_OK`.

- [ ] **Step 3: Run the full suite** to confirm the test-dir manifest is untouched:
  Run: `meson test -C build` → Expected: `Ok: 22  Fail: 0`. (If `build` was reconfigured to `--prefix=/usr` in the verify, that's fine — prefix doesn't affect tests.)

- [ ] **Step 4: Commit.**
```bash
git add meson.build
git commit -m "build(install): meson emits the install-time layer manifest + desktop entry"
```

---

### Task 2: simplify `install-user.sh` to consume meson's manifest

**Goal:** The per-user installer drops its hand-written manifest block and relies on `meson install` (now emitting the manifest with the `~/.local`-prefixed `library_path`).

**Files:**
- Modify: `packaging/install-user.sh`

**Acceptance Criteria:**
- [ ] The `cat > "${MANIFEST_PATH}" <<EOF ... EOF` block (step 3) is removed.
- [ ] After running the script, `~/.local/share/vulkan/implicit_layer.d/choir_overlay.x86_64.json` exists with `library_path` = `<HOME>/.local/lib/choir/libchoir_overlay.so`.
- [ ] `vulkaninfo | grep -i choir` shows the layer.

**Verify:**
```
bash packaging/install-user.sh >/dev/null 2>&1 && \
grep -q "$HOME/.local/lib/choir/libchoir_overlay.so" "$HOME/.local/share/vulkan/implicit_layer.d/choir_overlay.x86_64.json" && \
vulkaninfo 2>/dev/null | grep -qi choir && echo USER_OK
```
→ prints `USER_OK`.

**Steps:**

- [ ] **Step 1: Delete the manual manifest block.** In `packaging/install-user.sh`, remove the entire section that starts with the comment `# ---- 3. write the implicit-layer manifest ...` and ends after `echo ">> wrote manifest ${MANIFEST_PATH}"` (the `if [ ! -f "${INSTALLED_SO}" ]` check, the `mkdir -p "${MANIFEST_DIR}"`, the `cat > "${MANIFEST_PATH}" <<EOF ... EOF`, and the echo). `meson install` (step 2 in the script) now writes the manifest because the build dir is configured with `--prefix="${PREFIX}"` (= `~/.local`), so the configured `library_path` is `~/.local/lib/choir/libchoir_overlay.so`.
- [ ] **Step 2: Keep the `MANIFEST_PATH` variable + the final "manifest: ${MANIFEST_PATH}" echo** (it still points at the now-meson-written file, used by the summary output). Leave steps 1, 2, 4, 5 of the script as-is.
- [ ] **Step 3: Run the verify** (the Verify block above) → `USER_OK`.
- [ ] **Step 4: Commit.**
```bash
git add packaging/install-user.sh
git commit -m "build(install): install-user.sh uses the meson-emitted layer manifest"
```

---

### Task 3: `packaging/PKGBUILD` (local-buildable pacman package)

**Goal:** A `PKGBUILD` that builds Choir offline from the local tree and packages it for pacman; `makepkg` produces a `.pkg.tar.zst` whose contents are the expected `/usr` tree.

**Files:**
- Create: `packaging/PKGBUILD`

**Acceptance Criteria:**
- [ ] `makepkg -f` (run in `packaging/`) builds a `choir-*.pkg.tar.zst` with exit 0.
- [ ] `pacman -Qpl <pkg>` lists `/usr/bin/choir`, `/usr/lib/choir/libchoir_overlay.so`, `/usr/share/vulkan/implicit_layer.d/choir_overlay.x86_64.json`, `/usr/share/applications/choir.desktop`.
- [ ] No file is installed to the live system (build + inspect only).

**Verify:**
```
cd packaging && makepkg -f && \
pkg=$(ls -t choir-*.pkg.tar.zst | head -1) && \
pacman -Qpl "$pkg" | grep -q 'usr/lib/choir/libchoir_overlay.so' && \
pacman -Qpl "$pkg" | grep -q 'usr/share/vulkan/implicit_layer.d/choir_overlay.x86_64.json' && \
pacman -Qpl "$pkg" | grep -q 'usr/bin/choir' && echo PKG_OK
```
→ prints `PKG_OK`.

**Steps:**

- [ ] **Step 1: Create `packaging/PKGBUILD`:**
```bash
# Maintainer: corvid <claude@corvid.boo>
pkgname=choir
pkgver=0.0.1
pkgrel=1
pkgdesc="Wayland Discord voice overlay for Vulkan games (not affiliated with Discord Inc.)"
arch=('x86_64')
url="https://github.com/craftycorvid/choir"
license=('MIT')
depends=('qt6-base' 'vulkan-icd-loader')
makedepends=('meson' 'vulkan-headers')
# Local build: sources the working tree this PKGBUILD lives in. For AUR, replace with a
# git source, e.g. source=("git+https://github.com/craftycorvid/choir.git") + a pkgver()
# (and drop the _srcroot indirection below). imgui + nlohmann_json are vendored under
# subprojects/, so the build runs offline.
_srcroot="${startdir}/.."

build() {
  meson setup "${srcdir}/build" "${_srcroot}" \
    --prefix=/usr --buildtype=release -Dbuild_tests=false
  meson compile -C "${srcdir}/build"
}

package() {
  DESTDIR="${pkgdir}" meson install -C "${srcdir}/build"
}
```
  - `-Dbuild_tests=false`: the package doesn't need the test binaries.
  - `vulkan-icd-loader` is the runtime dep (provides the loader that discovers + loads the layer); `qt6-base` covers the host's Core/Gui/Widgets/Network.

- [ ] **Step 2: Build + inspect** (the Verify block) → `PKG_OK`. If `makepkg` complains about a missing license file, that's a lint warning, not an error — the build still succeeds; ignore for the local package.

- [ ] **Step 3: Commit.**
```bash
git add packaging/PKGBUILD
git commit -m "packaging: add PKGBUILD for a native Arch (pacman) package"
```

---

### Task 4 (optional): adopt + smoke-test the package via pacman

**Goal:** Install the built package system-wide and confirm the overlay is discoverable, as the user's adoption check.

**USER-ORDERED GATE — NON-SKIPPABLE.** This task was requested by the user in the current conversation. It MUST NOT be closed by walking around it, by declaring it "verified inline", or by substituting a cheaper check. Close only after every item in `acceptanceCriteria` has been re-validated independently, with output captured.

**Files:** none (install + manual test)

**Acceptance Criteria:**
- [ ] `sudo pacman -U <pkg>` installs cleanly.
- [ ] `vulkaninfo | grep -i choir` shows the layer (from `/usr/share/vulkan/...`).
- [ ] `/usr/bin/choir` launches; a Vulkan game still shows the overlay.

**Verify:** user runs `cd packaging && makepkg -si`, then `vulkaninfo | grep -i choir`, then launches a game.

**Steps:**
- [ ] **Step 1 (optional):** to avoid two installs of the same layer, first remove the per-user one: `bash packaging/uninstall-user.sh` (the loader dedups implicit layers by name, so coexistence wouldn't double-render, but a single install is cleaner).
- [ ] **Step 2:** `cd packaging && makepkg -si` → installs `choir` via pacman.
- [ ] **Step 3:** `vulkaninfo | grep -i choir` → shows the layer; launch a game to confirm the overlay renders; `pacman -R choir` removes it cleanly if reverting.

---

## Self-Review

- **Spec coverage:** meson install completeness incl. install manifest + desktop (T1), PKGBUILD (T3), install-user.sh simplification (T2), verification via staging/makepkg (T1/T3 Verify), optional adoption smoke-test (T4). All spec sections mapped.
- **Placeholder scan:** none — every step has concrete code/commands. The `url`/license in the PKGBUILD use the known repo handle (`craftycorvid`); adjust if the public repo differs.
- **Consistency:** the manifest output name `choir_overlay.x86_64.json`, install dirs (`datadir/vulkan/implicit_layer.d`, `datadir/applications`, `libdir/choir`), and the `library_path` `/usr/lib/choir/libchoir_overlay.so` are identical across T1, T2, T3.
