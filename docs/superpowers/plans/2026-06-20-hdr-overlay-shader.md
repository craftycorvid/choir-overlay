# HDR Overlay Shader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render the Choir overlay with correct color AND a configurable paper-white brightness on HDR swapchains (scRGB / HDR10-PQ / HLG), by moving the color transfer into a fragment shader.

**Architecture:** A per-swapchain custom graphics pipeline whose fragment shader applies a per-color-space transfer (sRGB→linear, ±BT.2020+PQ/HLG, ±paper-white scale) to the final fragment color. The transfer mode + nits are baked as specialization constants. We feed this pipeline to `ImGui_ImplVulkan_RenderDrawData(draw_data, cmd, pipeline)` and reuse ImGui's own vertex shader + pipeline layout (via a helper in our unity TU). Avatars become plain UNORM so the shader handles their decode/encode uniformly.

**Tech Stack:** C++17, Vulkan, Dear ImGui 1.92.5 (vendored), GLSL→SPIR-V via glslangValidator, Meson.

**Spec:** `docs/superpowers/specs/2026-06-20-hdr-overlay-shader-design.md`

---

### Task 1: Color model — five transfer modes + nits

**Goal:** `swapchain_color.hpp` exposes a 5-value `TransferFunction` (adds `ScRgb`), re-adds `is_hdr_float_format`, maps `PASS_THROUGH`+float and `EXTENDED_SRGB_LINEAR` to `ScRgb`, and the CPU transfer math takes a `nits` argument — all unit-tested.

**Files:**
- Modify: `src/layer/swapchain_color.hpp`
- Test: `tests/layer/test_swapchain_color.cpp`

**Acceptance Criteria:**
- [ ] `TransferFunction` is `{ None, Srgb, ScRgb, Pq, Hlg }`.
- [ ] `transfer_function_for(R16G16B16A16_SFLOAT, PASS_THROUGH) == ScRgb`.
- [ ] `transfer_function_for(R16G16B16A16_SFLOAT, EXTENDED_SRGB_LINEAR) == ScRgb`.
- [ ] `transfer_function_for(A2B10G10R10_UNORM_PACK32, HDR10_ST2084) == Pq`.
- [ ] `transfer_function_for(B8G8R8A8_SRGB, SRGB_NONLINEAR) == Srgb`.
- [ ] `transfer_function_for(B8G8R8A8_UNORM, SRGB_NONLINEAR) == None`.
- [ ] `apply_transfer(ScRgb, nits=200, ...)` scales linear by 200/80 = 2.5 (white → 2.5).

**Verify:** `meson test -C build swapchain_color` → OK

**Steps:**

- [ ] **Step 1: Update the header.** In `src/layer/swapchain_color.hpp`:
  - Change the enum to `enum class TransferFunction { None, Srgb, ScRgb, Pq, Hlg };`
  - Re-add the helper:
    ```cpp
    inline bool is_hdr_float_format(VkFormat f) {
        switch (f) {
            case VK_FORMAT_R16G16B16A16_SFLOAT:
            case VK_FORMAT_R16G16B16_SFLOAT:
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
                return true;
            default:
                return false;
        }
    }
    ```
  - Rewrite `transfer_function_for`:
    ```cpp
    inline TransferFunction transfer_function_for(VkFormat fmt, VkColorSpaceKHR cs) {
        switch (cs) {
            case VK_COLOR_SPACE_HDR10_ST2084_EXT:         return TransferFunction::Pq;
            case VK_COLOR_SPACE_HDR10_HLG_EXT:            return TransferFunction::Hlg;
            case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: return TransferFunction::ScRgb;
            case VK_COLOR_SPACE_PASS_THROUGH_EXT:
                return is_hdr_float_format(fmt) ? TransferFunction::ScRgb : TransferFunction::None;
            default: break;
        }
        return is_srgb_format(fmt) ? TransferFunction::Srgb : TransferFunction::None;
    }
    ```
  - Change `apply_transfer` to take nits and handle `ScRgb` (the CPU version is kept as the source-of-truth mirror of the shader, used by tests):
    ```cpp
    inline void apply_transfer(TransferFunction tf, float nits, float& r, float& g, float& b) {
        if (tf == TransferFunction::None) return;
        r = srgb_to_linear(r); g = srgb_to_linear(g); b = srgb_to_linear(b);
        if (tf == TransferFunction::Srgb) return;
        if (tf == TransferFunction::ScRgb) {
            const float s = nits / 80.0f;
            r *= s; g *= s; b *= s;
            return;
        }
        bt709_to_bt2020(r, g, b);
        if (tf == TransferFunction::Pq) {
            r = linear_to_pq(r, nits); g = linear_to_pq(g, nits); b = linear_to_pq(b, nits);
        } else {
            r = linear_to_hlg(r); g = linear_to_hlg(g); b = linear_to_hlg(b);
        }
    }
    ```
  - Parameterize `linear_to_pq` to take nits as `targetL` (was a hard-coded 200):
    ```cpp
    inline float linear_to_pq(float in, float targetL) {
        const float m1 = 0.1593017578125f, m2 = 78.84375f;
        const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
        in = std::pow(in * (targetL / 10000.0f), m1);
        in = (c1 + c2 * in) / (1.0f + c3 * in);
        return std::pow(in, m2);
    }
    ```
  - Delete `apply_transfer_u8` (the shader replaces the per-vertex u8 path; nothing else uses it after Task 4). Keep `transfer_name` and update it to include `ScRgb` → `"scrgb"`.

- [ ] **Step 2: Update the unit test** `tests/layer/test_swapchain_color.cpp` to the new signatures:
  ```cpp
  #include "swapchain_color.hpp"
  #include <cassert>
  #include <cmath>
  using namespace choir;
  static bool approx(float a, float b, float t = 1e-3f) { return std::fabs(a - b) <= t; }
  int main() {
      assert(transfer_function_for(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_PASS_THROUGH_EXT) == TransferFunction::ScRgb);
      assert(transfer_function_for(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) == TransferFunction::ScRgb);
      assert(transfer_function_for(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT) == TransferFunction::Pq);
      assert(transfer_function_for(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_HDR10_HLG_EXT) == TransferFunction::Hlg);
      assert(transfer_function_for(VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) == TransferFunction::Srgb);
      assert(transfer_function_for(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) == TransferFunction::None);
      assert(transfer_function_for(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_PASS_THROUGH_EXT) == TransferFunction::None);

      { float r=0,g=0,b=0; apply_transfer(TransferFunction::None,200,r,g,b); assert(r==0&&g==0&&b==0); }
      { float r=1,g=0.5f,b=0; apply_transfer(TransferFunction::Srgb,200,r,g,b); assert(approx(r,1)&&approx(g,0.214f,5e-3f)); }
      { float r=1,g=1,b=1; apply_transfer(TransferFunction::ScRgb,200,r,g,b); assert(approx(r,2.5f)&&approx(g,2.5f)); }
      { float r=0,g=0,b=0; apply_transfer(TransferFunction::Pq,200,r,g,b); assert(approx(r,0,5e-3f)); float w=1,x=1,y=1; apply_transfer(TransferFunction::Pq,200,w,x,y); assert(w>0.3f&&w<0.8f); }
      { float r=1,g=1,b=1; apply_transfer(TransferFunction::Hlg,200,r,g,b); assert(r>0.5f&&r<=1.0f); }
      return 0;
  }
  ```

- [ ] **Step 3: Build + run.** `meson compile -C build && meson test -C build swapchain_color` → expect OK.

- [ ] **Step 4: Commit.**
  ```bash
  git add src/layer/swapchain_color.hpp tests/layer/test_swapchain_color.cpp
  git commit -m "feat(layer): 5-mode transfer model (add ScRgb) + nits-parameterized transfer math"
  ```

---

### Task 2: HDR fragment shader (GLSL + generated SPIR-V header)

**Goal:** A fragment shader that applies the per-mode transfer to the final fragment color, compiled to an embedded SPIR-V header.

**Files:**
- Create: `src/layer/shaders/overlay_hdr.frag`
- Create: `src/layer/shaders/overlay_hdr.frag.spv.h` (generated)

**Acceptance Criteria:**
- [ ] `glslangValidator -V src/layer/shaders/overlay_hdr.frag` compiles with no errors.
- [ ] `overlay_hdr.frag.spv.h` defines `static const uint32_t choir_overlay_hdr_frag_spv[]`.

**Verify:** `glslangValidator -V src/layer/shaders/overlay_hdr.frag -o /tmp/t.spv` → "compilation terminated" absent, exit 0

**Steps:**

- [ ] **Step 1: Write the GLSL** `src/layer/shaders/overlay_hdr.frag`. Input/output match ImGui's stock `overlay.frag` so it pairs with ImGui's vertex shader:
  ```glsl
  #version 450
  layout(location = 0) out vec4 fColor;
  layout(set = 0, binding = 0) uniform sampler2D sTexture;
  layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

  layout(constant_id = 0) const int  uMode = 0;     // 0 None,1 Srgb,2 ScRgb,3 Pq,4 Hlg
  layout(constant_id = 1) const float uNits = 200.0;

  float s2l(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
  vec3  s2l(vec3 c)  { return vec3(s2l(c.r), s2l(c.g), s2l(c.b)); }
  vec3  to2020(vec3 c) {
      return vec3(0.627392*c.r + 0.329030*c.g + 0.0432691*c.b,
                  0.069123*c.r + 0.919523*c.g + 0.0113204*c.b,
                  0.016423*c.r + 0.088042*c.g + 0.8956166*c.b);
  }
  float l2pq(float v) {
      const float m1=0.1593017578125, m2=78.84375, c1=0.8359375, c2=18.8515625, c3=18.6875;
      v = pow(v * (uNits / 10000.0), m1);
      v = (c1 + c2*v) / (1.0 + c3*v);
      return pow(v, m2);
  }
  float l2hlg(float v) {
      const float a=0.17883277, b=0.28466892, c=0.55991073;
      return v <= 1.0/12.0 ? sqrt(3.0*v) : a*log(12.0*v - b) + c;
  }
  vec3 hdr_encode(vec3 c) {
      if (uMode == 0) return c;
      c = s2l(c);
      if (uMode == 1) return c;
      if (uMode == 2) return c * (uNits / 80.0);
      c = to2020(c);
      if (uMode == 3) return vec3(l2pq(c.r), l2pq(c.g), l2pq(c.b));
      return vec3(l2hlg(c.r), l2hlg(c.g), l2hlg(c.b));
  }
  void main() {
      vec4 c = In.Color * texture(sTexture, In.UV.st);
      c.rgb = hdr_encode(c.rgb);
      fColor = c;
  }
  ```

- [ ] **Step 2: Generate the SPIR-V header.**
  ```bash
  glslangValidator -V --vn choir_overlay_hdr_frag_spv \
    src/layer/shaders/overlay_hdr.frag -o src/layer/shaders/overlay_hdr.frag.spv.h
  ```
  Expected: exit 0, file created containing `static const uint32_t choir_overlay_hdr_frag_spv[] = { ... };`

- [ ] **Step 3: Commit.**
  ```bash
  git add src/layer/shaders/overlay_hdr.frag src/layer/shaders/overlay_hdr.frag.spv.h
  git commit -m "feat(layer): HDR overlay fragment shader (transfer via specialization constants)"
  ```

---

### Task 3: Unity-TU HDR pipeline helper

**Goal:** Two helper functions, defined in OUR unity TU (no vendored edits), that reuse ImGui's vertex shader + pipeline layout to build a custom HDR pipeline and expose it to the renderer.

**Files:**
- Create: `src/layer/imgui_vk_backend.hpp`
- Modify: `src/layer/imgui_impl_vulkan_unity.cpp`

**Acceptance Criteria:**
- [ ] `choir::create_hdr_pipeline(...)` returns a non-null `VkPipeline` for a valid render pass after the ImGui backend is initialized.
- [ ] No edits to any file under `subprojects/`.

**Verify:** `meson compile -C build` → links clean (helper exercised by Task 4's golden test).

**Steps:**

- [ ] **Step 1: Header** `src/layer/imgui_vk_backend.hpp`:
  ```cpp
  // Choir helpers layered on the vendored ImGui Vulkan backend. Defined in
  // imgui_impl_vulkan_unity.cpp (which #includes imgui_impl_vulkan.cpp, giving these
  // access to the backend's static internals — bd->ShaderModuleVert, bd->PipelineLayout —
  // without modifying vendored code).
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
  ```

- [ ] **Step 2: Implementation.** Append to `src/layer/imgui_impl_vulkan_unity.cpp` (AFTER its existing `#include "imgui_impl_vulkan.cpp"` line, so the static `ImGui_ImplVulkan_GetBackendData`, `ImGui_ImplVulkan_CreateShaderModules`, and `ImDrawVert` are in scope). Replicates `ImGui_ImplVulkan_CreatePipeline` (imgui_impl_vulkan.cpp:923) but swaps the fragment stage + adds specialization:
  ```cpp
  #include "imgui_vk_backend.hpp"
  namespace choir {
  VkPipeline create_hdr_pipeline(VkRenderPass render_pass, const uint32_t* frag_spv,
                                 size_t frag_spv_size_bytes, int mode, float nits) {
      ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
      if (!bd || render_pass == VK_NULL_HANDLE) return VK_NULL_HANDLE;
      ImGui_ImplVulkan_InitInfo* v = &bd->VulkanInitInfo;
      const VkAllocationCallbacks* alloc = v->Allocator;
      // Ensure ImGui's vertex shader module exists.
      ImGui_ImplVulkan_CreateShaderModules(v->Device, alloc);
      if (!bd->ShaderModuleVert || !bd->PipelineLayout) return VK_NULL_HANDLE;

      VkShaderModule frag = VK_NULL_HANDLE;
      VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      smci.codeSize = frag_spv_size_bytes;
      smci.pCode = frag_spv;
      if (vkCreateShaderModule(v->Device, &smci, alloc, &frag) != VK_SUCCESS) return VK_NULL_HANDLE;

      // Specialization constants: 0 = int mode, 1 = float nits.
      struct Spec { int32_t mode; float nits; } spec{mode, nits};
      VkSpecializationMapEntry entries[2] = {
          {0, offsetof(Spec, mode), sizeof(int32_t)},
          {1, offsetof(Spec, nits), sizeof(float)},
      };
      VkSpecializationInfo si{2, entries, sizeof(Spec), &spec};

      VkPipelineShaderStageCreateInfo stage[2] = {};
      stage[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stage[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
      stage[0].module = bd->ShaderModuleVert; stage[0].pName = "main";
      stage[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stage[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      stage[1].module = frag; stage[1].pName = "main"; stage[1].pSpecializationInfo = &si;

      VkVertexInputBindingDescription bind{0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX};
      VkVertexInputAttributeDescription attr[3] = {
          {0, 0, VK_FORMAT_R32G32_SFLOAT,  offsetof(ImDrawVert, pos)},
          {1, 0, VK_FORMAT_R32G32_SFLOAT,  offsetof(ImDrawVert, uv)},
          {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col)},
      };
      VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &bind;
      vin.vertexAttributeDescriptionCount = 3; vin.pVertexAttributeDescriptions = attr;

      VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
      ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
      vp.viewportCount = 1; vp.scissorCount = 1;
      VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
      rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
      rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
      VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
      ms.rasterizationSamples = (v->MSAASamples != 0) ? v->MSAASamples : VK_SAMPLE_COUNT_1_BIT;
      VkPipelineColorBlendAttachmentState cba{};
      cba.blendEnable = VK_TRUE;
      cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      cba.colorBlendOp = VK_BLEND_OP_ADD;
      cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      cba.alphaBlendOp = VK_BLEND_OP_ADD;
      cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
      VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      cb.attachmentCount = 1; cb.pAttachments = &cba;
      VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
      VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
      dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;

      VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      ci.flags = bd->PipelineCreateFlags;
      ci.stageCount = 2; ci.pStages = stage;
      ci.pVertexInputState = &vin; ci.pInputAssemblyState = &ia;
      ci.pViewportState = &vp; ci.pRasterizationState = &rs;
      ci.pMultisampleState = &ms; ci.pDepthStencilState = &ds;
      ci.pColorBlendState = &cb; ci.pDynamicState = &dy;
      ci.layout = bd->PipelineLayout; ci.renderPass = render_pass; ci.subpass = 0;

      VkPipeline pipe = VK_NULL_HANDLE;
      VkResult r = vkCreateGraphicsPipelines(v->Device, v->PipelineCache, 1, &ci, alloc, &pipe);
      vkDestroyShaderModule(v->Device, frag, alloc);
      return (r == VK_SUCCESS) ? pipe : VK_NULL_HANDLE;
  }
  }  // namespace choir
  ```
  Note: this TU is compiled with `VK_NO_PROTOTYPES`; the `vkCreate*`/`vkDestroy*` symbols resolve through the same loader the backend uses (the unity TU already relies on this — confirm by checking the existing `#include`/dispatch setup at the top of `imgui_impl_vulkan_unity.cpp` and follow the same pattern; if the file thunks Vulkan calls via a dispatch table, route these through it identically).

- [ ] **Step 2b (if VK_NO_PROTOTYPES blocks direct vk* calls):** read the top of `src/layer/imgui_impl_vulkan_unity.cpp` to see how it provides Vulkan entry points to the backend, and call `vkCreateShaderModule` / `vkCreateGraphicsPipelines` / `vkDestroyShaderModule` through the same mechanism (e.g. `bd->VulkanInitInfo`-stored loader or a file-local dispatch). Do not introduce a second loader.

- [ ] **Step 3: Build.** `meson compile -C build` → expect clean compile/link.

- [ ] **Step 4: Commit.**
  ```bash
  git add src/layer/imgui_vk_backend.hpp src/layer/imgui_impl_vulkan_unity.cpp
  git commit -m "feat(layer): unity-TU helper to build a custom HDR ImGui pipeline"
  ```

---

### Task 4: Renderer — create/use/destroy the HDR pipeline

**Goal:** `ImguiRenderer::init` takes `(TransferFunction transfer, float nits)`, builds the HDR pipeline when transfer ≠ None, draws with it via `RenderDrawData(..., hdr_pipeline_)`, removes the CPU per-vertex transform, and destroys the pipeline on shutdown.

**Files:**
- Modify: `src/layer/imgui_renderer.hpp`
- Modify: `src/layer/imgui_renderer.cpp`

**Acceptance Criteria:**
- [ ] `init(...)` signature ends `..., TransferFunction transfer, float nits, PFN_vkGetInstanceProcAddr gipa, const DeviceDispatch& disp)`.
- [ ] When `transfer != None`, `hdr_pipeline_` is created via `choir::create_hdr_pipeline(render_pass, choir_overlay_hdr_frag_spv, sizeof(choir_overlay_hdr_frag_spv), int(transfer), nits)`.
- [ ] `end_frame` calls `ImGui_ImplVulkan_RenderDrawData(draw_data, cmd, hdr_pipeline_)` when `hdr_pipeline_ != VK_NULL_HANDLE`, else the 2-arg form.
- [ ] The old `apply_transfer_u8` vertex loop is removed.
- [ ] `shutdown()` destroys `hdr_pipeline_`.

**Verify:** `meson compile -C build` → clean (golden test run in Task 6).

**Steps:**

- [ ] **Step 1: Header.** In `src/layer/imgui_renderer.hpp`: include `"imgui_vk_backend.hpp"` is NOT needed here; keep `#include "swapchain_color.hpp"`. Change the `init` declaration to take `TransferFunction transfer, float nits` (replace the single `TransferFunction transfer` added earlier — it now also takes nits). Replace the member `TransferFunction transfer_` with both:
  ```cpp
  TransferFunction transfer_ = TransferFunction::None;
  float nits_ = 200.0f;
  VkPipeline hdr_pipeline_ = VK_NULL_HANDLE;
  ```

- [ ] **Step 2: init.** In `src/layer/imgui_renderer.cpp`:
  - Add includes: `#include "imgui_vk_backend.hpp"` and `#include "shaders/overlay_hdr.frag.spv.h"`.
  - Change the signature to `... uint32_t api_version, TransferFunction transfer, float nits, PFN_vkGetInstanceProcAddr gipa, const DeviceDispatch& disp)`.
  - At the top set `transfer_ = transfer; nits_ = nits;`.
  - After the ImGui backend is fully initialized (after `ImGui_ImplVulkan_Init` succeeds and before `init_done_ = true`), add:
    ```cpp
    if (transfer_ != TransferFunction::None) {
        hdr_pipeline_ = choir::create_hdr_pipeline(
            render_pass, choir_overlay_hdr_frag_spv, sizeof(choir_overlay_hdr_frag_spv),
            static_cast<int>(transfer_), nits_);
        if (hdr_pipeline_ == VK_NULL_HANDLE)
            std::fprintf(stderr, "[choir] HDR pipeline creation failed; overlay color may be off\n");
    }
    ```
    (`render_pass` is the `VkRenderPass` parameter already passed to `init`.)

- [ ] **Step 3: end_frame.** Replace the existing `if (transfer_ != TransferFunction::None) { ...per-vertex apply_transfer_u8 loop... } ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);` block with:
  ```cpp
  if (hdr_pipeline_ != VK_NULL_HANDLE)
      ImGui_ImplVulkan_RenderDrawData(draw_data, cmd, hdr_pipeline_);
  else
      ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
  ```
  Remove the now-unused `#include "swapchain_color.hpp"`-based `apply_transfer_u8` usage (the include stays for `TransferFunction`).

- [ ] **Step 4: shutdown.** Where the renderer tears down its Vulkan objects (the `shutdown()` body), before destroying the device-owned descriptor pool / backend, add:
  ```cpp
  if (hdr_pipeline_ != VK_NULL_HANDLE) {
      disp_.DestroyPipeline(device_, hdr_pipeline_, nullptr);  // use the renderer's stored device + dispatch
      hdr_pipeline_ = VK_NULL_HANDLE;
  }
  ```
  Check how the renderer stores its device + `DeviceDispatch` (member names) and match them; if it calls ImGui teardown that idles/clears, destroy the pipeline BEFORE `ImGui_ImplVulkan_Shutdown`.

- [ ] **Step 5: Build.** `meson compile -C build` → clean.

- [ ] **Step 6: Commit.**
  ```bash
  git add src/layer/imgui_renderer.hpp src/layer/imgui_renderer.cpp
  git commit -m "feat(layer): draw overlay through the HDR pipeline; drop CPU vertex transform"
  ```

---

### Task 5: Avatars always UNORM

**Goal:** Avatar textures are always `R8G8B8A8_UNORM` (raw sRGB bytes); the `srgb` parameter and `_SRGB` branching are removed, so the shader handles their transfer uniformly.

**Files:**
- Modify: `src/layer/avatar_textures.hpp`
- Modify: `src/layer/avatar_textures.cpp`

**Acceptance Criteria:**
- [ ] `AvatarTextures::init(ImguiRenderer*)` no longer takes a `bool srgb`.
- [ ] `create_texture` uses `VK_FORMAT_R8G8B8A8_UNORM` for both image + view (no conditional).
- [ ] The `srgb_` member is removed.

**Verify:** `meson compile -C build` → clean (golden avatar checks in Task 6).

**Steps:**

- [ ] **Step 1: Header.** In `src/layer/avatar_textures.hpp`: change `void init(ImguiRenderer* renderer, bool srgb);` to `void init(ImguiRenderer* renderer);` and delete the `bool srgb_ = false;` member + its doc comment.

- [ ] **Step 2: Source.** In `src/layer/avatar_textures.cpp`:
  - `init`: drop the `srgb` param and the `srgb_ = srgb;` assignment.
  - In `create_texture`, set both formats unconditionally:
    ```cpp
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ...
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ```

- [ ] **Step 3: Build.** `meson compile -C build` (will fail at the `avatars->init(...)` call site in swapchain.cpp until Task 6 — that's expected; this task's own files compile. If building the whole layer, do Task 5 + 6 back-to-back before `meson compile`.)

- [ ] **Step 4: Commit.**
  ```bash
  git add src/layer/avatar_textures.hpp src/layer/avatar_textures.cpp
  git commit -m "feat(layer): avatars always UNORM (shader handles transfer)"
  ```

---

### Task 6: Swapchain wiring + nits env + full suite

**Goal:** `swapchain.cpp` computes the transfer function, reads `CHOIR_HDR_NITS` (default 200, clamped [80,1000]), passes `(transfer, nits)` to `imgui->init` and calls `avatars->init(renderer)`; the debug log shows transfer + nits; the full test suite passes.

**Files:**
- Modify: `src/layer/swapchain.cpp`

**Acceptance Criteria:**
- [ ] `imgui->init(...)` is called with `transfer_function_for(s.format, s.color_space)` and the resolved nits.
- [ ] `avatars->init(s.imgui.get())` (no bool).
- [ ] `CHOIR_HDR_NITS` parsed once, clamped to [80, 1000], default 200.
- [ ] `CHOIR_DEBUG_FORMAT` log prints `transfer=<name> nits=<n>`.
- [ ] `meson test -C build` → all green.

**Verify:** `meson test -C build` → `Fail: 0`

**Steps:**

- [ ] **Step 1: nits helper.** Near the top of `swapchain.cpp`'s anonymous namespace add:
  ```cpp
  float hdr_nits() {
      float n = 200.0f;
      if (const char* e = ::getenv("CHOIR_HDR_NITS"); e && *e) {
          float v = std::strtof(e, nullptr);
          if (v >= 80.0f && v <= 1000.0f) n = v;
      }
      return n;
  }
  ```
  Ensure `#include <cstdlib>` is present (it is).

- [ ] **Step 2: imgui->init call.** Replace the `transfer_function_for(s.format, s.color_space)` argument block so it passes BOTH transfer and nits:
  ```cpp
  s.dd->api_version,
  transfer_function_for(s.format, s.color_space), hdr_nits(), s.dd->instance_gipa,
  d)) {
  ```

- [ ] **Step 3: avatars->init call.** Replace:
  ```cpp
  s.avatars->init(s.imgui.get());
  ```
  (delete the `transfer_function_for(...) != None` boolean argument).

- [ ] **Step 4: debug log.** Update the `CHOIR_DEBUG_FORMAT` block's format string + args:
  ```cpp
  std::fprintf(stderr,
               "[choir] swapchain format=%d colorspace=%d (%s) -> transfer=%s nits=%.0f\n",
               static_cast<int>(s.format), static_cast<int>(s.color_space),
               colorspace_name(s.color_space),
               transfer_name(transfer_function_for(s.format, s.color_space)), hdr_nits());
  ```

- [ ] **Step 5: Build + full suite.** `meson compile -C build && meson test -C build` → expect `Ok: 22  Fail: 0`. The golden test's `_SRGB` Phase 3 now drives the HDR pipeline (mode=Srgb) with UNORM avatars on the real GPU; it must still pass (avatars + colors read back correctly).

- [ ] **Step 6: Commit.**
  ```bash
  git add src/layer/swapchain.cpp
  git commit -m "feat(layer): wire HDR transfer + CHOIR_HDR_NITS into the swapchain"
  ```

---

### Task 7: Reinstall + manual HDR verification

**Goal:** Install the rebuilt layer and confirm in real HDR games that the overlay has correct color AND readable brightness.

**USER-ORDERED GATE — NON-SKIPPABLE.** This task was requested by the user in the current conversation. It MUST NOT be closed by walking around it, by declaring it "verified inline", or by substituting a cheaper check. Close only after every item in `acceptanceCriteria` has been re-validated independently, with output captured.

**Files:** none (install + manual test)

**Acceptance Criteria:**
- [ ] `bash packaging/install-user.sh` reports installed.
- [ ] Overwatch in HDR (`CHOIR_DEBUG_FORMAT=1`): log shows `transfer=scrgb`; the overlay is NOT washed out and is bright/readable over the game.
- [ ] Cyberpunk 2077 in HDR: log shows `transfer=pq`; the overlay is correctly saturated (not over-saturated) and readable.
- [ ] `CHOIR_HDR_NITS=<value>` visibly changes overlay brightness.

**Verify:** user launches both games in HDR, inspects the overlay + `~/choir.log`, and confirms the criteria.

**Steps:**

- [ ] **Step 1: Reinstall.** `bash packaging/install-user.sh` → "Choir installed."
- [ ] **Step 2: Overwatch (scRGB).** Launch in HDR with `CHOIR_DEBUG_FORMAT=1 %command% 2>$HOME/choir.log`; confirm `transfer=scrgb` and the overlay reads correctly/brightly.
- [ ] **Step 3: Cyberpunk (PQ).** Same; confirm `transfer=pq` and correct saturation.
- [ ] **Step 4: Tune.** Try `CHOIR_HDR_NITS=300` and `=120` to confirm the brightness knob; settle on a default.

---

## Self-Review

- **Spec coverage:** color model (T1), shaders (T2), unity helper/layout reuse (T3), renderer pipeline lifecycle + RenderDrawData override + remove CPU transform (T4), avatars UNORM (T5), swapchain wiring + nits + log (T6), tests (T1 unit + T6 golden), manual HDR check (T7). All spec sections mapped.
- **Placeholder scan:** Task 3 Step 2b is a conditional investigation (VK_NO_PROTOTYPES dispatch), not a placeholder — it names the exact file + mechanism to follow. Task 4 Steps 4 references "member names" to match — the engineer reads the existing `shutdown()`; acceptable (the renderer's device/dispatch members are pre-existing).
- **Type consistency:** `TransferFunction` (T1) used in T4/T6; `create_hdr_pipeline` signature identical in T3 header/impl and T4 call; `choir_overlay_hdr_frag_spv` symbol name matches `--vn` in T2 and the use in T4.
