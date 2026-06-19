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

test('rpc_client',
  executable('t_rpc_client',
    'host/test_rpc_client.cpp',
    link_with: [libchoir_ipc, libchoir_host_core],
    include_directories: [ipc_inc, host_inc],
    dependencies: [json_dep, dependency('threads')],
  ),
)

test('overlay_state',
  executable('t_overlay_state',
    'host/test_overlay_state.cpp',
    link_with: [libchoir_ipc, libchoir_host_core],
    include_directories: [ipc_inc, host_inc],
    dependencies: [json_dep],
  ),
)

test('avatar_cache',
  executable('t_avatar_cache',
    'host/test_avatar_cache.cpp',
    link_with: [libchoir_ipc, libchoir_host_core],
    include_directories: [ipc_inc, host_inc],
  ),
)

test('config',
  executable('t_config',
    'host/test_config.cpp',
    link_with: [libchoir_ipc, libchoir_host_core],
    include_directories: [ipc_inc, host_inc],
    dependencies: [json_dep],
  ),
)

test('denylist',
  executable('t_denylist',
    'host/test_denylist.cpp',
    link_with: [libchoir_ipc, libchoir_host_core],
    include_directories: [ipc_inc, host_inc],
  ),
)

# StateServer test: needs a Qt event loop (QtCore) + QtNetwork (QLocalServer),
# but NOT Widgets/Gui — it talks to the server with a raw AF_UNIX client.
test('state_server',
  executable('t_state_server',
    'host/test_state_server.cpp',
    files('../src/host/server/state_server.cpp'),
    link_with: [libchoir_ipc, libchoir_host_core],
    include_directories: [ipc_inc, host_inc],
    dependencies: [json_dep, qt_core_net_dep],
  ),
)

# M0 auth spike (Task 7 gate): an interactive, manually-run binary that streams
# live voice events from a real Discord client. Built (so it's ready to run) but
# NOT registered as a test — it needs a real Discord client + human OAuth consent.
executable('auth_spike',
  'harness/auth_spike.cpp',
  files('../src/host/discord/qt_http.cpp'),
  link_with: [libchoir_ipc, libchoir_host_core],
  include_directories: [ipc_inc, host_inc],
  dependencies: [json_dep, qt_full_dep],  # qt_http.cpp's QtAvatarSource needs QtGui (QImage)
)

# Vulkan layer test harness (Task 12). Not registered tests — used by the layer
# golden tests (Tasks 13-18) and for manual runs.
#   vk_min_present: headless Vulkan swapchain present app (loads the layer into it)
#   fake_host: scripted local-IPC server impersonating the choir host
vk_min_present = executable('vk_min_present',
  'harness/vk_min_present.cpp',
  dependencies: [dependency('vulkan')],
  # Vulkan structs are brace-initialized with just sType ({VK_STRUCTURE_TYPE_...});
  # the remaining zero-init is intentional and idiomatic.
  cpp_args: ['-Wno-missing-field-initializers'],
)
fake_host = executable('fake_host',
  'harness/fake_host.cpp',
  link_with: [libchoir_ipc],
  include_directories: [ipc_inc],
  dependencies: [json_dep],
)

# Task 13: load the Choir implicit layer into vk_min_present and confirm it is a
# transparent pass-through. The layer .so + its build-tree manifest live in
# build/src/layer (libchoir_overlay target above). We force the layer on via the
# loader env vars and point VK_LAYER_PATH at that dir. vk_min_present exits 77
# when no headless surface is available — meson treats exit 77 as SKIP.
# Layer opacity-easing math (pure; no Vulkan/ImGui — just includes src/layer/fade.hpp).
test('fade',
  executable('t_fade',
    'layer/test_fade.cpp',
    include_directories: layer_inc,
  ),
)

# Layer sRGB<->linear conversion (pure; includes src/layer/srgb.hpp).
test('srgb',
  executable('t_srgb',
    'layer/test_srgb.cpp',
    include_directories: layer_inc,
  ),
)

layer_dir = meson.current_build_dir() / '..' / 'src' / 'layer'
# Point XDG_DATA_HOME at an empty dir so the loader does NOT discover a globally
# *installed* choir implicit layer (~/.local/share/vulkan/implicit_layer.d) — which
# would shadow (dedup by name) the build-tree layer we load via VK_LAYER_PATH and
# make these tests depend on whatever is installed. Only the build layer must load.
xdg_isolate = meson.current_build_dir() / 'test-xdg-isolate'
test('layer_loads',
  vk_min_present,
  args: ['--frames', '2', '--layer-info'],
  depends: [libchoir_overlay],
  env: {
    'VK_LAYER_PATH': layer_dir,
    'VK_INSTANCE_LAYERS': 'VK_LAYER_choir_overlay_x86_64',
    'VK_LOADER_LAYERS_ENABLE': 'VK_LAYER_choir_overlay_x86_64',
    'XDG_DATA_HOME': xdg_isolate,
  },
)

# Task 17: feature-based golden test for the real overlay. The driver starts fake_host
# on a temp XDG_RUNTIME_DIR, runs vk_min_present with the layer + readback, and asserts
# the voice panel + three avatar colors (red/green/blue) + Bob's speaking ring + Carol's
# mute glyph are drawn in the panel region while a far pixel stays the app blue. It also
# runs a --recreate present (avatars must survive the swapchain rebuild via the retention
# fix) and a no-host present (overlay draws nothing; frame stays blue). vk_min_present's
# exit 77 (no headless surface) is propagated as a SKIP.
test_layer_golden = executable('test_layer_golden',
  'harness/test_layer_golden.cpp',
  cpp_args: ['-Wno-missing-field-initializers'],
)
test('layer_golden',
  test_layer_golden,
  args: [vk_min_present.full_path(), fake_host.full_path(), layer_dir,
         meson.current_build_dir() / 'layer_golden.ppm'],
  depends: [libchoir_overlay, vk_min_present, fake_host],
  env: {'XDG_DATA_HOME': xdg_isolate},
)

# Task 16: state-client + avatar-texture integration. The driver starts fake_host on
# a temp XDG_RUNTIME_DIR, runs vk_min_present with the layer + CHOIR_DEBUG_DUMP, and
# asserts the dumped snapshot matches fake_host's scripted state (3 participants, Bob
# speaking, Carol self_mute). It also runs vk_min_present WITHOUT a host to prove the
# layer is safe (no crash / no hang) when the socket is absent. vk_min_present's exit
# 77 (no headless surface) is propagated as a SKIP.
test('layer_state',
  executable('test_layer_state',
    'harness/test_layer_state.cpp',
    include_directories: [ipc_inc],
    dependencies: [json_dep],
  ),
  args: [vk_min_present.full_path(), fake_host.full_path(), layer_dir],
  depends: [libchoir_overlay, vk_min_present, fake_host],
  env: {'XDG_DATA_HOME': xdg_isolate},
)

# Task 18: fault-injection / failure-isolation test. The driver runs vk_min_present with
# the layer forced on under injected faults (corrupt avatars, malformed snapshot frame,
# host disconnect mid-stream, in_voice flapping for 300 frames) and asserts the app
# ALWAYS exits 0 within a per-case timeout (a hang is a FAIL, killed by a watchdog). It
# also proves lazy-init: a no-voice run does ZERO overlay allocation (the
# CHOIR_DEBUG_LAZY_INIT signal is never logged). vk_min_present's exit 77 (no headless
# surface) is propagated as a SKIP. The 300-frame case bumps the meson timeout.
test('layer_faults',
  executable('test_layer_faults',
    'harness/test_layer_faults.cpp',
    dependencies: [dependency('threads')],
  ),
  args: [vk_min_present.full_path(), fake_host.full_path(), layer_dir],
  depends: [libchoir_overlay, vk_min_present, fake_host],
  timeout: 240,
  env: {'XDG_DATA_HOME': xdg_isolate},
)
