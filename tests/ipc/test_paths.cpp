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
    return 0;
}
