# HDR overlay shader — design

**Date:** 2026-06-20
**Status:** Approved (brainstorming) → ready for implementation plan

## Context

The Choir Vulkan layer renders an ImGui overlay (voice panel, name pills, text,
speaking rings, mute/deaf glyphs, avatars, notification toasts) into the game's
swapchain via a hooked `vkQueuePresentKHR`. On HDR swapchains the colors and brightness
are wrong:

- **Overwatch** (DX11/DXVK, HDR): swapchain is `R16G16B16A16_SFLOAT` +
  `VK_COLOR_SPACE_PASS_THROUGH_EXT` (scRGB-linear). The overlay is washed out.
- **Cyberpunk 2077** (HDR): `A2B10G10R10_UNORM_PACK32` + `VK_COLOR_SPACE_HDR10_ST2084_EXT`
  (PQ). Over-saturated before the MangoHud PQ port.

We first matched MangoHud exactly (commit 53dcf53): the color space selects a transfer
function applied to ImGui's vertex colors on the CPU. That fixes *saturation* for the
spaces MangoHud handles, but **cannot fix brightness**: ImGui's vertex colors and avatar
textures are 8-bit (clamp at 1.0), and HDR brightness needs values > 1.0 (scRGB white at
200 nits = linear 2.5). MangoHud has the same limitation and has no scRGB/PASS_THROUGH
or brightness handling at all — the overlay (icons included) sits at ~80 nits and looks
dim/soft over bright HDR scenes.

This feature adds **real HDR support beyond MangoHud**: correct color *and* a configurable
paper-white brightness, applied uniformly to the whole overlay (vertex shapes and avatar
images), by moving the color math into a fragment shader where HDR values can live.

## Goal / success criteria

- HDR overlay has correct saturation **and** readable brightness (default ~200 nits
  paper-white) over bright HDR content, for both scRGB (Overwatch) and HDR10/PQ
  (Cyberpunk).
- Icons/avatars get the same brightness treatment as the rest of the overlay.
- SDR (UNORM / `_SRGB`) rendering is unchanged in result.
- No crashes; failure-isolation preserved.

Non-goals: per-display HDR metadata (`VkHdrMetadataEXT`) reading; tone-mapping; correct
translucent blending in PQ-encoded space (a known, accepted limitation).

## Approach

**Custom fragment shader via ImGui's pipeline override.**
`ImGui_ImplVulkan_RenderDrawData(draw_data, cmd, pipeline)` takes an optional pipeline.
We create one custom pipeline per swapchain whose fragment shader does the HDR math on the
final fragment color. The transfer mode + nits are baked as **specialization constants**
(constant for the swapchain's lifetime), so no push-constant / descriptor-layout change is
needed and the pipeline stays layout-compatible with ImGui's. RenderDrawData keeps driving
the draws.

Rejected: (B) offscreen render + composite pass — more infrastructure, no benefit here;
(C) patching the vendored ImGui backend shader — churns vendored code on every update.

## Color model (`src/layer/swapchain_color.hpp`)

Replace the 4-value `TransferFunction { None, Srgb, Pq, Hlg }` with five modes so the shader
knows whether to apply the paper-white brightness scale:

```
enum class TransferFunction { None, Srgb, ScRgb, Pq, Hlg };
```

Re-add `is_hdr_float_format(VkFormat)` (R16G16B16A16_SFLOAT, R16G16B16_SFLOAT,
B10G11R11_UFLOAT_PACK32). `transfer_function_for(fmt, cs)`:

| format / color space | mode |
|---|---|
| `HDR10_ST2084` | `Pq` |
| `HDR10_HLG` | `Hlg` |
| `EXTENDED_SRGB_LINEAR` | `ScRgb` |
| `PASS_THROUGH` + `is_hdr_float_format` | `ScRgb` |
| `_SRGB` format | `Srgb` |
| else | `None` |

The CPU `apply_transfer*` helpers are no longer applied to vertices (the shader replaces
them), but the math (sRGB→linear, BT.709→BT.2020, PQ, HLG) stays in the header as the
single source of truth, mirrored by the shader and exercised by the unit test.

### Shader math per mode (final RGB, input is `In.Color * texture`, sRGB-authored 0..1)

- `None`: identity (uses ImGui's **default** pipeline; no custom pipeline created).
- `Srgb`: `sRGB→linear` (the `_SRGB` framebuffer re-encodes on store; white stays 1.0).
- `ScRgb`: `sRGB→linear × (nits/80)` (linear buffer; scRGB white 1.0 = 80 nits).
- `Pq`: `sRGB→linear`, `BT.709→BT.2020`, `LinearToPQ(targetL = nits)`.
- `Hlg`: `sRGB→linear`, `BT.709→BT.2020`, `LinearToHLG` (no absolute scale).

## Shaders (new, SPIR-V embedded in the layer)

- `overlay.vert`: matches ImGui's vertex shader — `aPos, aUV, aColor`; vec4 push constant
  (uScale.xy, uTranslate.xy) in the vertex stage; outputs interpolated color + uv.
- `overlay.frag`: `vec4 c = In.Color * texture(sTexture, In.UV); c.rgb = hdr_encode(c.rgb);
  fColor = c;` where `hdr_encode` switches on `layout(constant_id=0) const int uMode` and
  uses `layout(constant_id=1) const float uNits`.

Compiled to SPIR-V and embedded as `uint32_t` arrays, following the ImGui backend's own
embedded-SPIR-V convention. The `.spv.h` headers are **generated with glslangValidator
during development and checked in** (no build-time shader-compiler dependency, deterministic
installs) — the GLSL sources live alongside them in `src/layer/shaders/` for review and
regeneration. Alpha is passed through untouched.

## Renderer (`src/layer/imgui_renderer.{hpp,cpp}`)

- `init(...)` takes `TransferFunction transfer` and `float nits` (replacing `bool srgb`).
- After the ImGui backend is ready, if `transfer != None`, build the custom pipeline:
  - **Reuse ImGui's pipeline layout** — add a tiny accessor to the vendored backend
    (`VkPipelineLayout ImGui_ImplVulkan_GetPipelineLayout()` returning `bd->PipelineLayout`)
    so our pipeline is guaranteed compatible with the descriptor sets + push constants that
    `RenderDrawData` binds.
  - Replicate ImGui's pipeline state: `ImDrawVert` vertex input (pos@0, uv@8, col=UNORM8@16),
    triangle list, no cull, standard ImGui alpha blend (`SRC_ALPHA / ONE_MINUS_SRC_ALPHA`,
    alpha `ONE / ONE_MINUS_SRC_ALPHA`), dynamic viewport+scissor, against the swapchain
    render pass, with our two shaders + specialization `{mode, nits}`.
  - On pipeline-creation failure: leave `hdr_pipeline_ = VK_NULL_HANDLE`, log, and fall back
    to the default pipeline (overlay still renders, HDR color wrong but no crash).
- `end_frame`: `RenderDrawData(draw_data, cmd, hdr_pipeline_)` when `hdr_pipeline_` is set,
  else `RenderDrawData(draw_data, cmd)`.
- **Remove** the CPU per-vertex `apply_transfer_u8` block.
- `shutdown()` destroys `hdr_pipeline_`.

## Avatars (`src/layer/avatar_textures.{hpp,cpp}`)

- Drop the `srgb` parameter and the `_SRGB`-vs-UNORM branching. Avatars are **always
  `R8G8B8A8_UNORM`** (raw sRGB bytes). The shader (or, for `None`, ImGui's identity default)
  performs decode/encode, so icons receive the same transfer + brightness scaling as
  vertex-colored content.

## Swapchain wiring (`src/layer/swapchain.cpp`)

- Compute `TransferFunction` via `transfer_function_for(s.format, s.color_space)`.
- Read `nits` once: `CHOIR_HDR_NITS` env (default `200.0`), clamped to `[80, 1000]`.
- Pass `transfer` + `nits` to `imgui->init`; call `avatars->init(renderer)` (no srgb arg).
- `CHOIR_DEBUG_FORMAT` log: add the resolved `transfer=` mode and `nits=`.

## Data flow

`CreateSwapchainKHR` stores format + color space. First in-voice present lazily inits
`ImguiRenderer` with `transfer_function_for(...)` + nits; the renderer builds the custom HDR
pipeline (if mode ≠ None). Each present, `end_frame` records ImGui draw data with the custom
pipeline; the fragment shader applies the per-mode HDR transform to every fragment (vertex
shapes via the font-atlas white pixel, avatars via their UNORM texture).

## Testing

- **Unit** (`tests/layer/test_swapchain_color.cpp`): mode selection for every color space
  (incl. `PASS_THROUGH`+float → `ScRgb`, `ST2084` → `Pq`, plain UNORM → `None`) and the
  transfer math (sRGB→linear endpoints, ScRgb scale = nits/80, PQ/HLG endpoints).
- **Golden** (`tests/harness/test_layer_golden.cpp`, real GPU): existing SDR (`None`) and
  `_SRGB` (`Srgb`) phases must still pass. Phase 3 (`_SRGB`) now drives the **custom shader
  pipeline** and **UNORM avatars** end-to-end on hardware — validating the pipeline-override
  mechanism, layout compatibility, and avatar path. True HDR (scRGB/PQ) cannot be exercised
  on a headless surface; its math is covered by the unit test and the user's in-game check.
- **Manual**: user verifies Overwatch (scRGB) and Cyberpunk (PQ) in HDR look correct and are
  bright enough; `CHOIR_HDR_NITS` tunes brightness.

## Risks

- **Pipeline-layout compatibility** — mitigated by reusing ImGui's own layout via the
  accessor (no recreated descriptor-set layout / immutable-sampler mismatch).
- **Pipeline-state drift from ImGui** — replicate ImGui's `CreatePipeline` faithfully
  (blend, vertex input, dynamic state); the golden `_SRGB` phase catches gross mistakes.
- **SDR/_SRGB regression** — covered by the golden test on the real GPU.
- **Cannot test true HDR locally** — rely on unit tests for the math + user validation.

## Known limitation

Translucent blending into a PQ (non-linear) framebuffer happens in PQ space, so the
translucent name pills over HDR10 content blend slightly incorrectly — same as any overlay
in PQ. scRGB (linear) blends correctly.
