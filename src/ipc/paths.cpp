#include "ipc/paths.hpp"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

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

std::string abstract_socket_name() {
    // $CHOIR_SOCKET lets tests pick a unique name (and a user override the default).
    return env_or("CHOIR_SOCKET", "choir-overlay-" + std::to_string(::getuid()));
}

socklen_t make_abstract_addr(struct sockaddr_un& addr, const std::string& name) {
    if (name.size() + 1 > sizeof(addr.sun_path)) return 0;  // +1 for the leading NUL
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    std::memcpy(addr.sun_path + 1, name.data(), name.size());
    return static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + 1 + name.size());
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
