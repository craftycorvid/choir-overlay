#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ipc/protocol.hpp"

namespace choir {

// Encode `payload` as a frame and write it all to `fd`. Retries on EINTR/EAGAIN
// (so it works for blocking fds). Returns false on a hard write error / closed fd.
bool write_frame(int fd, MsgType type, const std::string& payload);

// Decouples byte I/O from frame boundaries: `feed` reads whatever bytes are
// available from `fd` into an internal buffer, `next` pops one complete frame.
// The layer's non-blocking reader reuses this.
class FrameReader {
public:
    // Append available bytes from `fd`. Returns false on EOF or hard error.
    // On EAGAIN/EWOULDBLOCK (nothing available right now) returns true.
    bool feed(int fd);

    // Pop one complete frame if buffered. Returns false if none is ready yet.
    bool next(DecodedFrame& out);

private:
    std::vector<uint8_t> buf_;
};

} // namespace choir
