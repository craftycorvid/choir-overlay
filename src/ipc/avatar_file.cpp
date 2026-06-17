#include "ipc/avatar_file.hpp"

#include <cstdio>

namespace choir {

namespace {
constexpr char kMagic[4] = {'C', 'H', 'A', 'V'};

void put_u32_le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

uint32_t get_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
} // namespace

bool write_avatar_rgba(const std::string& path, uint32_t w, uint32_t h, const uint8_t* rgba) {
    if (rgba == nullptr && (w != 0 && h != 0)) return false;

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    bool ok = true;

    uint8_t header[12];
    header[0] = kMagic[0];
    header[1] = kMagic[1];
    header[2] = kMagic[2];
    header[3] = kMagic[3];
    put_u32_le(header + 4, w);
    put_u32_le(header + 8, h);

    if (std::fwrite(header, 1, sizeof(header), f) != sizeof(header)) ok = false;

    const size_t body = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    if (ok && body > 0) {
        if (std::fwrite(rgba, 1, body, f) != body) ok = false;
    }

    if (std::fclose(f) != 0) ok = false;
    return ok;
}

bool read_avatar_rgba(const std::string& path, uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgba) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;

    uint8_t header[12];
    if (std::fread(header, 1, sizeof(header), f) != sizeof(header)) {
        std::fclose(f);
        return false;
    }

    if (header[0] != kMagic[0] || header[1] != kMagic[1] ||
        header[2] != kMagic[2] || header[3] != kMagic[3]) {
        std::fclose(f);
        return false; // wrong magic
    }

    const uint32_t fw = get_u32_le(header + 4);
    const uint32_t fh = get_u32_le(header + 8);
    const size_t body = static_cast<size_t>(fw) * static_cast<size_t>(fh) * 4u;

    std::vector<uint8_t> pixels(body);
    if (body > 0) {
        if (std::fread(pixels.data(), 1, body, f) != body) {
            std::fclose(f); // truncated / size mismatch
            return false;
        }
    }

    // Reject trailing garbage (file larger than declared).
    uint8_t extra;
    if (std::fread(&extra, 1, 1, f) != 0) {
        std::fclose(f);
        return false;
    }

    std::fclose(f);

    w = fw;
    h = fh;
    rgba = std::move(pixels);
    return true;
}

} // namespace choir
