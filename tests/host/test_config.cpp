// Tests for config.json load/save (Task 10).
//
// Config persists overlay appearance, Discord auth (mode + credentials/tokens),
// and the process denylist. Because it holds an access token, the saved file
// MUST be mode 0600. load() is defensive: an absent, corrupt, or partial file
// yields defaults (with the default denylist populated) and never throws.
//
// No Qt; std + nlohmann + POSIX stat only. Uses a unique temp path under /tmp.

#include "config/config.hpp"
#include "config/denylist.hpp"
#include "ipc/state.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>

using namespace choir;

namespace {

std::string temp_path() {
    // Unique per process so parallel meson tests don't collide.
    return "/tmp/choir_test_config_" + std::to_string(::getpid()) + ".json";
}

bool exists(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0;
}

}  // namespace

int main() {
    const std::string path = temp_path();
    ::unlink(path.c_str());  // ensure absent

    // --- absent file -> defaults, with default denylist populated --------
    {
        Config c = Config::load(path);
        assert(c.auth_mode == AuthMode::Streamkit);
        assert(c.client_id == "207646673902501888");
        assert(c.client_secret.empty());
        assert(c.access_token.empty());
        assert(c.refresh_token.empty());
        // Default denylist must be populated and contain the required names.
        assert(!c.denylist.empty());
        for (const char* req : {"Discord", "steam", "steamwebhelper", "gamescope",
                                "obs", "firefox", "chrome", "chromium"}) {
            assert(std::find(c.denylist.begin(), c.denylist.end(), std::string(req)) !=
                   c.denylist.end());
        }
        // Appearance defaults.
        assert(c.appearance.anchor == Anchor::TopRight);
    }

    // --- round-trip: appearance + auth + denylist ------------------------
    {
        Config c;
        c.appearance.anchor = Anchor::BottomLeft;
        c.appearance.scale = 1.5f;
        c.appearance.opacity = 0.42f;
        c.appearance.show_all_members = false;
        c.appearance.toast_anchor = Anchor::BottomRight;
        c.appearance.toast_duration_ms = 8000;
        c.auth_mode = AuthMode::OwnApp;
        c.client_id = "my-client-id";
        c.client_secret = "my-secret";
        c.access_token = "access-tok";
        c.refresh_token = "refresh-tok";
        c.denylist = {"foo", "bar", "*baz*"};

        assert(c.save(path));
        assert(exists(path));

        Config r = Config::load(path);
        assert(r.appearance.anchor == Anchor::BottomLeft);
        assert(r.appearance.scale == 1.5f);
        assert(r.appearance.opacity == 0.42f);
        assert(r.appearance.show_all_members == false);
        assert(r.appearance.toast_anchor == Anchor::BottomRight);
        assert(r.appearance.toast_duration_ms == 8000);
        assert(r.auth_mode == AuthMode::OwnApp);
        assert(r.client_id == "my-client-id");
        assert(r.client_secret == "my-secret");
        assert(r.access_token == "access-tok");
        assert(r.refresh_token == "refresh-tok");
        assert((r.denylist == std::vector<std::string>{"foo", "bar", "*baz*"}));
    }

    // --- saved file mode is exactly 0600 ---------------------------------
    {
        struct stat st {};
        assert(::stat(path.c_str(), &st) == 0);
        assert((st.st_mode & 0777) == 0600);
    }

    // --- 0600 is enforced even if the file pre-existed with looser perms --
    {
        ::chmod(path.c_str(), 0644);
        Config c;
        assert(c.save(path));
        struct stat st {};
        assert(::stat(path.c_str(), &st) == 0);
        assert((st.st_mode & 0777) == 0600);
    }

    // --- auth_mode string round-trip ("streamkit" / "own-app") -----------
    {
        Config a;
        a.auth_mode = AuthMode::Streamkit;
        assert(a.save(path));
        assert(Config::load(path).auth_mode == AuthMode::Streamkit);

        Config b;
        b.auth_mode = AuthMode::OwnApp;
        assert(b.save(path));
        assert(Config::load(path).auth_mode == AuthMode::OwnApp);
    }

    // --- partial / corrupt file defensiveness ----------------------------
    {
        // A present-but-partial JSON object: only client_id set. Missing fields
        // must fall back to defaults; denylist (absent) must be the defaults.
        FILE* f = std::fopen(path.c_str(), "w");
        assert(f);
        std::fputs("{\"client_id\":\"partial-id\"}", f);
        std::fclose(f);

        Config c = Config::load(path);
        assert(c.client_id == "partial-id");        // present field honored
        assert(c.auth_mode == AuthMode::Streamkit);  // default
        assert(c.client_secret.empty());             // default
        assert(c.appearance.anchor == Anchor::TopRight);  // default
        assert(!c.denylist.empty());                 // absent -> default denylist
    }
    {
        // Garbage that doesn't parse -> full defaults, no throw.
        FILE* f = std::fopen(path.c_str(), "w");
        assert(f);
        std::fputs("not json at all {{{", f);
        std::fclose(f);

        Config c = Config::load(path);
        assert(c.auth_mode == AuthMode::Streamkit);
        assert(!c.denylist.empty());
    }
    {
        // Unknown auth_mode string -> default to Streamkit.
        FILE* f = std::fopen(path.c_str(), "w");
        assert(f);
        std::fputs("{\"auth_mode\":\"bogus\"}", f);
        std::fclose(f);

        Config c = Config::load(path);
        assert(c.auth_mode == AuthMode::Streamkit);
    }

    // --- save creates a missing parent directory -------------------------
    {
        const std::string dir = "/tmp/choir_test_cfgdir_" + std::to_string(::getpid());
        const std::string nested = dir + "/sub/config.json";
        // Best-effort cleanup of a prior run.
        ::unlink(nested.c_str());
        ::rmdir((dir + "/sub").c_str());
        ::rmdir(dir.c_str());

        Config c;
        assert(c.save(nested));
        assert(exists(nested));

        // cleanup
        ::unlink(nested.c_str());
        ::rmdir((dir + "/sub").c_str());
        ::rmdir(dir.c_str());
    }

    ::unlink(path.c_str());
    return 0;
}
