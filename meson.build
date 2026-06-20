project('choir', 'cpp',
  version: '0.0.1',
  default_options: ['cpp_std=c++20', 'warning_level=2'])

# Dear ImGui CORE only. We deliberately disable the wrap's built-in Vulkan backend
# (imgui_impl_vulkan.cpp): the layer vendors that one file itself, compiled with
# VK_NO_PROTOTYPES, so ImGui's Vulkan calls route through the layer's own dispatch
# tables (via ImGui_ImplVulkan_LoadFunctions) instead of the global loader. Inside a
# layer the physical-device/device handles are already unwrapped, so letting ImGui
# call the loader's trampolines fails with "Invalid physicalDevice" — routing through
# our dispatch is the correct fix. See src/layer/imgui_renderer.cpp.
#
# default_library=static forces the wrap to build a STATIC libimgui.a instead of a
# shared libimgui.so. This is load-bearing for the injected layer: a separate
# libimgui.so would expose global ImGui::* symbols that ELF interposition could bind
# into a game that ALSO uses Dear ImGui (MangoHud, Steam overlay, the game itself),
# crashing/corrupting state. Linked statically + a version script (src/layer/
# choir_overlay.version) hiding everything but the 3 Vulkan loader entrypoints, the
# layer carries its own private ImGui copy and exports none of it.
imgui_dep = dependency('imgui', fallback: ['imgui', 'imgui_dep'],
  default_options: ['vulkan=disabled', 'default_library=static'])
# Include dir holding the wrap's Vulkan backend (imgui_impl_vulkan.{h,cpp}). The layer
# compiles the backend via a thin wrapper TU that #includes imgui_impl_vulkan.cpp from
# here (a compile-time #include, so it sidesteps the meson source sandbox), built with
# VK_NO_PROTOTYPES. imgui_dep above is core-only (vulkan=disabled).
imgui_backends_inc = include_directories('subprojects' / 'imgui-1.92.5' / 'backends')
json_dep  = dependency('nlohmann_json', fallback: ['nlohmann_json', 'nlohmann_json_dep'])

# Qt6 for the host (Task 11). The state server only needs Core + Network
# (QLocalServer); the tray + settings window add Gui + Widgets.
qt6 = import('qt6')
qt_full_dep = dependency('qt6', modules: ['Core', 'Gui', 'Widgets', 'Network'])
# Lighter dep for the StateServer unit test (no Widgets/Gui needed there).
qt_core_net_dep = dependency('qt6', modules: ['Core', 'Network'])

subdir('src/ipc')
subdir('src/host')
subdir('src/layer')

# --- Install-time artifacts (packaging). The layer .so + host binary are installed by
# their own meson.build (install: true). Here we add the install-time implicit-layer
# manifest (ABSOLUTE library_path, derived from the prefix) and the desktop entry, so
# `meson install --prefix=X` is a complete, correct install for any X (e.g. /usr for the
# pacman package, ~/.local for install-user.sh). This manifest is generated in the TOP
# build dir (build/), NOT build/src/layer/, so the tests' VK_LAYER_PATH=build/src/layer
# only ever sees the relative build-tree manifest.
configure_file(
  input: 'src/layer/manifest/choir_overlay.json.in',
  output: 'choir_overlay.x86_64.json',
  configuration: {
    'ARCH': 'x86_64',
    'LIBPATH': get_option('prefix') / get_option('libdir') / 'choir' / 'libchoir_overlay.so',
  },
  install: true,
  install_dir: get_option('datadir') / 'vulkan' / 'implicit_layer.d',
)

# App-menu entry. Exec=choir (resolved on PATH).
install_data('packaging/choir.desktop',
  install_dir: get_option('datadir') / 'applications')

if get_option('build_tests')
  subdir('tests')
endif
