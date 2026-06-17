// Overlay gating: the env-var kill switch for the Choir layer.
//
// The implicit-layer manifest declares `disable_environment`
// {"DISABLE_CHOIR_OVERLAY": "1"}, which makes the Vulkan loader skip loading
// the layer entirely when that variable is set. But a user may also force the
// layer on (VK_LAYER_PATH + VK_INSTANCE_LAYERS / VK_LOADER_LAYERS_ENABLE) while
// still wanting it inert, so we re-check the variable inside the layer: when set,
// we still wire the dispatch chain and forward every call, but never do any
// overlay work. For the skeleton there is no overlay work yet; later tasks read
// this flag before initializing overlay state / rendering.
#pragma once

namespace choir {

// True when DISABLE_CHOIR_OVERLAY == "1". Read once (cached) on first call.
bool overlay_disabled();

}  // namespace choir
