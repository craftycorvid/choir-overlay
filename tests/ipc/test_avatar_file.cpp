#include "ipc/avatar_file.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

int main() {
    using namespace choir;

    std::string path = "/tmp/choir_test_avatar.rgba";

    // 2x2 RGBA image, distinct bytes per pixel.
    uint32_t w = 2, h = 2;
    std::vector<uint8_t> pixels(w * h * 4);
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<uint8_t>(i * 7 + 1);
    }

    assert(write_avatar_rgba(path, w, h, pixels.data()));

    uint32_t rw = 0, rh = 0;
    std::vector<uint8_t> rpixels;
    assert(read_avatar_rgba(path, rw, rh, rpixels));
    assert(rw == w);
    assert(rh == h);
    assert(rpixels.size() == pixels.size());
    assert(rpixels == pixels);

    // Wrong magic -> rejected.
    {
        std::string bad = "/tmp/choir_test_avatar_bad.rgba";
        FILE* f = std::fopen(bad.c_str(), "wb");
        assert(f);
        const char* hdr = "XXXX"; // not "CHAV"
        std::fwrite(hdr, 1, 4, f);
        uint8_t dims[8] = {2, 0, 0, 0, 2, 0, 0, 0};
        std::fwrite(dims, 1, 8, f);
        std::vector<uint8_t> body(2 * 2 * 4, 0);
        std::fwrite(body.data(), 1, body.size(), f);
        std::fclose(f);

        uint32_t bw = 0, bh = 0;
        std::vector<uint8_t> bp;
        assert(read_avatar_rgba(bad, bw, bh, bp) == false);
        std::remove(bad.c_str());
    }

    // Truncated body (size mismatch) -> rejected.
    {
        std::string trunc = "/tmp/choir_test_avatar_trunc.rgba";
        FILE* f = std::fopen(trunc.c_str(), "wb");
        assert(f);
        std::fwrite("CHAV", 1, 4, f);
        uint8_t dims[8] = {2, 0, 0, 0, 2, 0, 0, 0}; // claims 2x2 = 16 bytes
        std::fwrite(dims, 1, 8, f);
        uint8_t body[4] = {1, 2, 3, 4}; // only 4 bytes
        std::fwrite(body, 1, 4, f);
        std::fclose(f);

        uint32_t tw = 0, th = 0;
        std::vector<uint8_t> tp;
        assert(read_avatar_rgba(trunc, tw, th, tp) == false);
        std::remove(trunc.c_str());
    }

    // Absurd dimensions in a crafted header -> rejected before any huge allocation.
    // width=100000 (* height 1 * 4) would be ~400KB here, but the cap (8192) is what
    // matters: a crafted width near UINT32_MAX would otherwise demand a giant vector.
    {
        std::string huge = "/tmp/choir_test_avatar_huge.rgba";
        FILE* f = std::fopen(huge.c_str(), "wb");
        assert(f);
        std::fwrite("CHAV", 1, 4, f);
        // width = 100000 (0x000186A0 LE), height = 1.
        uint8_t dims[8] = {0xA0, 0x86, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00};
        std::fwrite(dims, 1, 8, f);
        // No body needed: validation happens before the body read/allocation.
        std::fclose(f);

        uint32_t hw = 0, hh = 0;
        std::vector<uint8_t> hp;
        assert(read_avatar_rgba(huge, hw, hh, hp) == false);
        assert(hp.empty()); // nothing allocated/returned
        std::remove(huge.c_str());
    }

    std::remove(path.c_str());
    return 0;
}
