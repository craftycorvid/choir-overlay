// Choir helpers layered on the vendored ImGui Vulkan backend. Defined in
// imgui_impl_vulkan_unity.cpp (which #includes imgui_impl_vulkan.cpp, giving these access
// to the backend's static internals without modifying vendored code).
#pragma once
#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>
namespace choir {
// Build a graphics pipeline identical to ImGui's overlay pipeline but with `frag_spv` as
// the fragment stage and specialization constants {0:int mode, 1:float nits}. Reuses
// ImGui's vertex shader module + pipeline layout, so it stays compatible with the
// descriptor sets/push constants RenderDrawData binds. Returns VK_NULL_HANDLE on failure.
VkPipeline create_hdr_pipeline(VkRenderPass render_pass, const uint32_t* frag_spv,
                               size_t frag_spv_size_bytes, int mode, float nits);
}
