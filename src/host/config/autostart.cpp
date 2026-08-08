#include "config/autostart.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace choir {

namespace fs = std::filesystem;

namespace {

// Desktop-entry Exec values are shell-ish: a path with a space or a quote in it must
// be quoted, with `"`, `\`, `$` and backtick backslash-escaped (XDG desktop entry spec).
std::string quote_exec(const std::string& path) {
    if (path.find_first_of(" \t\"'\\$`") == std::string::npos) return path;
    std::string out = "\"";
    for (char c : path) {
        if (c == '"' || c == '\\' || c == '$' || c == '`') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

// Whether `line` is `key=value` with a value of "true" (case-insensitive, trimmed).
bool key_is_true(const std::string& line, const std::string& key) {
    if (line.compare(0, key.size(), key) != 0 || line.size() <= key.size()) return false;
    std::string rest = line.substr(key.size());
    // Tolerate spaces around the '=' even though the spec forbids them.
    const auto eq = rest.find('=');
    if (eq == std::string::npos || rest.find_first_not_of(" \t") != eq) return false;
    std::string value = rest.substr(eq + 1);
    const auto begin = value.find_first_not_of(" \t\r");
    const auto end = value.find_last_not_of(" \t\r");
    if (begin == std::string::npos) return false;
    value = value.substr(begin, end - begin + 1);
    for (char& c : value) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return value == "true";
}

}  // namespace

std::string host_exec_path(const std::string& app_path) {
    const char* appimage = std::getenv("APPIMAGE");
    if (appimage && appimage[0] != '\0') return std::string(appimage);
    return app_path;
}

bool autostart_enabled(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    for (std::string line; std::getline(in, line);) {
        if (key_is_true(line, "Hidden")) return false;
        // Present-and-not-true is how the shipped template disables itself.
        if (line.compare(0, 26, "X-GNOME-Autostart-enabled=") == 0 &&
            !key_is_true(line, "X-GNOME-Autostart-enabled")) {
            return false;
        }
    }
    return true;
}

bool set_autostart(const std::string& path, bool on, const std::string& exec_path) {
    std::error_code ec;
    if (!on) {
        fs::remove(path, ec);
        return !ec;
    }

    const fs::path dir = fs::path(path).parent_path();
    if (!dir.empty()) {
        fs::create_directories(dir, ec);
        if (ec) return false;
    }

    // Kept in sync by hand with packaging/choir.desktop (the app-menu entry meson
    // installs). Generated rather than copied because the installed template's
    // location varies by prefix (/usr vs ~/.local), and autostart needs an ABSOLUTE
    // Exec — the login session has no useful PATH.
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << "[Desktop Entry]\n"
           "Type=Application\n"
           "Name=Choir\n"
           "Comment=Discord voice overlay for Vulkan and OpenGL games "
           "(not affiliated with Discord Inc.)\n"
           "Exec=" << quote_exec(exec_path) << "\n"
           "Icon=choir\n"
           "Terminal=false\n"
           "X-GNOME-Autostart-enabled=true\n";
    out.close();
    return out.good();
}

}  // namespace choir
