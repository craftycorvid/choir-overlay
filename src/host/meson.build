# Host build.
#
# libchoir_host_core: Qt-free static lib for the host's pure logic, so it stays
# unit-testable without Qt. Later tasks (4,5,6,8,9,10) append their .cpp here.
# Qt-using code (ui/, qt_http, the QLocalServer state server) lands in Task 11
# as separate sources and must NOT be added to this lib.
#
# host_inc lets host headers resolve as `"discord/ipc_transport.hpp"`.
host_inc = include_directories('.')

host_core_sources = files(
  'discord/ipc_transport.cpp',
  'discord/rpc_messages.cpp',
  'discord/oauth.cpp',
  'discord/rpc_client.cpp',
  'model/overlay_state.cpp',
  'model/avatar_cache.cpp',
)

libchoir_host_core = static_library('choir_host_core',
  host_core_sources,
  include_directories: [ipc_inc, host_inc],
  link_with: libchoir_ipc,
  dependencies: [json_dep],
)

# `choir` host binary. Qt6 is wired in later tasks; for now just the stub + core.
executable('choir',
  'main.cpp',
  include_directories: [ipc_inc, host_inc],
  link_with: [libchoir_ipc, libchoir_host_core],
  cpp_args: ['-DCHOIR_VERSION="' + meson.project_version() + '"'],
  install: true,
)
