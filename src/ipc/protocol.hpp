#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace choir {

enum class MsgType : uint8_t {
    Hello       = 1,  // layer -> host : {"pid":int,"exe":string,"proto":1}
    Snapshot    = 2,  // host  -> layer: full choir::Snapshot as JSON  (v1: full state every change)
    AvatarReady = 3,  // host  -> layer: {"hash":string,"path":string,"w":int,"h":int}
    Disabled    = 4,  // host  -> layer: this process is denylisted; stay inert
    Ping        = 5,
    Pong        = 6,
};

// A frame on the wire = [uint32 LE total_len][uint8 type][payload bytes(total_len-1)].
// encode_frame appends to `out`; decode pulls one frame from a buffer.
void encode_frame(MsgType type, const std::string& payload, std::vector<uint8_t>& out);

struct DecodedFrame { MsgType type; std::string payload; };
// Returns number of bytes consumed (0 if a full frame isn't buffered yet), sets `frame`.
size_t try_decode_frame(const uint8_t* buf, size_t len, DecodedFrame& frame);

} // namespace choir
