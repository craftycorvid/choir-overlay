// Integration test for StateServer (Task 11).
//
// StateServer is the host-side QLocalServer that serves Snapshot/AvatarReady
// frames to connecting layer clients and applies the denylist per process.
//
// This test drives it with a real Qt event loop (QCoreApplication) and a RAW
// POSIX AF_UNIX/SOCK_STREAM client (NOT QLocalSocket) — that proves the layer
// (Task 16, plain POSIX) can connect to the QLocalServer socket and that both
// sides speak the same libchoir_ipc framing.
//
// Scenarios:
//   1. Hello{exe:"MyGame"} (allowed)   -> Snapshot frame seeded with current state.
//   2. Hello{exe:"steam"}  (denylisted) -> Disabled frame, never a Snapshot.
//   3. broadcast(snapshot)             -> active client receives a Snapshot frame.
//   4. broadcast_avatar(...)           -> active client receives an AvatarReady frame.
//   5. Hello for an allowed client also delivers AvatarReady for known avatars.

#include "config/denylist.hpp"
#include "ipc/paths.hpp"
#include "ipc/protocol.hpp"
#include "ipc/state.hpp"
#include "server/state_server.hpp"

#include <nlohmann/json.hpp>

#include <QCoreApplication>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace choir;

namespace {

// Connect a raw AF_UNIX/SOCK_STREAM client to `path`. Returns the fd or -1.
int raw_connect(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Send a Hello frame with the given exe over the raw fd.
void send_hello(int fd, const std::string& exe) {
    nlohmann::json j = {{"pid", 4242}, {"exe", exe}, {"proto", 1}};
    std::vector<uint8_t> frame;
    encode_frame(MsgType::Hello, j.dump(), frame);
    ssize_t n = ::write(fd, frame.data(), frame.size());
    assert(n == static_cast<ssize_t>(frame.size()));
}

// Pump the Qt event loop and the socket until a full frame is decoded from `fd`
// or the deadline passes. Accumulates partial reads in `buf`. Returns true and
// fills `out` on success; false on timeout.
bool wait_for_frame(int fd, std::vector<uint8_t>& buf, DecodedFrame& out, int timeout_ms) {
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        // Try to decode from whatever we already have.
        bool err = false;
        size_t consumed = try_decode_frame(buf.data(), buf.size(), out, &err);
        assert(!err && "protocol error decoding server frame");
        if (consumed > 0) {
            buf.erase(buf.begin(), buf.begin() + consumed);
            return true;
        }
        if (clock::now() >= deadline) return false;

        // Let the server process its side.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

        // Drain any available bytes (socket is blocking; use a tiny MSG_DONTWAIT read).
        uint8_t tmp[4096];
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n > 0) {
            buf.insert(buf.end(), tmp, tmp + n);
        } else if (n == 0) {
            return false;  // peer closed
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // Isolate the runtime dir so we get a clean choir.sock under our control.
    char tmpl[] = "/tmp/choir_ss_test_XXXXXX";
    char* dir = ::mkdtemp(tmpl);
    assert(dir != nullptr);
    ::setenv("XDG_RUNTIME_DIR", dir, 1);

    const std::string sock = runtime_socket_path();

    Denylist denylist({"steam", "discord"});
    StateServer server([&denylist](const std::string& exe) { return denylist.blocks(exe); });

    bool ok = server.listen();
    assert(ok && "StateServer failed to listen");

    // Seed an initial snapshot + a known avatar.
    Snapshot snap;
    snap.in_voice = true;
    snap.channel_name = "General";
    Participant p;
    p.user_id = "111";
    p.display_name = "Alice";
    p.avatar_hash = "abc";
    p.speaking = true;
    snap.participants.push_back(p);
    snap.revision = 7;
    server.set_snapshot(snap);
    server.broadcast_avatar("abc", "/tmp/abc.rgba", 64, 64);

    // --- Scenario 1: allowed exe receives a Snapshot (and AvatarReady). ---
    {
        int fd = raw_connect(sock);
        assert(fd >= 0 && "raw AF_UNIX connect to QLocalServer socket failed");
        send_hello(fd, "MyGame");

        std::vector<uint8_t> buf;
        DecodedFrame f;
        bool got = wait_for_frame(fd, buf, f, 2000);
        assert(got && "no frame from server for allowed Hello");
        assert(f.type == MsgType::Snapshot && "allowed client should get a Snapshot first");

        Snapshot recv;
        assert(from_json_str(f.payload, recv));
        assert(recv.in_voice == true);
        assert(recv.channel_name == "General");
        assert(recv.participants.size() == 1);
        assert(recv.participants[0].display_name == "Alice");
        assert(recv.revision == 7);

        // Then the AvatarReady for the known avatar "abc".
        bool got2 = wait_for_frame(fd, buf, f, 2000);
        assert(got2 && "expected AvatarReady after Snapshot");
        assert(f.type == MsgType::AvatarReady);
        nlohmann::json av = nlohmann::json::parse(f.payload);
        assert(av["hash"].get<std::string>() == "abc");
        assert(av["w"].get<int>() == 64);

        // --- Scenario 3: broadcast a new snapshot, this active client gets it. ---
        Snapshot snap2 = snap;
        snap2.channel_name = "Gaming";
        snap2.revision = 8;
        server.set_snapshot(snap2);
        server.broadcast(snap2);

        bool got3 = wait_for_frame(fd, buf, f, 2000);
        assert(got3 && "active client did not receive broadcast Snapshot");
        assert(f.type == MsgType::Snapshot);
        Snapshot recv2;
        assert(from_json_str(f.payload, recv2));
        assert(recv2.channel_name == "Gaming");
        assert(recv2.revision == 8);

        // --- Scenario 4: broadcast_avatar reaches the active client. ---
        server.broadcast_avatar("def", "/tmp/def.rgba", 32, 48);
        bool got4 = wait_for_frame(fd, buf, f, 2000);
        assert(got4 && "active client did not receive broadcast_avatar");
        assert(f.type == MsgType::AvatarReady);
        nlohmann::json av2 = nlohmann::json::parse(f.payload);
        assert(av2["hash"].get<std::string>() == "def");
        assert(av2["w"].get<int>() == 32);
        assert(av2["h"].get<int>() == 48);
        assert(av2["path"].get<std::string>() == "/tmp/def.rgba");

        ::close(fd);
    }

    // --- Scenario 2: denylisted exe gets Disabled, never a Snapshot. ---
    {
        int fd = raw_connect(sock);
        assert(fd >= 0);
        send_hello(fd, "steam");

        std::vector<uint8_t> buf;
        DecodedFrame f;
        bool got = wait_for_frame(fd, buf, f, 2000);
        assert(got && "no frame from server for denylisted Hello");
        assert(f.type == MsgType::Disabled && "denylisted client should get Disabled");

        // A subsequent broadcast must NOT reach this inert client.
        Snapshot snap3 = snap;
        snap3.channel_name = "ShouldNotArrive";
        server.broadcast(snap3);

        bool more = wait_for_frame(fd, buf, f, 300);
        assert(!more && "denylisted client must not receive broadcasts");

        ::close(fd);
    }

    std::printf("test_state_server: all scenarios passed\n");
    return 0;
}
