# libchoir_overlay_shared: backend-agnostic overlay code linked by BOTH the Vulkan layer
# and the GL interposer. ImGui CORE only (imgui_dep); no renderer backend lives here —
# each .so adds its own (imgui_impl_vulkan / imgui_impl_opengl3).
overlay_inc = include_directories('.')

libchoir_overlay_shared = static_library('choir_overlay_shared',
  files('overlay_ui.cpp', 'state_client.cpp', 'gating.cpp'),
  include_directories: [overlay_inc, ipc_inc],
  link_with: [libchoir_ipc],
  dependencies: [imgui_dep, json_dep, dependency('threads')],
)
