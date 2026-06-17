#pragma once

// Local IPC state server (Task 11).
//
// StateServer is the host side of the host<->layer protocol. It listens on
// choir::runtime_socket_path() with a QLocalServer and serves each connecting
// layer client a current Snapshot (+ AvatarReady frames for known avatars),
// then broadcasts subsequent state changes.
//
// Per the wire contract (src/ipc/protocol.hpp) every message is a length-
// prefixed libchoir_ipc frame, so a plain POSIX AF_UNIX client (the layer,
// Task 16) is wire-compatible with this QLocalServer socket.
//
// Denylist gating: each connection sends a Hello{pid,exe,proto}. If is_blocked
// (built from the host's Denylist) returns true for the exe, the client gets a
// single Disabled frame and is kept inert — it never receives snapshots or
// avatars. Otherwise it is marked active and immediately seeded with the
// current Snapshot and all known avatars.
//
// This class is the testable core of the assembly; the production main wires
// its set_snapshot/broadcast/broadcast_avatar to the OverlayState + AvatarCache
// callbacks. It deliberately avoids Q_OBJECT (lambda connections only) so it
// needs no MOC pass beyond the QLocalServer/QLocalSocket Qt types.
//
// This is the only host header that pulls in Qt; it must NOT be compiled into
// the Qt-free libchoir_host_core.

#include "ipc/protocol.hpp"
#include "ipc/state.hpp"

#include <QtGlobal>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

QT_BEGIN_NAMESPACE
class QLocalServer;
class QLocalSocket;
QT_END_NAMESPACE

namespace choir {

class StateServer {
public:
    // is_blocked(exe) decides per connection whether to disable the client.
    // Typically [&denylist](const std::string& e){ return denylist.blocks(e); }.
    explicit StateServer(std::function<bool(const std::string&)> is_blocked);
    ~StateServer();

    StateServer(const StateServer&) = delete;
    StateServer& operator=(const StateServer&) = delete;

    // Begin listening on runtime_socket_path() (removing a stale socket file
    // first). Returns false if the socket could not be created.
    bool listen();

    // Store the current snapshot (sent to newly-connecting allowed clients).
    void set_snapshot(const Snapshot& s);

    // Push a Snapshot frame to every active (non-disabled) client. Also stores
    // it as current (so a connect-after-broadcast race still seeds correctly).
    void broadcast(const Snapshot& s);

    // Push an AvatarReady frame to every active client and remember it so a
    // future Hello replays it.
    void broadcast_avatar(const std::string& hash, const std::string& path,
                          uint32_t w, uint32_t h);

private:
    struct Client {
        QLocalSocket* sock = nullptr;
        std::vector<uint8_t> rbuf;   // accumulated inbound bytes
        bool active = false;         // sent Hello, not denylisted
        bool inert = false;          // denylisted: ignore forever
    };

    void on_new_connection();
    void on_ready_read(QLocalSocket* sock);
    void on_disconnected(QLocalSocket* sock);

    // Process all complete frames currently buffered for `c`. Returns false if
    // the connection must be dropped (protocol error).
    bool drain_frames(Client& c);
    void handle_hello(Client& c, const std::string& payload);

    static void send_frame(QLocalSocket* sock, MsgType type, const std::string& payload);
    void seed_client(Client& c);

    // Build the AvatarReady JSON payload for a known avatar entry.
    static std::string avatar_payload(const std::string& hash, const std::string& path,
                                      uint32_t w, uint32_t h);

    std::function<bool(const std::string&)> is_blocked_;
    QLocalServer* server_ = nullptr;
    std::unordered_map<QLocalSocket*, Client> clients_;

    Snapshot current_;

    struct AvatarEntry {
        std::string path;
        uint32_t w = 0, h = 0;
    };
    std::unordered_map<std::string, AvatarEntry> avatars_;  // hash -> entry
};

}  // namespace choir
