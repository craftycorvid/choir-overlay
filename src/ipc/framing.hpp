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
//
// On a malformed/hostile stream (a frame claiming total_len == 0 or > kMaxFrameLen,
// or the buffer growing past kMaxFrameLen without ever yielding a frame) the reader
// enters a permanent "failed" state: the buffer is cleared, `feed` and `next` both
// return false, and `failed()` returns true. The caller loop
//   while (reader.next(frame)) { handle(frame); }
//   if (reader.failed() || !reader.feed(fd)) close(fd);
// should drop the connection once the reader is dead.
class FrameReader {
public:
    // Append available bytes from `fd`. Returns false on EOF, hard error, or if the
    // reader has already failed. On EAGAIN/EWOULDBLOCK (nothing available right now)
    // returns true.
    bool feed(int fd);

    // Pop one complete frame if buffered. Returns false if none is ready yet OR if
    // the stream is dead; distinguish via failed().
    bool next(DecodedFrame& out);

    // True once a protocol error has killed the stream. Terminal: never clears.
    bool failed() const { return failed_; }

private:
    std::vector<uint8_t> buf_;
    bool failed_ = false;
};

} // namespace choir
