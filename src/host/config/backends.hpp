#pragma once
// AppImage backend install: getting libchoir_overlay.so + libchoir_gl.so out of the
// image and onto the real filesystem.
//
// An AppImage is a squashfs mounted at a random /tmp/.mount_XXXXXX only while the host
// process is alive, but BOTH injected backends have to be reachable from a different
// process at a later time: the Vulkan loader reads library_path out of the implicit-layer
// manifest inside each game, and choir-run LD_PRELOADs a path into a game. So the payload
// the AppImage carries at $APPDIR/usr/lib/choir is copied to ~/.local, and the manifest
// and choir-run are GENERATED here with absolute paths rather than copied out of the
// image — same reasoning as autostart.cpp, whose installed template's location likewise
// varies by prefix.
//
// Everything here is inert for source/pacman installs: appimage_dir() is empty when
// $APPDIR is unset, and every caller checks it first. Those installs get the equivalent
// files from `meson install`.
//
// Qt-free; lives in libchoir_host_core.

#include <string>

namespace choir {

// $APPDIR (set by the AppImage runtime), or "" when not running from an AppImage.
std::string appimage_dir();

// $APPDIR/usr/lib/choir — the payload to install from, or "" when not running from an
// AppImage. Callers treat empty as "nothing to do", which is what makes all of this
// inert for source/pacman installs.
std::string appimage_payload_dir();

// $HOME/.local/lib/choir — where the two injected .so files are installed. Matches
// packaging/install-user.sh, so an AppImage and a per-user source install agree.
std::string backend_lib_dir();

// $HOME/.local/bin/choir-run — the generated LD_PRELOAD wrapper for OpenGL games.
std::string gl_wrapper_path();

// True when both .so files in `dst_dir` are byte-identical to those in `src_dir`.
// False if either is missing on either side, so a fresh or half-finished install
// reads as out of date.
bool backends_up_to_date(const std::string& src_dir, const std::string& dst_dir);

// Install both .so files from `src_dir` (an AppImage's usr/lib/choir), then write the
// implicit-layer manifest and choir-run pointing at them. Returns false on any I/O
// failure, having written no manifest — a manifest naming an absent library is an
// error the Vulkan loader logs in every application on the system.
//
// Safe to re-run: files are replaced by rename, so an update while a game still has
// the old library mapped succeeds instead of failing with ETXTBSY.
bool install_backends(const std::string& src_dir);

// Remove everything install_backends() wrote, plus ~/.local/lib/choir if that leaves it
// empty. Idempotent, and true when there was nothing to remove.
//
// This exists because deleting an AppImage cannot clean up after itself. The manifest
// registers a GLOBAL implicit layer, so without this it keeps being dlopened into every
// Vulkan application on the system forever, with no owner and no way to trace it back.
// Removal order mirrors install: the manifest goes FIRST.
bool uninstall_backends();

}  // namespace choir
