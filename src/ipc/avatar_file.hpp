#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace choir {
// File layout: magic "CHAV"(4) | uint32 LE width | uint32 LE height | width*height*4 RGBA8 bytes.
bool write_avatar_rgba(const std::string& path, uint32_t w, uint32_t h, const uint8_t* rgba);
bool read_avatar_rgba (const std::string& path, uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgba);
} // namespace choir
