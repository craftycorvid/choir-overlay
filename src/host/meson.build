# `choir` host binary. Qt6 is wired in later tasks; for now just the stub + ipc.
executable('choir',
  'main.cpp',
  include_directories: ipc_inc,
  link_with: libchoir_ipc,
  cpp_args: ['-DCHOIR_VERSION="' + meson.project_version() + '"'],
  install: true,
)
