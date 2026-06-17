#include "discord/ipc_transport.hpp"

#include "ipc/paths.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace choir {

namespace {

// Discord RPC opcodes we care about at the transport level.
constexpr int kOpPing = 3;
constexpr int kOpPong = 4;

// 8-byte frame header: two little-endian int32 (opcode, length).
constexpr size_t kHeaderLen = 8;

// Sanity cap so a hostile/garbled length field can't make us allocate wildly.
// Discord RPC payloads are small JSON; 16 MiB is far beyond anything legitimate.
constexpr int32_t kMaxPayloadLen = 16 * 1024 * 1024;

// Bound on the receive backlog. If we accumulate more than this without yielding
// a complete message, the peer is sending garbage (or a wedged stream) — drop
// the connection rather than let rbuf_ grow without limit. Mirrors FrameReader
// in src/ipc/framing.cpp.
constexpr size_t kMaxRecvBuffer = 16 * 1024 * 1024;

// Timeout (ms) for waiting on POLLOUT when the send buffer is full. A frozen or
// wedged Discord client must not let us hot-spin a CPU core; on timeout we treat
// the write as failed and drop the connection.
constexpr int kWritePollTimeoutMs = 2000;

int32_t read_le32(const uint8_t* p) {
    uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                 (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    return static_cast<int32_t>(v);
}

void append_le32(std::vector<uint8_t>& out, int32_t value) {
    uint32_t v = static_cast<uint32_t>(value);
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

} // namespace

IpcTransport::~IpcTransport() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool IpcTransport::connect() {
    if (fd_ >= 0) return true; // already connected

    const std::string base = runtime_dir() + "/discord-ipc-";
    for (int i = 0; i <= 9; ++i) {
        std::string path = base + std::to_string(i);
        if (path.size() >= sizeof(sockaddr_un{}.sun_path)) continue;

        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) continue;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        // Blocking connect (allowed by the design) — only the post-connect reads
        // need to be non-blocking.
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            continue;
        }

        // Switch to non-blocking so poll() never blocks.
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            ::close(fd);
            continue;
        }

        fd_ = fd;
        rbuf_.clear();
        return true;
    }
    return false;
}

void IpcTransport::set_handlers(OnMessage on_msg, OnClosed on_closed) {
    on_msg_ = std::move(on_msg);
    on_closed_ = std::move(on_closed);
}

bool IpcTransport::send_raw(int op, const std::string& bytes) {
    if (fd_ < 0) return false;

    std::vector<uint8_t> frame;
    frame.reserve(kHeaderLen + bytes.size());
    append_le32(frame, op);
    append_le32(frame, static_cast<int32_t>(bytes.size()));
    frame.insert(frame.end(), bytes.begin(), bytes.end());

    size_t off = 0;
    while (off < frame.size()) {
        ssize_t n = ::write(fd_, frame.data() + off, frame.size() - off);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Socket send buffer is full. Block (with a timeout) on POLLOUT rather
            // than busy-spinning — a frozen/wedged Discord client must not let us
            // hot-spin a CPU core. On timeout, treat as a write failure and drop
            // the connection.
            struct pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLOUT;
            int pr = ::poll(&pfd, 1, kWritePollTimeoutMs);
            if (pr > 0 && (pfd.revents & POLLOUT)) {
                continue;  // writable again; retry the write
            }
            if (pr < 0 && errno == EINTR) {
                continue;  // interrupted; retry the poll/write
            }
            // pr == 0 (timeout) or a poll error / hangup: the peer is wedged.
            handle_closed();
            return false;
        }
        // Hard error / peer gone.
        handle_closed();
        return false;
    }
    return true;
}

bool IpcTransport::send(int op, const nlohmann::json& payload) {
    return send_raw(op, payload.dump());
}

bool IpcTransport::peel_one(int& op, std::string& json_bytes) {
    if (rbuf_.size() < kHeaderLen) return false;

    int32_t hdr_op = read_le32(rbuf_.data());
    int32_t len = read_le32(rbuf_.data() + 4);
    if (len < 0 || len > kMaxPayloadLen) {
        // Corrupt/hostile length. Drop the stream.
        handle_closed();
        return false;
    }

    size_t total = kHeaderLen + static_cast<size_t>(len);
    if (rbuf_.size() < total) return false; // wait for the rest

    op = hdr_op;
    json_bytes.assign(reinterpret_cast<const char*>(rbuf_.data() + kHeaderLen),
                      static_cast<size_t>(len));
    rbuf_.erase(rbuf_.begin(), rbuf_.begin() + total);
    return true;
}

void IpcTransport::poll() {
    if (fd_ < 0) return;

    // Drain everything currently available (non-blocking).
    for (;;) {
        uint8_t tmp[4096];
        ssize_t n = ::read(fd_, tmp, sizeof(tmp));
        if (n > 0) {
            rbuf_.insert(rbuf_.end(), tmp, tmp + n);
            // Bound the backlog: if we are holding more than a max-size message
            // and still cannot peel a complete one, the stream is garbage.
            if (rbuf_.size() > kMaxRecvBuffer) {
                handle_closed();
                return;
            }
            if (static_cast<size_t>(n) < sizeof(tmp)) break; // likely drained
            continue;
        }
        if (n == 0) {
            handle_closed(); // EOF
            return;
        }
        // n < 0
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // nothing more right now
        handle_closed(); // hard error
        return;
    }

    // Peel and dispatch every complete message.
    int op;
    std::string json_bytes;
    while (fd_ >= 0 && peel_one(op, json_bytes)) {
        if (op == kOpPing) {
            // Auto-reply with PONG echoing the exact payload bytes.
            send_raw(kOpPong, json_bytes);
            continue;
        }
        if (on_msg_) {
            nlohmann::json j = nlohmann::json::parse(json_bytes, nullptr, /*allow_exceptions=*/false);
            if (!j.is_discarded()) {
                on_msg_(op, j);
            }
            // A non-parseable payload is silently dropped: higher layers only
            // care about well-formed RPC JSON, and we must never throw out of poll().
        }
    }
}

void IpcTransport::handle_closed() {
    if (fd_ < 0) return; // already closed; fire OnClosed at most once
    ::close(fd_);
    fd_ = -1;
    rbuf_.clear();
    if (on_closed_) on_closed_();
}

void IpcTransport::close() {
    if (fd_ < 0) return;
    ::close(fd_);
    fd_ = -1;
    rbuf_.clear();
    // Explicit close() is caller-initiated, so we do NOT fire OnClosed (that
    // signals an unexpected drop to the FSM in later tasks).
}

} // namespace choir
