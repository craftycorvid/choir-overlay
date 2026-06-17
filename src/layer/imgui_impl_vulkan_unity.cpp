// Thin wrapper that builds Dear ImGui's Vulkan backend INTO the Choir layer with
// VK_NO_PROTOTYPES, so the backend's Vulkan calls go through function pointers we
// supply via ImGui_ImplVulkan_LoadFunctions (resolving through the layer's own
// dispatch chain) instead of the global loader trampolines, which reject the
// unwrapped handles a layer sees. See src/layer/imgui_renderer.cpp.
//
// We #include the backend .cpp from the imgui subproject's backends/ include dir (on
// the include path via meson). A compile-time #include sidesteps the meson source
// sandbox that forbids listing a nested-subproject file directly as a source, while
// still building exactly the vendored backend (no duplicate symbol vs libimgui.so,
// which is built core-only with vulkan=disabled).
#define VK_NO_PROTOTYPES
#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES

#include "imgui_impl_vulkan.cpp"
