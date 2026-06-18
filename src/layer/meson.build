# libchoir_overlay.so: the Choir Vulkan implicit layer.
#
# A Vulkan layer is loaded via dlopen by the loader; it uses the Vulkan headers
# but must NOT link the loader (vulkan_dep provides headers + would also link
# libvulkan, which is harmless here since we only call through function pointers
# obtained from the chain, never libvulkan symbols directly).
vulkan_dep = dependency('vulkan')

# Exposed so tests can include layer headers (e.g. fade.hpp) without an
# include_directories sandbox violation from the tests/ subdir.
layer_inc = include_directories('.')

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

# Symbol-visibility hardening for an INJECTED layer (Task 19 / Task 15 review).
#
# The layer is dlopen'd into games that may ALSO use Dear ImGui (MangoHud, the Steam
# overlay, the game itself). If our ImGui::* / ImGui_ImplVulkan_* symbols were exported,
# ELF symbol interposition could bind a game's ImGui calls into OUR copy (or vice
# versa) -> crash / state corruption. An injected layer MUST export ONLY the Vulkan
# loader entrypoints.
#
# Two layers of defence, both required:
#   1. --version-script: CHOIR_1 makes ONLY the 3 loader entrypoints global and
#      forces `local: *` on everything else. This is load-bearing: even though the
#      wrap stamps the ImGui backend TUs with -DIMGUI_API=visibility("default"), the
#      version script demotes them to local so nothing but the 3 vk* symbols is
#      dynamically exported. Verify with `nm -CD`.
#   2. --exclude-libs,ALL: belt-and-braces — keeps symbols pulled in from the static
#      archives (libimgui.a, the vendored backend, libchoir_ipc.a) out of the dynamic
#      symbol table too.
# imgui is now linked STATICALLY (default_library=static in the top-level meson.build),
# so there is no separate libimgui.so to interpose against.
layer_version_script = meson.current_source_dir() / 'choir_overlay.version'

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
  link_args: [
    '-Wl,--version-script=@0@'.format(layer_version_script),
    '-Wl,--exclude-libs,ALL',
  ],
  link_depends: layer_version_script,  # relink when the export list changes
  install: true,
  install_dir: get_option('libdir') / 'choir',  # per-user-friendly: <prefix>/lib/choir
)

# Implicit-layer manifest for the BUILD TREE. The library_path is relative
# ('libchoir_overlay.so'): the loader resolves it relative to the manifest dir,
# so `VK_LAYER_PATH=build/src/layer` finds the .so sitting beside the .json.
# Arch suffix is x86_64 (64-bit Vulkan-only for v1).
#
# This build-tree manifest uses a relative library_path (./libchoir_overlay.so) for
# tests via VK_LAYER_PATH=build/src/layer. The install-time manifest (absolute
# library_path under $XDG_DATA_HOME/vulkan/implicit_layer.d) is written separately
# by packaging/install-user.sh.
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
