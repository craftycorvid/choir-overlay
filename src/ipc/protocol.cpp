#include "ipc/protocol.hpp"

#include <cassert>

namespace choir {

void encode_frame(MsgType type, const std::string& payload, std::vector<uint8_t>& out) {
    // total_len covers the type byte plus the payload. Refuse payloads that would
    // overflow the wire cap; silently truncating the length field would corrupt the
    // stream for the peer. Caller's responsibility (see header) — assert in debug.
    assert(size_t(1) + payload.size() <= kMaxFrameLen &&
           "encode_frame: payload exceeds kMaxFrameLen");
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

size_t try_decode_frame(const uint8_t* buf, size_t len, DecodedFrame& frame, bool* error) {
    if (len < 4) return 0; // not enough for the length prefix

    // Read total_len into a size_t so all subsequent size math is wide and cannot
    // wrap (the 32-bit `4 + total` form wrapped for values near UINT32_MAX, which
    // let an over-cap frame slip past the bounds check and read OOB).
    const size_t total =
        static_cast<size_t>(buf[0]) |
        (static_cast<size_t>(buf[1]) << 8) |
        (static_cast<size_t>(buf[2]) << 16) |
        (static_cast<size_t>(buf[3]) << 24);

    // Validate `total` BEFORE any size arithmetic. A zero total can't even hold the
    // type byte, and anything over the cap is treated as a hostile/garbage stream.
    if (total < 1 || total > kMaxFrameLen) {
        if (error) *error = true;
        return 0; // hard protocol error
    }

    // Need the 4-byte prefix plus `total` bytes (type byte + payload). All in size_t.
    if (len < size_t(4) + total) return 0; // full frame not buffered yet

    frame.type = static_cast<MsgType>(buf[4]);
    const size_t payload_len = total - 1;
    frame.payload.assign(reinterpret_cast<const char*>(buf + 5), payload_len);

    return size_t(4) + total;
}

} // namespace choir
