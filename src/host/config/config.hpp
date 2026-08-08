#pragma once

// Persisted host configuration: config.json (Task 10).
//
// Stores overlay appearance, Discord auth (mode + client credentials + cached
// OAuth tokens), and the process denylist. Because it holds an access token,
// the on-disk file is written with mode 0600.
//
// load() is intentionally defensive: an absent, unreadable, partial, or corrupt
// file yields a default-constructed Config (with the default denylist
// populated) and NEVER throws. Present-but-partial files fill missing fields
// with defaults.
//
// Qt-free: std + nlohmann + POSIX (<fcntl.h>/<sys/stat.h>).

#include <string>
#include <vector>

#include "ipc/state.hpp"  // AppearanceConfig

namespace choir {

struct Config {
    AppearanceConfig appearance;  // overlay look (anchor/scale/hdr_nits/...)

    // --- auth (Streamkit) ---
    std::string client_id = "207646673902501888";   // Streamkit app id
    std::string access_token;                         // cached OAuth token
    std::string refresh_token;

    // --- activation ---
    // Glob patterns; if empty after parse, load() populates the defaults.
    std::vector<std::string> denylist;

    // --- AppImage backend install (see config/backends.hpp) ---
    // An AppImage has to copy the two injected .so files out to ~/.local before any
    // game can load them, i.e. write outside the image, so we ask once and remember
    // the answer. Never consulted for source/pacman installs, where nothing asks.
    static constexpr int kBackendUnasked = 0;
    static constexpr int kBackendGranted = 1;
    static constexpr int kBackendDeclined = 2;
    int backend_consent = kBackendUnasked;

    // Returns defaults (incl. the default denylist) if the file is
    // absent/unreadable/corrupt. Never throws.
    static Config load(const std::string& path);

    // Serializes to JSON at `path`, creating the parent directory if needed.
    // The resulting file mode is exactly 0600 (it holds a token). Returns false
    // on any I/O error.
    bool save(const std::string& path) const;
};

}  // namespace choir
