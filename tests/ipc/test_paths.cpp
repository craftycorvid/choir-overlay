#include "ipc/paths.hpp"
#include <cassert>
#include <cstdlib>
#include <string>
int main() {
    setenv("XDG_RUNTIME_DIR", "/run/user/test", 1);
    assert(choir::runtime_socket_path() == "/run/user/test/choir.sock");
    setenv("XDG_CACHE_HOME", "/tmp/c", 1);
    assert(choir::avatar_cache_dir() == "/tmp/c/choir/avatars");
    // Fallback: no XDG_CACHE_HOME -> $HOME/.cache
    unsetenv("XDG_CACHE_HOME"); setenv("HOME", "/home/u", 1);
    assert(choir::avatar_cache_dir() == "/home/u/.cache/choir/avatars");

    // The implicit-layer manifest the AppImage host writes must land where the Vulkan
    // loader actually searches, under $XDG_DATA_HOME when set and $HOME/.local/share
    // otherwise. Getting this wrong means a silently absent overlay in every game.
    setenv("XDG_DATA_HOME", "/tmp/d", 1);
    assert(choir::data_home() == "/tmp/d");
    assert(choir::vulkan_manifest_path() ==
           "/tmp/d/vulkan/implicit_layer.d/choir_overlay.x86_64.json");
    unsetenv("XDG_DATA_HOME");
    assert(choir::data_home() == "/home/u/.local/share");
    assert(choir::vulkan_manifest_path() ==
           "/home/u/.local/share/vulkan/implicit_layer.d/choir_overlay.x86_64.json");
    return 0;
}
