// State-client + avatar-textures integration test for the Choir layer (Task 16).
//
// Drives the real layer end-to-end against fake_host:
//   1. Sets a temp XDG_RUNTIME_DIR + cache dir, starts fake_host (background) serving
//      its scripted 3-participant snapshot ("Test Voice"; Bob speaking; Carol
//      self_mute) + 3 AvatarReady frames.
//   2. Runs vk_min_present with the Choir layer forced on, the same XDG_RUNTIME_DIR,
//      and CHOIR_DEBUG_DUMP=<tmp>/snap.json so the layer's state client writes the
//      first received snapshot to disk.
//   3. Asserts vk_min_present exited 0 and snap.json shows in_voice=true, channel
//      "Test Voice", 3 participants, Bob speaking, Carol self_mute — proving the layer
//      connected, sent Hello, received + parsed the Snapshot, and (by the same path)
//      enqueued the avatars for the render thread to upload.
//   4. Tears fake_host down, then runs vk_min_present WITHOUT a host (no socket) and
//      asserts it still exits 0 (no crash / no hang).
//
// vk_min_present exits 77 when no headless surface / present-capable device is
// available; this test propagates that as SKIP.
//
// Invocation (from meson): test_layer_state <vk_min_present> <fake_host> <layer_dir>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace {

std::string g_tmpdir;

// Run a child program with argv (NULL-terminated) and the given environment additions
// applied to the CURRENT environment (so the child inherits everything + overrides).
// Returns the child's exit code, or -1 on spawn failure. `extra_env` is "K=V" strings.
int run_child(const char* path, const char* const argv[],
              const std::vector<std::string>& extra_env) {
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        for (const std::string& kv : extra_env) {
            auto eq = kv.find('=');
            if (eq != std::string::npos)
                ::setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
        }
        ::execv(path, const_cast<char* const*>(argv));
        ::_exit(127);  // exec failed
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Spawn fake_host in the background; returns its pid (or -1). fake_host binds the
// abstract socket named by $CHOIR_SOCKET (inherited from our environment).
pid_t spawn_fake_host(const char* path, const std::string& cache) {
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        const char* argv[] = {path, "--cache-dir", cache.c_str(), nullptr};
        ::execv(path, const_cast<char* const*>(argv));
        ::_exit(127);
    }
    return pid;
}

void kill_and_reap(pid_t pid) {
    if (pid <= 0) return;
    ::kill(pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 50; ++i) {
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid, SIGKILL);
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

bool file_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <vk_min_present> <fake_host> <layer_dir>\n", argv[0]);
        return 2;
    }
    const std::string vk_app = argv[1];
    const std::string fake_host = argv[2];
    const std::string layer_dir = argv[3];

    // --- Temp dir for the socket + avatar cache + debug dump. ---
    char tmpl[] = "/tmp/choir_layer_state_XXXXXX";
    char* d = ::mkdtemp(tmpl);
    if (!d) {
        std::fprintf(stderr, "test_layer_state: mkdtemp failed\n");
        return 2;
    }
    g_tmpdir = d;
    const std::string cache = g_tmpdir + "/avatars";
    const std::string dump = g_tmpdir + "/snap.json";
    ::mkdir(cache.c_str(), 0700);

    // Abstract socket name, unique per test run. Both fake_host (inherits our env)
    // and the layer inside vk_min_present read it from $CHOIR_SOCKET. (Abstract
    // sockets live in the netns, not the filesystem — no path needed.)
    const std::string sockname = "choir-test-ls-" + std::to_string(::getpid());
    ::setenv("CHOIR_SOCKET", sockname.c_str(), 1);

    // Common loader env so vk_min_present loads our layer. CHOIR_SOCKET is inherited
    // from our environment (set above), so the layer connects to the same abstract
    // socket fake_host binds.
    const std::vector<std::string> layer_env = {
        "VK_LAYER_PATH=" + layer_dir,
        "VK_INSTANCE_LAYERS=VK_LAYER_choir_overlay_x86_64",
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_choir_overlay_x86_64",
    };

    int rc = 0;

    // =========================================================================
    // Phase 1: with fake_host running, the layer must receive the snapshot.
    // =========================================================================
    pid_t host = spawn_fake_host(fake_host.c_str(), cache);
    if (host < 0) {
        std::fprintf(stderr, "test_layer_state: failed to spawn fake_host\n");
        return 2;
    }
    // Give fake_host a moment to bind + listen before the layer connects. The layer
    // also retries with backoff, so this is just to avoid the first reconnect wait.
    // (Abstract socket has no file to poll, so wait a fixed short interval.)
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        // A few frames so the client thread has time to connect, Hello, and deliver
        // the snapshot, and so the render thread drains + uploads the avatars.
        const std::string frames = "30";
        std::vector<std::string> env = layer_env;
        env.push_back("CHOIR_DEBUG_DUMP=" + dump);
        env.push_back("DISABLE_CHOIR_OVERLAY=");  // ensure not disabled
        const char* a[] = {vk_app.c_str(), "--frames", frames.c_str(), nullptr};
        int code = run_child(vk_app.c_str(), a, env);

        if (code == 77) {
            std::fprintf(stderr, "test_layer_state: vk_min_present skipped (no headless) — SKIP\n");
            kill_and_reap(host);
            return 77;
        }
        if (code != 0) {
            std::fprintf(stderr, "test_layer_state: vk_min_present (with host) exited %d\n", code);
            rc = 1;
        }
    }

    kill_and_reap(host);

    // The dump may lag the present loop slightly (it is written by the client thread
    // on the first Snapshot). Poll briefly for it.
    for (int i = 0; i < 50 && !file_exists(dump); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    if (!file_exists(dump)) {
        std::fprintf(stderr, "test_layer_state: FAIL — debug dump %s was never written "
                             "(layer did not receive a snapshot)\n", dump.c_str());
        rc = 1;
    } else {
        const std::string content = read_file(dump);
        std::printf("test_layer_state: CHOIR_DEBUG_DUMP snapshot JSON:\n%s\n", content.c_str());
        try {
            auto j = nlohmann::json::parse(content);
            bool ok = true;
            auto check = [&](bool cond, const char* msg) {
                if (!cond) { std::fprintf(stderr, "test_layer_state: FAIL — %s\n", msg); ok = false; }
            };
            check(j.value("in_voice", false), "in_voice != true");
            check(j.value("channel_name", std::string{}) == "Test Voice", "channel_name != 'Test Voice'");
            const auto& ps = j["participants"];
            check(ps.is_array() && ps.size() == 3, "participants count != 3");
            if (ps.is_array() && ps.size() == 3) {
                check(ps[0].value("display_name", std::string{}) == "Alice", "p0 != Alice");
                check(ps[1].value("display_name", std::string{}) == "Bob", "p1 != Bob");
                check(ps[1].value("speaking", false), "Bob not speaking");
                check(ps[2].value("display_name", std::string{}) == "Carol", "p2 != Carol");
                check(ps[2].value("self_mute", false), "Carol not self_mute");
            }
            if (!ok) rc = 1;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "test_layer_state: FAIL — dump JSON parse error: %s\n", e.what());
            rc = 1;
        }
    }

    // =========================================================================
    // Phase 2: WITHOUT a host (fake_host already killed above; abstract socket is
    // released on its exit). The layer must draw nothing and vk_min_present must
    // still exit 0 (no crash, no hang on shutdown).
    // =========================================================================
    {
        std::vector<std::string> env = layer_env;  // no host
        const char* a[] = {vk_app.c_str(), "--frames", "5", nullptr};
        int code = run_child(vk_app.c_str(), a, env);
        if (code == 77) {
            std::fprintf(stderr, "test_layer_state: no-host run skipped (no headless) — SKIP\n");
            // Phase 1 already ran; treat as overall skip only if phase 1 also skipped
            // (it didn't reach here if it had). Fall through reporting phase-1 result.
        } else if (code != 0) {
            std::fprintf(stderr, "test_layer_state: FAIL — vk_min_present WITHOUT host exited %d "
                                 "(expected 0)\n", code);
            rc = 1;
        } else {
            std::printf("test_layer_state: no-host run exited 0 (layer safe without a host)\n");
        }
    }

    if (rc == 0)
        std::puts("test_layer_state: PASS — layer received the scripted snapshot and "
                  "is safe with/without a host");
    return rc;
}
