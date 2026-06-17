# libchoir_ipc: Qt-free static lib shared by host and layer.
# include_directories('..') so headers resolve as `ipc/paths.hpp` everywhere.
ipc_inc = include_directories('..')

ipc_sources = files(
  'paths.cpp',
  'state.cpp',
  'protocol.cpp',
  'framing.cpp',
  'avatar_file.cpp',
)

libchoir_ipc = static_library('choir_ipc',
  ipc_sources,
  include_directories: ipc_inc,
  dependencies: [json_dep],
)
