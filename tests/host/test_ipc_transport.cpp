// Test for choir::IpcTransport against an in-process mock Discord RPC server.
//
// The mock binds an AF_UNIX/SOCK_STREAM listener at "<tmp>/discord-ipc-0" with
// XDG_RUNTIME_DIR pointed at <tmp>, so IpcTransport::connect() discovers it via
// its discord-ipc-{0..9} probe. The mock then:
//   1. accepts the connection,
//   2. reads the handshake frame the transport sends (op 0),
//   3. replies op 1 with {"cmd":"DISPATCH","evt":"READY","data":{}},
//   4. later sends an op 3 PING with a payload and asserts it reads back an op 4
//      PONG echoing that same payload.
// The test drives IpcTransport::poll() in a loop (the real host pumps it via a
// QSocketNotifier on fd()).

#include "discord/ipc_transport.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using nlohmann::json;

namespace {

// Read exactly n bytes (blocking) from fd into out. Returns false on EOF/error.
bool read_exact(int fd, void* out, size_t n) {
    auto* p = static_cast<uint8_t*>(out);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, p + got, n - got);
        if (r > 0) {
            got += static_cast<size_t>(r);
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

// Read one op-framed message (8-byte LE [op][len] header + len JSON bytes).
bool read_frame(int fd, int32_t& op, std::string& payload) {
    int32_t hdr[2];
    if (!read_exact(fd, hdr, sizeof(hdr))) return false;
    op = hdr[0];
    int32_t len = hdr[1];
    if (len < 0) return false;
    payload.resize(static_cast<size_t>(len));
    if (len > 0 && !read_exact(fd, payload.data(), static_cast<size_t>(len))) return false;
    return true;
}

// Write one op-framed message.
bool write_frame(int fd, int32_t op, const std::string& payload) {
    int32_t hdr[2] = {op, static_cast<int32_t>(payload.size())};
    std::vector<uint8_t> buf(sizeof(hdr) + payload.size());
    std::memcpy(buf.data(), hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), payload.data(), payload.size());
    size_t off = 0;
    while (off < buf.size()) {
        ssize_t w = ::write(fd, buf.data() + off, buf.size() - off);
        if (w > 0) { off += static_cast<size_t>(w); continue; }
        if (w < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

} // namespace

int main() {
    using namespace choir;

    // Unique temp dir for XDG_RUNTIME_DIR.
    char tmpl[] = "/tmp/choir_ipc_test_XXXXXX";
    char* dir = ::mkdtemp(tmpl);
    assert(dir != nullptr);
    setenv("XDG_RUNTIME_DIR", dir, 1);

    std::string sock_path = std::string(dir) + "/discord-ipc-0";

    // Mock server listener.
    int lfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    assert(lfd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    assert(sock_path.size() < sizeof(addr.sun_path));
    std::strcpy(addr.sun_path, sock_path.c_str());
    ::unlink(sock_path.c_str());
    assert(::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(::listen(lfd, 1) == 0);

    std::atomic<bool> mock_ok{false};
    std::atomic<bool> pong_ok{false};

    std::thread mock([&] {
        int cfd = ::accept(lfd, nullptr, nullptr);
        assert(cfd >= 0);

        // 1. Read the handshake (op 0) the transport sends.
        int32_t op;
        std::string payload;
        bool got = read_frame(cfd, op, payload);
        assert(got);
        assert(op == 0);
        json hs = json::parse(payload);
        assert(hs.contains("v"));

        // 2. Reply with op 1 READY.
        json ready = {{"cmd", "DISPATCH"}, {"evt", "READY"}, {"data", json::object()}};
        assert(write_frame(cfd, 1, ready.dump()));

        // 3. Send a PING (op 3) with a distinctive payload; expect a matching PONG.
        json ping = {{"nonce", "ping-123"}};
        std::string ping_payload = ping.dump();
        assert(write_frame(cfd, 3, ping_payload));

        int32_t pop;
        std::string ppayload;
        bool gotpong = read_frame(cfd, pop, ppayload);
        assert(gotpong);
        assert(pop == 4);             // PONG opcode
        assert(ppayload == ping_payload); // echoes the PING payload
        pong_ok = true;

        mock_ok = true;
        ::close(cfd);
    });

    // Drive the transport.
    IpcTransport t;
    bool connected = t.connect();
    assert(connected);
    assert(t.fd() >= 0);

    std::atomic<bool> ready_seen{false};
    std::atomic<bool> closed_seen{false};
    t.set_handlers(
        [&](int op, const json& j) {
            assert(op == 1);
            assert(j.at("cmd") == "DISPATCH");
            assert(j.at("evt") == "READY");
            assert(j.at("data").is_object());
            ready_seen = true;
        },
        [&] { closed_seen = true; });

    // Send the handshake frame (op 0).
    json handshake = {{"v", 1}, {"client_id", "0"}};
    assert(t.send(0, handshake));

    // Pump poll() until we have both the READY callback and the PONG round-trip.
    for (int i = 0; i < 2000 && !(ready_seen && pong_ok); ++i) {
        t.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    mock.join();
    t.close();
    ::close(lfd);
    ::unlink(sock_path.c_str());
    ::rmdir(dir);

    assert(mock_ok);
    assert(ready_seen);
    assert(pong_ok);

    return 0;
}
