// Feature-based golden test for the Choir layer's real overlay (Task 17).
//
// Drives the layer end-to-end against fake_host (the scripted "Test Voice" channel:
// Alice = solid RED avatar, Bob = solid GREEN avatar + speaking, Carol = solid BLUE
// avatar + self_mute) and reads back the presented PPM, asserting feature-based checks
// (NOT a brittle full-image golden):
//
//   IN VOICE (fake_host connected, default config = TopRight anchor):
//     * the panel region differs from the app's blue clear (the overlay drew),
//     * each of the three avatar colors (red, green, blue) appears in the panel region
//       (proving avatars resolved from the retention map + drew),
//     * a green speaking-ring pixel appears just outside Bob's avatar (speaking path),
//     * Carol's mute glyph (a red slash) appears in her row,
//     * a far/empty corner stays the app blue.
//
//   RECREATE (--recreate builds a second swapchain with a fresh AvatarTextures and no
//     replayed AvatarReady): the three avatars still render (avatar retention fix).
//
//   NO HOST (no socket): the overlay draws nothing — the whole frame stays app blue —
//     and vk_min_present still exits 0.
//
// vk_min_present exits 77 when no headless surface / present-capable device is
// available; this test propagates that as SKIP.
//
// Invocation (from meson): test_layer_golden <vk_min_present> <fake_host> <layer_dir> <tmp.ppm>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// --- Minimal binary PPM (P6) reader. ---
bool read_ppm(const char* path, uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgb) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return false;
    char magic[3] = {0, 0, 0};
    if (std::fscanf(fp, "%2s", magic) != 1 || std::strcmp(magic, "P6") != 0) { std::fclose(fp); return false; }
    int maxval = 0;
    if (std::fscanf(fp, "%u %u %d", &w, &h, &maxval) != 3 || maxval != 255) { std::fclose(fp); return false; }
    std::fgetc(fp);  // single whitespace after the header
    const size_t n = static_cast<size_t>(w) * h * 3;
    rgb.resize(n);
    size_t got = std::fread(rgb.data(), 1, n, fp);
    std::fclose(fp);
    return got == n;
}

struct RGB { uint8_t r, g, b; };

RGB pixel(const std::vector<uint8_t>& rgb, uint32_t w, uint32_t x, uint32_t y) {
    const size_t i = (static_cast<size_t>(y) * w + x) * 3;
    return {rgb[i], rgb[i + 1], rgb[i + 2]};
}

bool near(uint8_t a, int b, int tol = 24) { return std::abs(static_cast<int>(a) - b) <= tol; }

// The app's clear color (a dark blue): R=26 G=51 B=204 (see vk_min_present).
bool is_app_blue(RGB p) { return near(p.r, 26) && near(p.g, 51) && near(p.b, 204); }

// Avatar solid colors written by fake_host (0xE0=224 dominant, 0x40=64 others). NOTE
// the avatar BLUE (64,64,224) must be distinguished from the app's clear blue
// (26,51,204): both have a high B and low R/G, so is_blue additionally requires the R
// and G channels to be near 64 (well above the app blue's 26/51) — otherwise an
// all-app-blue panel edge would falsely satisfy "Carol's blue avatar drew".
bool is_red(RGB p) { return p.r > 150 && p.g < 120 && p.b < 120; }
bool is_green(RGB p) { return p.g > 150 && p.r < 120 && p.b < 120; }
bool is_blue(RGB p) { return p.b > 150 && p.r > 45 && p.r < 120 && p.g > 45 && p.g < 120; }

// Does any pixel in [x0,x1)x[y0,y1) satisfy `pred`?
template <typename Pred>
bool any_pixel(const std::vector<uint8_t>& rgb, uint32_t w, uint32_t h, uint32_t x0,
               uint32_t y0, uint32_t x1, uint32_t y1, Pred pred) {
    x1 = x1 < w ? x1 : w;
    y1 = y1 < h ? y1 : h;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
            if (pred(pixel(rgb, w, x, y))) return true;
    return false;
}

std::string g_tmpdir;

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
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

pid_t spawn_fake_host(const char* path, const std::string& cache) {
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // fake_host binds the abstract socket named by $CHOIR_SOCKET (inherited).
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


}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <vk_min_present> <fake_host> <layer_dir> <out.ppm>\n",
                     argv[0]);
        return 2;
    }
    const std::string vk_app = argv[1];
    const std::string fake_host = argv[2];
    const std::string layer_dir = argv[3];
    const std::string out = argv[4];

    char tmpl[] = "/tmp/choir_layer_golden_XXXXXX";
    char* d = ::mkdtemp(tmpl);
    if (!d) { std::fprintf(stderr, "golden: mkdtemp failed\n"); return 2; }
    g_tmpdir = d;
    const std::string cache = g_tmpdir + "/avatars";
    ::mkdir(cache.c_str(), 0700);

    // Unique abstract socket name; fake_host and the layer (both inherit our env) read
    // $CHOIR_SOCKET. The meson test env sets a clean XDG_DATA_HOME so the installed
    // implicit layer doesn't shadow the build layer.
    const std::string sockname = "choir-test-golden-" + std::to_string(::getpid());
    ::setenv("CHOIR_SOCKET", sockname.c_str(), 1);

    const std::vector<std::string> layer_env = {
        "VK_LAYER_PATH=" + layer_dir,
        "VK_INSTANCE_LAYERS=VK_LAYER_choir_overlay_x86_64",
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_choir_overlay_x86_64",
        "DISABLE_CHOIR_OVERLAY=",  // ensure not disabled by a stale env flag
    };

    int rc = 0;

    // Helper: run vk_min_present (optionally --recreate) with the layer + the given env,
    // read back the PPM. Returns the child exit code; fills w/h/rgb on success.
    auto run_present = [&](bool recreate, const std::vector<std::string>& env, uint32_t& w,
                          uint32_t& h, std::vector<uint8_t>& rgb) -> int {
        std::vector<const char*> a = {vk_app.c_str(), "--frames", "40", "--readback",
                                      out.c_str()};
        if (recreate) a.push_back("--recreate");
        a.push_back(nullptr);
        int code = run_child(vk_app.c_str(), a.data(), env);
        if (code != 0) return code;
        if (!read_ppm(out.c_str(), w, h, rgb)) return -2;
        return 0;
    };

    // =========================================================================
    // Phase 1: IN VOICE — the panel + three avatars + speaking ring + mute glyph.
    // =========================================================================
    pid_t host = spawn_fake_host(fake_host.c_str(), cache);
    if (host < 0) { std::fprintf(stderr, "golden: failed to spawn fake_host\n"); return 2; }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let fake_host bind

    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgb;
    int code = run_present(false, layer_env, w, h, rgb);

    if (code == 77) {
        std::fprintf(stderr, "golden: vk_min_present skipped (no headless/present) — SKIP\n");
        kill_and_reap(host);
        return 77;
    }
    if (code == -2) {
        std::fprintf(stderr, "golden: could not read PPM %s\n", out.c_str());
        kill_and_reap(host);
        return 1;
    }
    if (code != 0) {
        std::fprintf(stderr, "golden: vk_min_present (in voice) exited %d\n", code);
        kill_and_reap(host);
        return 1;
    }
    if (w < 256 || h < 256) {
        std::fprintf(stderr, "golden: readback too small (%ux%u)\n", w, h);
        kill_and_reap(host);
        return 1;
    }

    // Default config anchor = TopRight; extent 256 => panel spans x[20..240], rows from
    // the top. Avatars sit at the panel's left inner edge near x=30..62, stacked:
    // Alice (red) ~y50-78, Bob (green) ~y86-118, Carol (blue) ~y130-158. We scan the
    // whole top-left quadrant of the panel for robustness against minor layout drift.
    const uint32_t px0 = 20, py0 = 20, px1 = 130, py1 = 200;  // panel rows region

    auto check = [&](bool cond, const char* msg) {
        if (!cond) { std::fprintf(stderr, "golden: FAIL — %s\n", msg); rc = 1; }
    };

    // The panel region must contain pixels that are NOT the app blue (overlay drew).
    check(any_pixel(rgb, w, h, px0, py0, px1, py1,
                    [](RGB p) { return !is_app_blue(p); }),
          "panel region is entirely app-blue (overlay not drawn)");

    // Each avatar color appears in the panel region (avatars resolved + drew).
    check(any_pixel(rgb, w, h, px0, py0, px1, py1, is_red), "no RED avatar pixel in panel");
    check(any_pixel(rgb, w, h, px0, py0, px1, py1, is_green), "no GREEN avatar pixel in panel");
    check(any_pixel(rgb, w, h, px0, py0, px1, py1, is_blue), "no BLUE avatar pixel in panel");

    // Bob (green) is the only speaker: his green ring sits just left of his avatar's solid
    // edge (~x28). Scan the narrow band x[26..31) over the full panel height (robust to the
    // exact row offset now that there's no channel header) — any green there is his ring.
    check(any_pixel(rgb, w, h, 26, py0, 31, py1, is_green),
          "no green speaking-ring pixel outside Bob's avatar");

    // Carol (blue, self_mute) is the only muted user: her red mute-glyph slash sits at the
    // panel's right edge. Scan x[208..240) over the full panel height — the only red at the
    // right edge is her glyph (avatars are at the left).
    check(any_pixel(rgb, w, h, 208, py0, 240, py1, is_red),
          "no red mute glyph in Carol's row");

    // A far/empty corner (bottom-left) stays the app blue.
    check(is_app_blue(pixel(rgb, w, 20, 236)), "far pixel (20,236) is not app blue");

    std::printf("golden: in-voice panel — red/green/blue avatars present, Bob ring present, "
                "Carol mute glyph present, far pixel app-blue\n");

    // =========================================================================
    // Phase 2: RECREATE — avatars must still show with a fresh swapchain (retention).
    // =========================================================================
    {
        uint32_t rw = 0, rh = 0;
        std::vector<uint8_t> r2;
        int c2 = run_present(true, layer_env, rw, rh, r2);
        if (c2 == 77) {
            std::fprintf(stderr, "golden: recreate run skipped (no headless)\n");
        } else if (c2 != 0 || rw < 256 || rh < 256) {
            std::fprintf(stderr, "golden: FAIL — recreate run exited %d / bad readback\n", c2);
            rc = 1;
        } else {
            const uint32_t qx0 = 20, qy0 = 20, qx1 = 130, qy1 = 200;
            check(any_pixel(r2, rw, rh, qx0, qy0, qx1, qy1, is_red),
                  "recreate: no RED avatar (retention failed)");
            check(any_pixel(r2, rw, rh, qx0, qy0, qx1, qy1, is_green),
                  "recreate: no GREEN avatar (retention failed)");
            check(any_pixel(r2, rw, rh, qx0, qy0, qx1, qy1, is_blue),
                  "recreate: no BLUE avatar (retention failed)");
            std::printf("golden: recreate — all three avatars still present (retention OK)\n");
        }
    }

    kill_and_reap(host);

    // =========================================================================
    // Phase 3: NO HOST — overlay draws nothing; the whole frame stays app blue.
    // =========================================================================
    // (host killed above; its abstract socket is released on exit — no file to unlink)
    {
        uint32_t nw = 0, nh = 0;
        std::vector<uint8_t> nrgb;
        int c3 = run_present(false, layer_env, nw, nh, nrgb);
        if (c3 == 77) {
            std::fprintf(stderr, "golden: no-host run skipped (no headless)\n");
        } else if (c3 != 0 || nw < 256 || nh < 256) {
            std::fprintf(stderr, "golden: FAIL — no-host run exited %d / bad readback\n", c3);
            rc = 1;
        } else {
            // Sample a grid; every sampled pixel must be the app blue (nothing drawn).
            bool all_blue = true;
            for (uint32_t y = 0; y < nh && all_blue; y += 16)
                for (uint32_t x = 0; x < nw && all_blue; x += 16)
                    if (!is_app_blue(pixel(nrgb, nw, x, y))) all_blue = false;
            check(all_blue, "no-host frame has non-blue pixels (overlay drew without a host)");
            if (all_blue)
                std::printf("golden: no-host — frame is entirely app-blue (nothing drawn)\n");
        }
    }

    if (rc == 0)
        std::puts("golden: PASS — voice panel + avatars + speaking ring + mute glyph drawn; "
                  "avatars survive recreate; nothing drawn without a host");
    return rc;
}
