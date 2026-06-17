#pragma once

// Process-name denylist (Task 10).
//
// Choir uses the "global with denylist" activation model: the Vulkan overlay
// layer is loaded into every Vulkan app, and the host decides per-process
// whether to activate. A process is suppressed (sent `Disabled`) when its exe
// basename matches a denylist pattern.
//
// Matching is case-insensitive fnmatch of each pattern against the BASENAME of
// the exe (any leading path is stripped). Plain names ("steam") match exactly;
// globs ("*launcher*") match as you'd expect. Both sides are lowercased before
// matching, so a stored "Discord" pattern matches the exe "discord".
//
// Qt-free: std + <fnmatch.h> only.

#include <string>
#include <vector>

namespace choir {

class Denylist {
public:
    // Store patterns as-is (matching lowercases them on the fly).
    explicit Denylist(std::vector<std::string> patterns);

    // True if `exe_name`'s basename matches any pattern (case-insensitively).
    bool blocks(const std::string& exe_name) const;

    // The built-in default patterns (processes that are never games).
    static std::vector<std::string> defaults();

private:
    std::vector<std::string> patterns_;
};

}  // namespace choir
