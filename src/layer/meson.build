# libchoir_overlay.so: the Vulkan implicit layer. Minimal stub for now.
vulkan_dep = dependency('vulkan')

shared_library('choir_overlay',
  'layer_entry.cpp',
  name_prefix: 'lib',
  include_directories: ipc_inc,
  link_with: libchoir_ipc,
  dependencies: [vulkan_dep],
  install: true,
)
