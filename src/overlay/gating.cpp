#include "gating.hpp"

#include <cstdlib>
#include <cstring>

namespace choir {

bool overlay_disabled() {
    // Read the env var exactly once. The result is process-lifetime stable;
    // changing the variable after the layer loads has no effect (matching the
    // loader's own one-shot read of disable_environment).
    static const bool disabled = [] {
        const char* v = std::getenv("DISABLE_CHOIR_OVERLAY");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return disabled;
}

}  // namespace choir
