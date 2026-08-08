#pragma once
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
namespace choir {
std::string runtime_dir();         // $XDG_RUNTIME_DIR or /tmp
std::string runtime_socket_path(); // runtime_dir()/choir.sock (legacy; see note)
std::string cache_home();          // $XDG_CACHE_HOME or $HOME/.cache
std::string avatar_cache_dir();    // cache_home()/choir/avatars
std::string config_home();         // $XDG_CONFIG_HOME or $HOME/.config
std::string config_path();         // config_home()/choir/config.json
std::string autostart_path();      // config_home()/autostart/choir.desktop (XDG autostart)
std::string data_home();           // $XDG_DATA_HOME or $HOME/.local/share

// data_home()/vulkan/implicit_layer.d/choir_overlay.x86_64.json — where the Vulkan
// loader looks for a per-user implicit layer. Written by the AppImage build of the
// host (see host/config/backends.hpp); for source/pacman installs meson writes the
// equivalent file under its own prefix instead.
std::string vulkan_manifest_path();

// The host<->layer IPC uses an ABSTRACT-namespace unix socket (not a filesystem
// path) so it crosses Steam's pressure-vessel container boundary: filesystem
// sockets under $XDG_RUNTIME_DIR are NOT shared into the container, but the
// network namespace IS, and abstract sockets live in the netns. Returns $CHOIR_SOCKET
// if set (used by tests for unique names), else "choir-overlay-<uid>".
std::string abstract_socket_name();

// Fill `addr` for an abstract AF_UNIX socket named `name` (prepends the leading
// NUL). Returns the addrlen to pass to bind()/connect(), or 0 if `name` is too long.
socklen_t make_abstract_addr(struct sockaddr_un& addr, const std::string& name);
}
