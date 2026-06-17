#include "ipc/framing.hpp"

#include <cerrno>
#include <unistd.h>

namespace choir {

bool write_frame(int fd, MsgType type, const std::string& payload) {
    std::vector<uint8_t> frame;
    encode_frame(type, payload, frame);

    size_t off = 0;
    while (off < frame.size()) {
        ssize_t n = ::write(fd, frame.data() + off, frame.size() - off);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue; // retry (blocking fd; spin on transient conditions)
        }
        return false; // hard error or closed
    }
    return true;
}

bool FrameReader::feed(int fd) {
    uint8_t tmp[4096];
    ssize_t n = ::read(fd, tmp, sizeof(tmp));
    if (n > 0) {
        buf_.insert(buf_.end(), tmp, tmp + n);
        return true;
    }
    if (n == 0) {
        return false; // EOF
    }
    // n < 0
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return true; // alive; just nothing available right now
    }
    return false; // hard error
}

bool FrameReader::next(DecodedFrame& out) {
    size_t consumed = try_decode_frame(buf_.data(), buf_.size(), out);
    if (consumed == 0) return false;
    buf_.erase(buf_.begin(), buf_.begin() + consumed);
    return true;
}

} // namespace choir
