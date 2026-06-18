#include "server/state_server.hpp"

#include "ipc/paths.hpp"

#include <nlohmann/json.hpp>

#include <QLocalServer>
#include <QLocalSocket>

#include <cstdio>

namespace choir {

StateServer::StateServer(std::function<bool(const std::string&)> is_blocked)
    : is_blocked_(std::move(is_blocked)) {}

StateServer::~StateServer() {
    // QLocalServer cleans up its child sockets; nothing extra to do.
    delete server_;
}

bool StateServer::listen() {
    // Bind an ABSTRACT-namespace socket so the injected layer can reach us from
    // inside Steam's pressure-vessel container (a filesystem socket under
    // $XDG_RUNTIME_DIR is not shared into the container; the netns — and thus
    // abstract sockets — is). Abstract sockets need no stale-file cleanup.
    const std::string name = abstract_socket_name();

    server_ = new QLocalServer();
    server_->setSocketOptions(QLocalServer::AbstractNamespaceOption);

    QObject::connect(server_, &QLocalServer::newConnection,
                     [this]() { on_new_connection(); });

    if (!server_->listen(QString::fromStdString(name))) {
        std::fprintf(stderr, "choir: StateServer failed to listen on @%s: %s\n",
                     name.c_str(), server_->errorString().toUtf8().constData());
        return false;
    }
    return true;
}

void StateServer::on_new_connection() {
    while (server_ && server_->hasPendingConnections()) {
        QLocalSocket* sock = server_->nextPendingConnection();
        if (!sock) break;

        Client& c = clients_[sock];
        c.sock = sock;

        QObject::connect(sock, &QLocalSocket::readyRead,
                         [this, sock]() { on_ready_read(sock); });
        QObject::connect(sock, &QLocalSocket::disconnected,
                         [this, sock]() { on_disconnected(sock); });

        // Some bytes may already be buffered by the time we connect readyRead.
        if (sock->bytesAvailable() > 0) on_ready_read(sock);
    }
}

void StateServer::on_ready_read(QLocalSocket* sock) {
    auto it = clients_.find(sock);
    if (it == clients_.end()) return;
    Client& c = it->second;

    const QByteArray data = sock->readAll();
    if (!data.isEmpty()) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(data.constData());
        c.rbuf.insert(c.rbuf.end(), p, p + data.size());
    }

    if (!drain_frames(c)) {
        // Protocol error -> drop the connection.
        sock->abort();
        on_disconnected(sock);
    }
}

void StateServer::on_disconnected(QLocalSocket* sock) {
    auto it = clients_.find(sock);
    if (it == clients_.end()) return;
    clients_.erase(it);
    sock->deleteLater();
}

bool StateServer::drain_frames(Client& c) {
    for (;;) {
        DecodedFrame frame;
        bool err = false;
        size_t consumed = try_decode_frame(c.rbuf.data(), c.rbuf.size(), frame, &err);
        if (err) return false;            // hard protocol error
        if (consumed == 0) {
            // Guard against an unbounded buffer that never yields a frame.
            if (c.rbuf.size() > kMaxFrameLen) return false;
            break;                         // need more bytes
        }
        c.rbuf.erase(c.rbuf.begin(), c.rbuf.begin() + consumed);

        switch (frame.type) {
            case MsgType::Hello:
                handle_hello(c, frame.payload);
                break;
            case MsgType::Ping:
                // Respond to any client (even inert ones) so liveness checks work.
                send_frame(c.sock, MsgType::Pong, std::string());
                break;
            default:
                // The layer only ever sends Hello/Ping/Pong; ignore the rest.
                break;
        }
    }
    return true;
}

void StateServer::handle_hello(Client& c, const std::string& payload) {
    // A second Hello on an already-classified connection is ignored.
    if (c.active || c.inert) return;

    std::string exe;
    try {
        nlohmann::json j = nlohmann::json::parse(payload);
        if (j.contains("exe") && j["exe"].is_string()) {
            exe = j["exe"].get<std::string>();
        }
    } catch (...) {
        // Malformed Hello: treat the exe as empty (won't match the denylist).
    }

    if (is_blocked_ && is_blocked_(exe)) {
        c.inert = true;
        send_frame(c.sock, MsgType::Disabled, std::string());
        return;
    }

    c.active = true;
    seed_client(c);
}

void StateServer::seed_client(Client& c) {
    std::string snap_json;
    to_json_str(current_, snap_json);
    send_frame(c.sock, MsgType::Snapshot, snap_json);

    for (const auto& [hash, entry] : avatars_) {
        send_frame(c.sock, MsgType::AvatarReady,
                   avatar_payload(hash, entry.path, entry.w, entry.h));
    }
}

void StateServer::set_snapshot(const Snapshot& s) { current_ = s; }

void StateServer::broadcast(const Snapshot& s) {
    current_ = s;
    std::string snap_json;
    to_json_str(s, snap_json);
    for (auto& [sock, c] : clients_) {
        if (c.active) send_frame(sock, MsgType::Snapshot, snap_json);
    }
}

void StateServer::broadcast_avatar(const std::string& hash, const std::string& path,
                                   uint32_t w, uint32_t h) {
    avatars_[hash] = AvatarEntry{path, w, h};
    const std::string payload = avatar_payload(hash, path, w, h);
    for (auto& [sock, c] : clients_) {
        if (c.active) send_frame(sock, MsgType::AvatarReady, payload);
    }
}

std::string StateServer::avatar_payload(const std::string& hash, const std::string& path,
                                        uint32_t w, uint32_t h) {
    nlohmann::json j = {
        {"hash", hash},
        {"path", path},
        {"w", w},
        {"h", h},
    };
    return j.dump();
}

void StateServer::send_frame(QLocalSocket* sock, MsgType type, const std::string& payload) {
    if (!sock) return;
    std::vector<uint8_t> frame;
    encode_frame(type, payload, frame);
    sock->write(reinterpret_cast<const char*>(frame.data()),
                static_cast<qint64>(frame.size()));
    // Push the bytes out promptly so a synchronously-blocking layer test sees
    // them without waiting for the next event-loop turn.
    sock->flush();
}

}  // namespace choir
