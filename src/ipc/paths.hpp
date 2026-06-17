#pragma once
#include <string>
namespace choir {
std::string runtime_dir();         // $XDG_RUNTIME_DIR or /tmp
std::string runtime_socket_path(); // runtime_dir()/choir.sock
std::string cache_home();          // $XDG_CACHE_HOME or $HOME/.cache
std::string avatar_cache_dir();    // cache_home()/choir/avatars
std::string config_home();         // $XDG_CONFIG_HOME or $HOME/.config
std::string config_path();         // config_home()/choir/config.json
}
