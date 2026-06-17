#!/usr/bin/env bash
#
# Choir — per-user install (no root). Installs entirely under the user's $HOME:
#
#   ~/.local/bin/choir                                            the Qt host (tray)
#   ~/.local/lib/choir/libchoir_overlay.so                        the injected Vulkan layer
#   ~/.local/share/vulkan/implicit_layer.d/choir_overlay.x86_64.json
#                                                                 the implicit-layer manifest
#                                                                 (ABSOLUTE library_path)
#   ~/.config/autostart/choir.desktop  (only with --autostart)    launch choir on login
#
# Idempotent: re-running reconfigures + reinstalls cleanly. Touches nothing outside $HOME.
#
# Usage:
#   bash packaging/install-user.sh [--autostart] [--build-dir DIR]
#
#   --autostart      also install the XDG autostart entry ENABLED, so `choir` starts on login.
#   --build-dir DIR  use/create this release build dir (default: build-release).
#
set -euo pipefail

# ---- locate the project root (this script lives in <root>/packaging/) ----------------
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

# ---- args -----------------------------------------------------------------------------
AUTOSTART=0
BUILD_DIR="${PROJECT_ROOT}/build-release"
while [ "$#" -gt 0 ]; do
  case "$1" in
    --autostart) AUTOSTART=1; shift ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    -h|--help) sed -n '2,22p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "install-user.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done

# ---- per-user locations (XDG, honouring overrides) ------------------------------------
PREFIX="${HOME}/.local"
ARCH="x86_64"
XDG_DATA_HOME_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}"
XDG_CONFIG_HOME_DIR="${XDG_CONFIG_HOME:-${HOME}/.config}"
MANIFEST_DIR="${XDG_DATA_HOME_DIR}/vulkan/implicit_layer.d"
MANIFEST_PATH="${MANIFEST_DIR}/choir_overlay.${ARCH}.json"
AUTOSTART_DIR="${XDG_CONFIG_HOME_DIR}/autostart"

INSTALLED_SO="${PREFIX}/lib/choir/libchoir_overlay.so"
INSTALLED_BIN="${PREFIX}/bin/choir"

echo "Choir per-user install"
echo "  project    : ${PROJECT_ROOT}"
echo "  build dir  : ${BUILD_DIR}"
echo "  prefix     : ${PREFIX}"
echo "  manifest   : ${MANIFEST_PATH}"
echo

# ---- 1. configure a release build (idempotent: setup if new, else reconfigure) --------
if [ -d "${BUILD_DIR}" ]; then
  echo ">> reconfiguring existing build dir ${BUILD_DIR}"
  meson setup --reconfigure "${BUILD_DIR}" "${PROJECT_ROOT}" \
    --buildtype=release --prefix="${PREFIX}"
else
  echo ">> setting up release build dir ${BUILD_DIR}"
  meson setup "${BUILD_DIR}" "${PROJECT_ROOT}" \
    --buildtype=release --prefix="${PREFIX}"
fi
# imgui MUST be static in the layer (no separate libimgui.so to interpose); the
# top-level meson.build asks for it, but pin it here too so a pre-existing dir obeys.
meson configure "${BUILD_DIR}" -Dimgui:default_library=static >/dev/null

# ---- 2. compile + install into ~/.local -----------------------------------------------
echo ">> compiling"
meson compile -C "${BUILD_DIR}"
echo ">> installing into ${PREFIX}"
meson install -C "${BUILD_DIR}"

# ---- 3. write the implicit-layer manifest with an ABSOLUTE library_path ---------------
# The build-tree manifest uses a relative path; the loader resolves an installed
# implicit-layer manifest from a fixed dir, so it needs the absolute installed .so.
if [ ! -f "${INSTALLED_SO}" ]; then
  echo "install-user.sh: expected installed layer at ${INSTALLED_SO} but it is missing" >&2
  exit 1
fi
mkdir -p "${MANIFEST_DIR}"
cat > "${MANIFEST_PATH}" <<EOF
{ "file_format_version": "1.2.0",
  "layer": { "name": "VK_LAYER_choir_overlay_${ARCH}",
    "type": "GLOBAL", "library_path": "${INSTALLED_SO}",
    "api_version": "1.1.0", "implementation_version": "1",
    "description": "Choir — an overlay for Discord (not affiliated with Discord Inc.)",
    "disable_environment": { "DISABLE_CHOIR_OVERLAY": "1" } } }
EOF
echo ">> wrote manifest ${MANIFEST_PATH}"

# ---- 4. autostart entry (opt-in) -------------------------------------------------------
AUTOSTART_PATH="${AUTOSTART_DIR}/choir.desktop"
if [ "${AUTOSTART}" -eq 1 ]; then
  mkdir -p "${AUTOSTART_DIR}"
  # Template the .desktop with the absolute Exec path + autostart ENABLED.
  sed -e "s|^Exec=.*|Exec=${INSTALLED_BIN}|" \
      -e "s|^X-GNOME-Autostart-enabled=.*|X-GNOME-Autostart-enabled=true|" \
      "${SCRIPT_DIR}/choir.desktop" > "${AUTOSTART_PATH}"
  echo ">> installed autostart entry (ENABLED): ${AUTOSTART_PATH}"
fi

# ---- 5. next steps ---------------------------------------------------------------------
echo
echo "Choir installed."
echo
echo "  layer  : ${INSTALLED_SO}"
echo "  host   : ${INSTALLED_BIN}"
echo "  manifest: ${MANIFEST_PATH}"
echo
if ! printf '%s' ":${PATH}:" | grep -q ":${PREFIX}/bin:"; then
  echo "  NOTE: ${PREFIX}/bin is not on your PATH. Add it, e.g.:"
  echo "        export PATH=\"${PREFIX}/bin:\$PATH\""
  echo
fi
echo "Next steps:"
echo "  1. Run the host:           choir   (or ${INSTALLED_BIN})"
echo "     - authorize Discord from the tray, then join a voice channel."
echo "  2. The overlay is now active on ALL Vulkan games by default."
echo "     Disable for one launch:  DISABLE_CHOIR_OVERLAY=1 %command%   (Steam launch options)"
echo "     Per-game denylist:       edit it in the Choir settings window."
echo "  3. Verify the layer is visible to Vulkan:"
echo "        vulkaninfo | grep -i choir"
if [ "${AUTOSTART}" -eq 0 ]; then
  echo "  4. (Optional) start choir on login:"
  echo "        bash packaging/install-user.sh --autostart"
  echo "     or copy packaging/choir.desktop to ${AUTOSTART_DIR}/ and set X-GNOME-Autostart-enabled=true"
fi
echo
echo "Uninstall: bash packaging/uninstall-user.sh"
