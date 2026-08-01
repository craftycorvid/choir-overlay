#pragma once
// XDG autostart toggle: the ~/.config/autostart/choir.desktop entry that starts the
// host on login (the same one `install-user.sh --autostart` writes).
//
// The file's presence IS the state — there is no autostart field in config.json, so
// the checkbox can't drift from what the desktop actually does if the user (or the
// install script, or their DE's own startup-apps UI) changes it behind our back.
//
// Qt-free; lives in libchoir_host_core.

#include <string>

namespace choir {

// True if `path` exists and isn't disabled in place (X-GNOME-Autostart-enabled=false,
// as packaging/choir.desktop ships it, or the XDG-standard Hidden=true that KDE's
// startup-apps UI writes when you untick an entry).
bool autostart_enabled(const std::string& path);

// Write (on) or remove (off) the autostart entry, creating parent dirs as needed.
// `exec_path` is the absolute path of the host binary to launch. Returns false if the
// filesystem refused; the caller should re-read autostart_enabled() rather than assume.
bool set_autostart(const std::string& path, bool on, const std::string& exec_path);

}  // namespace choir
