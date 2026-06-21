#!/usr/bin/env bash
#
# Choir — per-user uninstall. Removes everything install-user.sh placed under $HOME:
#   - the implicit-layer manifest
#   - the installed layer .so + the GL overlay .so (and ~/.local/lib/choir if now empty)
#   - the choir binary + the choir-run GL launcher
#   - the autostart entry
# Leaves user config/cache (~/.config/choir, ~/.cache/choir) untouched by default;
# pass --purge to remove those too. Touches nothing outside $HOME.
#
# Usage: bash packaging/uninstall-user.sh [--purge]
#
set -euo pipefail

PURGE=0
case "${1:-}" in
  --purge) PURGE=1 ;;
  -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}"; exit 0 ;;
  "") ;;
  *) echo "uninstall-user.sh: unknown argument: $1" >&2; exit 2 ;;
esac

PREFIX="${HOME}/.local"
ARCH="x86_64"
XDG_DATA_HOME_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}"
XDG_CONFIG_HOME_DIR="${XDG_CONFIG_HOME:-${HOME}/.config}"
XDG_CACHE_HOME_DIR="${XDG_CACHE_HOME:-${HOME}/.cache}"

MANIFEST_PATH="${XDG_DATA_HOME_DIR}/vulkan/implicit_layer.d/choir_overlay.${ARCH}.json"
INSTALLED_SO="${PREFIX}/lib/choir/libchoir_overlay.so"
INSTALLED_GL_SO="${PREFIX}/lib/choir/libchoir_gl.so"
INSTALLED_BIN="${PREFIX}/bin/choir"
INSTALLED_CHOIR_RUN="${PREFIX}/bin/choir-run"
AUTOSTART_PATH="${XDG_CONFIG_HOME_DIR}/autostart/choir.desktop"
# meson installs the static ImGui dep alongside (it is linked privately into the
# layer .so, never dlopen'd); clean up the orphan so uninstall leaves no trace.
ORPHAN_IMGUI="${PREFIX}/lib/libimgui.a"

rm_if() {  # rm_if <path> <label>
  if [ -e "$1" ]; then rm -f "$1" && echo ">> removed $2: $1"; else echo ">> (absent) $2: $1"; fi
}

echo "Choir per-user uninstall"
echo
rm_if "${MANIFEST_PATH}" "manifest"
rm_if "${INSTALLED_SO}"  "layer .so"
rm_if "${INSTALLED_GL_SO}" "gl overlay .so"
rm_if "${INSTALLED_BIN}" "host binary"
rm_if "${INSTALLED_CHOIR_RUN}" "gl launcher"
rm_if "${AUTOSTART_PATH}" "autostart entry"
rm_if "${ORPHAN_IMGUI}" "bundled static imgui"

# Remove the now-empty ~/.local/lib/choir dir.
if [ -d "${PREFIX}/lib/choir" ] && [ -z "$(ls -A "${PREFIX}/lib/choir")" ]; then
  rmdir "${PREFIX}/lib/choir" && echo ">> removed empty dir: ${PREFIX}/lib/choir"
fi

if [ "${PURGE}" -eq 1 ]; then
  rm_if "${XDG_CONFIG_HOME_DIR}/choir/config.json" "config"
  [ -d "${XDG_CONFIG_HOME_DIR}/choir" ] && rmdir --ignore-fail-on-non-empty "${XDG_CONFIG_HOME_DIR}/choir" || true
  if [ -d "${XDG_CACHE_HOME_DIR}/choir" ]; then
    rm -rf "${XDG_CACHE_HOME_DIR}/choir" && echo ">> removed cache: ${XDG_CACHE_HOME_DIR}/choir"
  fi
fi

echo
echo "Choir uninstalled."
if [ "${PURGE}" -eq 0 ]; then
  echo "(User config/cache kept; re-run with --purge to remove ~/.config/choir + ~/.cache/choir.)"
fi
