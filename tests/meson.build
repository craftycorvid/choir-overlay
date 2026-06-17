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

test('ipc_transport',
  executable('t_ipc_transport',
    'host/test_ipc_transport.cpp',
    link_with: [libchoir_ipc, libchoir_host_core],
    include_directories: [ipc_inc, host_inc],
    dependencies: [json_dep],
  ),
)

test('rpc_messages',
  executable('t_rpc_messages',
    'host/test_rpc_messages.cpp',
    link_with: [libchoir_ipc, libchoir_host_core],
    include_directories: [ipc_inc, host_inc],
    dependencies: [json_dep],
  ),
)

test('oauth',
  executable('t_oauth',
    'host/test_oauth.cpp',
    link_with: [libchoir_ipc, libchoir_host_core],
    include_directories: [ipc_inc, host_inc],
    dependencies: [json_dep],
  ),
)
