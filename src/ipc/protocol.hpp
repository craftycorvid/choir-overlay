#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace choir {

// Hard cap on a single frame's total_len (type byte + payload). Sized generously
// for the largest legitimate Snapshot JSON. Frames claiming more than this are a
// protocol error (these bytes come off a socket inside an untrusted game).
constexpr size_t kMaxFrameLen = 16 * 1024 * 1024; // 16 MiB

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
// Precondition (asserted): 1 + payload.size() <= kMaxFrameLen. Callers must not
// hand encode_frame a payload large enough to exceed the wire cap.
void encode_frame(MsgType type, const std::string& payload, std::vector<uint8_t>& out);

struct DecodedFrame { MsgType type; std::string payload; };
// Returns number of bytes consumed (0 if a full frame isn't buffered yet), sets `frame`.
// On a hard protocol error (total_len == 0 or > kMaxFrameLen) returns 0 and, if
// `error != nullptr`, sets *error = true. A return of 0 with *error == false just
// means "need more bytes". *error is left untouched on the success/need-more paths,
// so callers should initialize it to false before the call.
size_t try_decode_frame(const uint8_t* buf, size_t len, DecodedFrame& frame, bool* error = nullptr);

} // namespace choir
