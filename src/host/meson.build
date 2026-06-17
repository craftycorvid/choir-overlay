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
  'config/config.cpp',
  'config/denylist.cpp',
)

libchoir_host_core = static_library('choir_host_core',
  host_core_sources,
  include_directories: [ipc_inc, host_inc],
  link_with: libchoir_ipc,
  dependencies: [json_dep],
)

# `choir` host binary: main.cpp + the Qt-using sources (state server, Qt-backed
# HTTP/avatar fetchers, tray + settings UI). These compile DIRECTLY into the
# executable and link libchoir_host_core + libchoir_ipc + Qt6; they are kept out
# of the Qt-free libchoir_host_core.
#
# MOC runs on the Q_OBJECT headers (Tray, SettingsWindow). state_server.hpp and
# qt_http.hpp have no Q_OBJECT (lambda connections only) so they need no MOC.
host_qt_moc = qt6.preprocess(
  moc_headers: files(
    'ui/tray.hpp',
    'ui/settings_window.hpp',
  ),
  dependencies: qt_full_dep,
  include_directories: [ipc_inc, host_inc],
)

host_qt_sources = files(
  'server/state_server.cpp',
  'discord/qt_http.cpp',
  'ui/tray.cpp',
  'ui/settings_window.cpp',
)

executable('choir',
  'main.cpp',
  host_qt_sources,
  host_qt_moc,
  include_directories: [ipc_inc, host_inc],
  link_with: [libchoir_ipc, libchoir_host_core],
  dependencies: [json_dep, qt_full_dep],
  cpp_args: ['-DCHOIR_VERSION="' + meson.project_version() + '"'],
  install: true,
)
