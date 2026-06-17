# libchoir_overlay.so: the Choir Vulkan implicit layer.
#
# A Vulkan layer is loaded via dlopen by the loader; it uses the Vulkan headers
# but must NOT link the loader (vulkan_dep provides headers + would also link
# libvulkan, which is harmless here since we only call through function pointers
# obtained from the chain, never libvulkan symbols directly).
vulkan_dep = dependency('vulkan')

layer_sources = files(
  'layer_entry.cpp',
  'dispatch.cpp',
  'gating.cpp',
  'swapchain.cpp',
)

libchoir_overlay = shared_library('choir_overlay',
  layer_sources,
  name_prefix: 'lib',
  include_directories: ipc_inc,
  link_with: libchoir_ipc,  # for later tasks (state client); harmless now
  dependencies: [vulkan_dep],
  # Vulkan structs are brace-initialized with just sType; the remaining
  # zero-init is intentional and idiomatic (matches the harness).
  cpp_args: ['-Wno-missing-field-initializers'],
  gnu_symbol_visibility: 'hidden',  # only the explicitly-exported entrypoints leak
  install: true,
)

# Implicit-layer manifest for the BUILD TREE. The library_path is relative
# ('libchoir_overlay.so'): the loader resolves it relative to the manifest dir,
# so `VK_LAYER_PATH=build/src/layer` finds the .so sitting beside the .json.
# Arch suffix is x86_64 (64-bit Vulkan-only for v1).
#
# TODO(Task 19): the install-time manifest needs an ABSOLUTE library_path to the
# installed .so under $XDG_DATA_HOME/vulkan/implicit_layer.d — done in packaging.
configure_file(
  input: 'manifest/choir_overlay.json.in',
  output: 'choir_overlay.x86_64.json',
  configuration: {
    'ARCH': 'x86_64',
    # './' prefix is REQUIRED: the loader only resolves a relative library_path
    # against the manifest's own directory when it contains a path separator;
    # a bare 'libchoir_overlay.so' is handed straight to dlopen and resolved
    # against the system library search path instead (=> "cannot open").
    'LIBPATH': './libchoir_overlay.so',
  },
)
