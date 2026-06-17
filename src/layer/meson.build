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
  'imgui_renderer.cpp',
  'overlay_ui.cpp',
  'state_client.cpp',
  'avatar_textures.cpp',
)

# Vendored Dear ImGui Vulkan backend, compiled INTO the layer (via the unity wrapper
# TU that #includes imgui_impl_vulkan.cpp) with VK_NO_PROTOTYPES so its Vulkan calls go
# through function pointers we supply via ImGui_ImplVulkan_LoadFunctions (which resolve
# through the layer's own dispatch). The wrap's libimgui.so is built core-only
# (vulkan=disabled) so there is no duplicate backend symbol to clash with.
imgui_backend_lib = static_library('choir_imgui_vk_backend',
  files('imgui_impl_vulkan_unity.cpp'),
  include_directories: [imgui_backends_inc],
  dependencies: [vulkan_dep, imgui_dep],
  cpp_args: [
    '-Wno-missing-field-initializers',
    '-Wno-unused-parameter',
  ],
)

libchoir_overlay = shared_library('choir_overlay',
  layer_sources,
  name_prefix: 'lib',
  include_directories: [ipc_inc, imgui_backends_inc],
  link_with: [libchoir_ipc, imgui_backend_lib],  # ipc for later tasks; backend = vendored impl
  # imgui_dep brings Dear ImGui CORE (no Vulkan backend) + its include dirs.
  # threads: the state client runs its socket I/O on a std::thread.
  # json_dep: the state client parses Snapshot/AvatarReady JSON (ipc/state.hpp).
  dependencies: [vulkan_dep, imgui_dep, json_dep, dependency('threads')],
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
