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
imgui_dep = dependency('imgui', fallback: ['imgui', 'imgui_dep'],
  default_options: ['vulkan=disabled'])
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

if get_option('build_tests')
  subdir('tests')
endif
