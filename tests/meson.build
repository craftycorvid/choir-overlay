test('paths',
  executable('t_paths',
    'ipc/test_paths.cpp',
    link_with: libchoir_ipc,
    include_directories: ipc_inc,
  ),
)

test('state',
  executable('t_state',
    'ipc/test_state.cpp',
    link_with: libchoir_ipc,
    include_directories: ipc_inc,
    dependencies: [json_dep],
  ),
)

test('protocol',
  executable('t_protocol',
    'ipc/test_protocol.cpp',
    link_with: libchoir_ipc,
    include_directories: ipc_inc,
  ),
)

test('framing',
  executable('t_framing',
    'ipc/test_framing.cpp',
    link_with: libchoir_ipc,
    include_directories: ipc_inc,
  ),
)

test('avatar_file',
  executable('t_avatar_file',
    'ipc/test_avatar_file.cpp',
    link_with: libchoir_ipc,
    include_directories: ipc_inc,
  ),
)
