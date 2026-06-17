#include "ipc/paths.hpp"
#include <cstdlib>

namespace choir {

namespace {
// Return the value of env var `name`, or `fallback` if it is unset or empty.
std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    if (v && v[0] != '\0') return std::string(v);
    return fallback;
}
} // namespace

std::string runtime_dir() {
    return env_or("XDG_RUNTIME_DIR", "/tmp");
}

std::string runtime_socket_path() {
    return runtime_dir() + "/choir.sock";
}

std::string cache_home() {
    std::string home = env_or("HOME", "");
    return env_or("XDG_CACHE_HOME", home + "/.cache");
}

std::string avatar_cache_dir() {
    return cache_home() + "/choir/avatars";
}

std::string config_home() {
    std::string home = env_or("HOME", "");
    return env_or("XDG_CONFIG_HOME", home + "/.config");
}

std::string config_path() {
    return config_home() + "/choir/config.json";
}

} // namespace choir
