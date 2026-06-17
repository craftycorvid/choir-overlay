#include "ipc/protocol.hpp"

namespace choir {

void encode_frame(MsgType type, const std::string& payload, std::vector<uint8_t>& out) {
    // total_len covers the type byte plus the payload.
    const uint32_t total = static_cast<uint32_t>(1 + payload.size());

    // [uint32 LE total_len]
    out.push_back(static_cast<uint8_t>(total & 0xFF));
    out.push_back(static_cast<uint8_t>((total >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((total >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((total >> 24) & 0xFF));

    // [uint8 type]
    out.push_back(static_cast<uint8_t>(type));

    // [payload]
    out.insert(out.end(), payload.begin(), payload.end());
}

size_t try_decode_frame(const uint8_t* buf, size_t len, DecodedFrame& frame) {
    if (len < 4) return 0; // not enough for the length prefix

    const uint32_t total =
        static_cast<uint32_t>(buf[0]) |
        (static_cast<uint32_t>(buf[1]) << 8) |
        (static_cast<uint32_t>(buf[2]) << 16) |
        (static_cast<uint32_t>(buf[3]) << 24);

    // Need the 4-byte prefix plus `total` bytes (type byte + payload).
    if (total < 1) return 0;            // malformed: must contain at least the type byte
    if (len < 4 + total) return 0;      // full frame not buffered yet

    frame.type = static_cast<MsgType>(buf[4]);
    const size_t payload_len = total - 1;
    frame.payload.assign(reinterpret_cast<const char*>(buf + 5), payload_len);

    return 4 + total;
}

} // namespace choir
