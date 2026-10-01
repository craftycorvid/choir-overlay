// Tests for the process-name denylist (Task 10).
//
// Choir uses the "global with denylist" activation model: the overlay is active
// on every Vulkan app EXCEPT processes whose basename matches a denylist
// pattern. Matching is case-insensitive fnmatch of each pattern against the
// basename of the exe path. No Qt, no I/O.

#include "config/denylist.hpp"

#include <algorithm>
#include <cassert>
#include <string>
#include <vector>

using namespace choir;

namespace {

bool defaults_contain(const std::string& name) {
    auto d = Denylist::defaults();
    return std::find(d.begin(), d.end(), name) != d.end();
}

}  // namespace

int main() {
    // --- defaults() includes the required names ---------------------------
    for (const char* required :
         {"Discord", "steam", "steamwebhelper", "gamescope", "obs", "firefox",
          "chrome", "chromium", "faugus*", "protontricks*"}) {
        assert(defaults_contain(required) && "default denylist missing a required name");
    }

    Denylist d(Denylist::defaults());

    // --- exact matches (case-insensitive) --------------------------------
    assert(d.blocks("steamwebhelper"));
    assert(d.blocks("steam"));
    assert(d.blocks("gamescope"));
    // "Discord" stored, matches lowercase exe "discord" because both sides are
    // lowercased before fnmatch.
    assert(d.blocks("discord"));
    assert(d.blocks("Discord"));
    assert(d.blocks("DISCORD"));

    // --- basename is used (path stripped) --------------------------------
    assert(d.blocks("/usr/bin/steam"));
    assert(d.blocks("/some/deep/path/firefox"));

    // --- prefix tooling: matched via comm or via an argv entry ------------
    // The server calls blocks() for the client's comm and for each argv entry,
    // so these are the strings it actually sees.
    assert(d.blocks("protontricks"));
    assert(d.blocks("/usr/bin/protontricks"));  // argv[0]
    assert(d.blocks("protontricks-la"));       // comm truncates at 15 chars
    assert(d.blocks("faugus.launcher"));       // `python3 -m faugus.launcher`
    assert(d.blocks("faugus.runner"));
    assert(d.blocks("faugus-launcher"));

    // --- a real game is NOT blocked --------------------------------------
    assert(!d.blocks("MyGame.exe"));
    assert(!d.blocks("/usr/games/MyGame.exe"));
    assert(!d.blocks("xonotic"));
    // ...not even when a *directory* in its argv path is denylisted: only the
    // basename of each argument is matched.
    assert(!d.blocks("/home/me/Games/faugus/MyGame.exe"));
    assert(!d.blocks("/home/me/SomeLauncher/MyGame.exe"));

    // --- glob patterns match case-insensitively --------------------------
    Denylist g({"*launcher*"});
    assert(g.blocks("GameLauncher"));
    assert(g.blocks("gamelauncher"));
    assert(g.blocks("EpicGamesLauncher.exe"));  // contains "launcher"
    assert(g.blocks("epicgameslauncher"));
    assert(!g.blocks("MyGame"));

    // --- a plain pattern matches exactly, not as a substring -------------
    Denylist exact({"steam"});
    assert(exact.blocks("steam"));
    assert(!exact.blocks("steamwebhelper"));  // plain "steam" != "steamwebhelper"

    // --- empty patterns block nothing ------------------------------------
    Denylist none(std::vector<std::string>{});
    assert(!none.blocks("anything"));

    return 0;
}
