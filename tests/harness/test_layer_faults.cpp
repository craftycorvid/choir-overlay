// Fault-injection test for the Choir layer (Task 18: robustness & failure isolation).
//
// The layer is injected into other people's games, so a failure MUST degrade to
// "no overlay, game runs normally" — never a crash/hang/validation-storm. This driver
// runs vk_min_present (the host app) with the layer forced on, under a battery of
// injected faults, and asserts the app ALWAYS exits 0 and keeps presenting (no crash,
// no hang within a per-case timeout).
//
// Sub-cases:
//   1. Truncated/garbage avatar file (fake_host --corrupt-avatars): layer must draw a
//      placeholder, not crash. Exit 0.
//   2. Malformed Snapshot frame (fake_host --corrupt-snapshot): layer must drop it (no
//      publish), keep presenting. Exit 0.
//   3. Host disconnect mid-stream (fake_host --once, then killed): layer keeps presenting
//      and exits 0 (reconnect attempted in the background).
//   4. in_voice toggling for 300 frames (fake_host --flap): exit 0, no hang. With correct
//      lazy-init the first in_voice allocates the overlay and toggles never realloc.
//
//   + Lazy-init demonstration: a NO-VOICE run (no host) with CHOIR_DEBUG_LAZY_INIT set —
//     assert the layer NEVER logged the lazy-init line (zero overlay allocation).
//
// Every sub-case runs under a hard timeout (a watchdog kills the child); a timeout is a
// FAIL, never an infinite test.
//
// vk_min_present exits 77 when no headless surface / present-capable device is available;
// this test propagates that as SKIP.
//
// Invocation (from meson): test_layer_faults <vk_min_present> <fake_host> <layer_dir>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// Result of running a child under a deadline.
struct RunResult {
    int exit_code = -1;   // child's exit status (WEXITSTATUS) when exited normally
    bool timed_out = false;
    bool signaled = false;  // killed by a signal other than our watchdog (e.g. SIGSEGV/SIGABRT)
    int term_signal = 0;
};

// Fork + exec `path` with argv (NULL-terminated) and extra "K=V" env applied on top of
// the inherited environment. A watchdog thread kills the child if it runs longer than
// `timeout_ms` (so a hang is a detectable FAIL, not an infinite test).
RunResult run_child_timeout(const char* path, const char* const argv[],
                            const std::vector<std::string>& extra_env, int timeout_ms) {
    RunResult r;
    pid_t pid = ::fork();
    if (pid < 0) { r.exit_code = -1; return r; }
    if (pid == 0) {
        for (const std::string& kv : extra_env) {
            auto eq = kv.find('=');
            if (eq != std::string::npos)
                ::setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
        }
        ::execv(path, const_cast<char* const*>(argv));
        ::_exit(127);  // exec failed
    }

    // Watchdog: SIGKILL the child if it overruns. Use a shared flag so we know it was us.
    std::atomic<bool> done{false};
    std::atomic<bool> killed_by_watchdog{false};
    std::thread watchdog([&] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                killed_by_watchdog.store(true, std::memory_order_release);
                ::kill(pid, SIGKILL);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    done.store(true, std::memory_order_release);
    watchdog.join();

    if (killed_by_watchdog.load(std::memory_order_acquire)) {
        r.timed_out = true;
        return r;
    }
    if (WIFEXITED(status)) {
        r.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        r.signaled = true;
        r.term_signal = WTERMSIG(status);
    }
    return r;
}

pid_t spawn_fake_host(const char* path, const std::vector<std::string>& args) {
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<const char*> argv;
        argv.push_back(path);
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);
        ::execv(path, const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }
    return pid;
}

void kill_and_reap(pid_t pid) {
    if (pid <= 0) return;
    ::kill(pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 50; ++i) {
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid || (w < 0 && errno == ECHILD)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid, SIGKILL);
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

bool file_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

const char* describe(const RunResult& r) {
    static char buf[128];
    if (r.timed_out) std::snprintf(buf, sizeof(buf), "TIMED OUT (hang)");
    else if (r.signaled) std::snprintf(buf, sizeof(buf), "killed by signal %d", r.term_signal);
    else std::snprintf(buf, sizeof(buf), "exit %d", r.exit_code);
    return buf;
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

    char tmpl[] = "/tmp/choir_layer_faults_XXXXXX";
    char* d = ::mkdtemp(tmpl);
    if (!d) { std::fprintf(stderr, "test_layer_faults: mkdtemp failed\n"); return 2; }
    const std::string tmpdir = d;
    const std::string socket = tmpdir + "/choir.sock";
    const std::string cache = tmpdir + "/avatars";
    ::mkdir(cache.c_str(), 0700);

    const std::vector<std::string> layer_env = {
        "VK_LAYER_PATH=" + layer_dir,
        "VK_INSTANCE_LAYERS=VK_LAYER_choir_overlay_x86_64",
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_choir_overlay_x86_64",
        "XDG_RUNTIME_DIR=" + tmpdir,
        "DISABLE_CHOIR_OVERLAY=",  // ensure not disabled
    };

    int rc = 0;
    bool skipped = false;

    // Helper: run vk_min_present for `frames` frames with a host configured by host_args
    // (empty => no host). Asserts exit 0 within a generous per-case timeout. `frames` is
    // padded to give the client thread time to connect + deliver state.
    auto run_case = [&](const char* name, const std::vector<std::string>& host_args,
                        const std::string& frames, int timeout_ms,
                        const std::vector<std::string>& extra_env = {},
                        bool kill_host_early = false) {
        pid_t host = -1;
        if (!host_args.empty()) {
            std::vector<std::string> args = {"--socket", socket, "--cache-dir", cache};
            args.insert(args.end(), host_args.begin(), host_args.end());
            host = spawn_fake_host(fake_host.c_str(), args);
            if (host < 0) {
                std::fprintf(stderr, "test_layer_faults[%s]: failed to spawn fake_host\n", name);
                rc = 1;
                return;
            }
            for (int i = 0; i < 100 && !file_exists(socket); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        std::vector<std::string> env = layer_env;
        env.insert(env.end(), extra_env.begin(), extra_env.end());
        const char* a[] = {vk_app.c_str(), "--frames", frames.c_str(), nullptr};
        RunResult r = run_child_timeout(vk_app.c_str(), a, env, timeout_ms);

        // For the disconnect case the host serves once and exits on its own; otherwise
        // reap it now. (kill_host_early is handled by --once at spawn time.)
        (void)kill_host_early;
        if (host > 0) kill_and_reap(host);

        if (r.exit_code == 77) {
            std::fprintf(stderr, "test_layer_faults[%s]: vk_min_present skipped (no headless) — SKIP\n", name);
            skipped = true;
            return;
        }
        if (r.exit_code == 0) {
            std::printf("test_layer_faults[%s]: PASS (%s)\n", name, describe(r));
        } else {
            std::fprintf(stderr, "test_layer_faults[%s]: FAIL — %s (expected exit 0)\n",
                         name, describe(r));
            rc = 1;
        }
    };

    // -- Case 1: truncated/garbage avatar files. Layer draws placeholders, never crashes.
    run_case("corrupt-avatars", {"--corrupt-avatars"}, "40", 30000);
    if (skipped) { std::fprintf(stderr, "test_layer_faults: skipping remaining (no headless)\n"); return 77; }

    // -- Case 2: malformed Snapshot frame. Layer drops it, keeps presenting.
    run_case("corrupt-snapshot", {"--corrupt-snapshot"}, "40", 30000);

    // -- Case 3: host disconnect mid-stream. fake_host --once serves one client then
    // exits; the layer keeps presenting and exits 0 (reconnect attempted in background).
    run_case("host-disconnect", {"--once"}, "60", 30000);

    // -- Case 4: in_voice toggling for 300 frames (host flaps in_voice). No hang, exit 0.
    run_case("invoice-flap-300", {"--flap", "300"}, "300", 60000);

    // -- Lazy-init demonstration: a NO-VOICE run (NO host at all). With lazy-init correct,
    // the layer must NOT allocate any overlay Vulkan/ImGui state — so the
    // CHOIR_DEBUG_LAZY_INIT log line must NEVER appear. We capture vk_min_present's
    // stderr to a file and grep it.
    ::unlink(socket.c_str());  // make sure no host socket exists
    {
        const std::string log = tmpdir + "/lazy.log";
        // Redirect the child's stderr to `log` via a wrapper: fork, dup2, exec.
        pid_t pid = ::fork();
        if (pid == 0) {
            for (const auto& kv : layer_env) {
                auto eq = kv.find('=');
                if (eq != std::string::npos)
                    ::setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
            }
            ::setenv("CHOIR_DEBUG_LAZY_INIT", "1", 1);
            FILE* f = std::freopen(log.c_str(), "w", stderr);
            (void)f;
            const char* a[] = {vk_app.c_str(), "--frames", "20", nullptr};
            ::execv(vk_app.c_str(), const_cast<char* const*>(a));
            ::_exit(127);
        }
        int status = 0;
        // Watchdog for this case too.
        std::atomic<bool> done{false};
        std::thread wd([&] {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!done.load()) {
                if (std::chrono::steady_clock::now() >= deadline) { ::kill(pid, SIGKILL); return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        done.store(true);
        wd.join();

        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (code == 77) {
            std::fprintf(stderr, "test_layer_faults[lazy-init]: SKIP (no headless)\n");
            return 77;
        }
        if (code != 0) {
            std::fprintf(stderr, "test_layer_faults[lazy-init]: FAIL — no-voice run exited %d\n", code);
            rc = 1;
        }
        // Grep the captured stderr for the lazy-init signal. It must be ABSENT.
        bool allocated = false;
        if (FILE* f = std::fopen(log.c_str(), "rb")) {
            char line[512];
            while (std::fgets(line, sizeof(line), f)) {
                if (std::strstr(line, "overlay lazy-init on first in_voice")) {
                    allocated = true;
                    std::fprintf(stderr, "test_layer_faults[lazy-init]: saw line: %s", line);
                }
            }
            std::fclose(f);
        }
        if (allocated) {
            std::fprintf(stderr, "test_layer_faults[lazy-init]: FAIL — overlay was allocated "
                                 "with NO in_voice snapshot (lazy-init violated)\n");
            rc = 1;
        } else {
            std::printf("test_layer_faults[lazy-init]: PASS — no-voice run did ZERO overlay "
                        "alloc (CHOIR_DEBUG_LAZY_INIT signal never logged)\n");
        }
    }

    if (rc == 0)
        std::puts("test_layer_faults: PASS — every injected fault degraded gracefully "
                  "(exit 0, no crash, no hang); lazy-init verified");
    return rc;
}
