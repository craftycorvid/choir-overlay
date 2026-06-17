#pragma once

// IpcTransport: the lowest layer of the Discord local RPC link.
//
// Connects to the running Discord desktop client's local RPC unix socket
// ($XDG_RUNTIME_DIR/discord-ipc-{0..9}) and exchanges opcode-framed JSON.
//
// Wire frame: an 8-byte header of two little-endian int32 (opcode, then length),
// followed by `length` bytes of JSON.
//
// This class is deliberately Qt-free and dependency-light (POSIX sockets +
// nlohmann/json) so it stays unit-testable. The real host pumps poll() via a
// QSocketNotifier on fd(); poll() uses non-blocking reads so it never blocks and
// returns promptly when called.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace choir {

class IpcTransport {
public:
    using OnMessage = std::function<void(int op, const nlohmann::json&)>;
    using OnClosed = std::function<void()>;

    IpcTransport() = default;
    ~IpcTransport();

    IpcTransport(const IpcTransport&) = delete;
    IpcTransport& operator=(const IpcTransport&) = delete;

    // Probe $XDG_RUNTIME_DIR/discord-ipc-0..9 and connect to the first that
    // accepts (AF_UNIX, SOCK_STREAM). The socket is left in non-blocking mode so
    // poll() never blocks. Returns true on success.
    bool connect();

    void set_handlers(OnMessage on_msg, OnClosed on_closed);

    // Build the 8-byte LE [op][len] header + JSON bytes and write them. Returns
    // false if there is no live connection or the write fails (which also closes
    // the connection and fires the closed handler).
    bool send(int op, const nlohmann::json& payload);

    // Read whatever bytes are currently available (non-blocking), append them to
    // the read buffer, and dispatch every complete message. PING (op 3) is
    // auto-answered with PONG (op 4) echoing the payload; all other messages go
    // to the OnMessage handler. On EOF/error, fires OnClosed and closes the fd.
    void poll();

    void close();

    int fd() const { return fd_; }

private:
    // Try to peel exactly one complete message off the front of rbuf_. Returns
    // false if a full message is not yet buffered.
    bool peel_one(int& op, std::string& json_bytes);

    // Send raw op-framed bytes; on failure, mark the connection dead.
    bool send_raw(int op, const std::string& bytes);

    // Tear down and fire OnClosed exactly once.
    void handle_closed();

    int fd_ = -1;
    std::vector<uint8_t> rbuf_;
    OnMessage on_msg_;
    OnClosed on_closed_;
};

} // namespace choir
