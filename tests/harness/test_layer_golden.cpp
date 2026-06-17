// Golden test for the Choir layer's present-time draw path (Task 14).
//
// Runs vk_min_present WITH the Choir implicit layer forced on, captures a PPM
// readback, and asserts the overlay's test rectangle was drawn ON TOP of the app's
// blue clear: a pixel inside the rect region (40,40) reads ~red, and a pixel
// outside (200,200) reads ~blue (the app clear). This proves the layer hooked
// present, recorded a render pass into the swapchain image, and chained semaphores
// correctly (otherwise the readback would be all blue, or the run would deadlock).
//
// Exit 77 (SKIP) is propagated from vk_min_present when no headless surface / no
// present-capable device is available, so this test skips on no-GPU CI.
//
// Invocation (from meson): test_layer_golden <vk_min_present> <layer_dir> <out.ppm>

#include <sys/wait.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Minimal binary PPM (P6) reader. Returns false on any malformed input.
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

bool is_red(RGB p) { return near(p.r, 255) && near(p.g, 0) && near(p.b, 0); }
bool is_blue(RGB p) { return near(p.r, 26) && near(p.g, 51) && near(p.b, 204); }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <vk_min_present> <layer_dir> <out.ppm>\n", argv[0]);
        return 2;
    }
    const char* vk_app = argv[1];
    const char* layer_dir = argv[2];
    const char* out = argv[3];

    // Force the layer on for the child process and run a few frames so per-image
    // command-buffer / fence reuse is exercised (FIFO queues several frames).
    setenv("VK_LAYER_PATH", layer_dir, 1);
    setenv("VK_INSTANCE_LAYERS", "VK_LAYER_choir_overlay_x86_64", 1);
    setenv("VK_LOADER_LAYERS_ENABLE", "VK_LAYER_choir_overlay_x86_64", 1);
    // Make sure a stale DISABLE flag from the environment can't suppress the draw.
    unsetenv("DISABLE_CHOIR_OVERLAY");

    std::string cmd = std::string(vk_app) + " --frames 3 --readback " + out;
    int rc = std::system(cmd.c_str());
    if (rc == -1) { std::fprintf(stderr, "golden: failed to launch %s\n", vk_app); return 2; }
    const int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    if (exit_code == 77) {
        std::fprintf(stderr, "golden: vk_min_present skipped (no headless/present) — SKIP\n");
        return 77;
    }
    if (exit_code != 0) {
        std::fprintf(stderr, "golden: vk_min_present exited %d\n", exit_code);
        return 1;
    }

    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgb;
    if (!read_ppm(out, w, h, rgb)) {
        std::fprintf(stderr, "golden: could not read PPM %s\n", out);
        return 1;
    }
    if (w < 201 || h < 201) {
        std::fprintf(stderr, "golden: readback too small (%ux%u)\n", w, h);
        return 1;
    }

    const RGB inside = pixel(rgb, w, 40, 40);     // inside the 64x64 rect at (20,20)
    const RGB outside = pixel(rgb, w, 200, 200);  // far outside the rect
    std::printf("golden: inside(40,40)=(%u,%u,%u) outside(200,200)=(%u,%u,%u)\n",
                inside.r, inside.g, inside.b, outside.r, outside.g, outside.b);

    bool ok = true;
    if (!is_red(inside)) {
        std::fprintf(stderr, "golden: FAIL — pixel inside rect is not red (overlay not drawn)\n");
        ok = false;
    }
    if (!is_blue(outside)) {
        std::fprintf(stderr, "golden: FAIL — pixel outside rect is not the app clear blue\n");
        ok = false;
    }
    if (!ok) return 1;
    std::puts("golden: PASS — overlay rectangle drawn over the app frame");
    return 0;
}
