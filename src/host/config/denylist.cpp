#include "config/denylist.hpp"

#include <fnmatch.h>

#include <algorithm>
#include <cctype>

namespace choir {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Strip any leading path: return the segment after the last '/'.
std::string basename_of(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

}  // namespace

Denylist::Denylist(std::vector<std::string> patterns) : patterns_(std::move(patterns)) {}

bool Denylist::blocks(const std::string& exe_name) const {
    const std::string base = to_lower(basename_of(exe_name));
    for (const auto& pat : patterns_) {
        const std::string lpat = to_lower(pat);
        if (::fnmatch(lpat.c_str(), base.c_str(), 0) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> Denylist::defaults() {
    // Exact names of common non-game processes, plus a couple of broad globs.
    // Stored mixed-case where it reads naturally; matching is case-insensitive.
    return {
        "Discord",
        "steam",
        "steamwebhelper",
        "gamescope",
        "obs",
        "obs-studio",
        "firefox",
        "chrome",
        "chromium",
        "code",
        "electron",
        "choir",
        "*launcher*",
    };
}

}  // namespace choir
