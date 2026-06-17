// fake_host — a standalone stand-in for the `choir` host (Task 12 harness).
//
// Impersonates the real StateServer on the local IPC socket so the injected
// overlay layer (Tasks 13-18) can be driven with deterministic state in tests
// and manual runs, without Discord. It is wire-compatible with the real host:
// same libchoir_ipc framing, same MsgType set, same Snapshot/AvatarReady JSON.
//
// On a client Hello it serves a fixed 3-participant voice channel ("Test Voice",
// Bob speaking, Carol self-muted) plus 3 solid-color 64x64 avatars written to the
// avatar cache dir. Qt-free (plain POSIX + libchoir_ipc + nlohmann json).
//
// Usage: fake_host [--socket PATH] [--cache-dir DIR] [--once]

#include "ipc/avatar_file.hpp"
#include "ipc/framing.hpp"
#include "ipc/paths.hpp"
#include "ipc/protocol.hpp"
#include "ipc/state.hpp"

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct TestAvatar {
    const char* hash;
    uint8_t r, g, b;
};
constexpr TestAvatar kAvatars[] = {
    {"avatarA", 0xE0, 0x40, 0x40},  // red
    {"avatarB", 0x40, 0xE0, 0x40},  // green
    {"avatarC", 0x40, 0x40, 0xE0},  // blue
};

bool write_test_avatar(const std::string& dir, const TestAvatar& a) {
    constexpr uint32_t W = 64, H = 64;
    std::vector<uint8_t> rgba(W * H * 4);
    for (size_t i = 0; i < W * H; ++i) {
        rgba[i * 4 + 0] = a.r;
        rgba[i * 4 + 1] = a.g;
        rgba[i * 4 + 2] = a.b;
        rgba[i * 4 + 3] = 0xFF;
    }
    const std::string path = (fs::path(dir) / (std::string(a.hash) + ".rgba")).string();
    return choir::write_avatar_rgba(path, W, H, rgba.data());
}

choir::Snapshot make_snapshot() {
    choir::Snapshot s;
    s.in_voice = true;
    s.channel_name = "Test Voice";
    s.revision = 1;

    choir::Participant p0;
    p0.user_id = "1"; p0.display_name = "Alice"; p0.avatar_hash = "avatarA";
    choir::Participant p1;
    p1.user_id = "2"; p1.display_name = "Bob"; p1.avatar_hash = "avatarB"; p1.speaking = true;
    choir::Participant p2;
    p2.user_id = "3"; p2.display_name = "Carol"; p2.avatar_hash = "avatarC"; p2.self_mute = true;
    s.participants = {p0, p1, p2};
    return s;
}

bool send_snapshot_and_avatars(int fd, const std::string& cache_dir) {
    std::string snap_json;
    choir::to_json_str(make_snapshot(), snap_json);
    if (!choir::write_frame(fd, choir::MsgType::Snapshot, snap_json)) return false;

    for (const auto& a : kAvatars) {
        nlohmann::json j;
        j["hash"] = a.hash;
        j["path"] = (fs::path(cache_dir) / (std::string(a.hash) + ".rgba")).string();
        j["w"] = 64;
        j["h"] = 64;
        if (!choir::write_frame(fd, choir::MsgType::AvatarReady, j.dump())) return false;
    }
    return true;
}

// Serve one connected client until it disconnects. Responds to Hello with the
// scripted snapshot + avatars, and to Ping with Pong.
void serve_client(int cfd, const std::string& cache_dir) {
    choir::FrameReader reader;
    bool seeded = false;
    for (;;) {
        if (!reader.feed(cfd)) break;  // EOF or hard error
        choir::DecodedFrame f;
        while (reader.next(f)) {
            switch (f.type) {
                case choir::MsgType::Hello:
                    if (!seeded) {
                        seeded = true;
                        std::printf("fake_host: client hello: %s\n", f.payload.c_str());
                        std::fflush(stdout);
                        if (!send_snapshot_and_avatars(cfd, cache_dir)) return;
                    }
                    break;
                case choir::MsgType::Ping:
                    choir::write_frame(cfd, choir::MsgType::Pong, f.payload);
                    break;
                default:
                    break;
            }
        }
        if (reader.failed()) break;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string socket_path = choir::runtime_socket_path();
    std::string cache_dir = choir::avatar_cache_dir();
    bool once = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc) socket_path = argv[++i];
        else if (std::strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) cache_dir = argv[++i];
        else if (std::strcmp(argv[i], "--once") == 0) once = true;
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::puts("fake_host [--socket PATH] [--cache-dir DIR] [--once]");
            return 0;
        }
    }

    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    for (const auto& a : kAvatars) {
        if (!write_test_avatar(cache_dir, a)) {
            std::fprintf(stderr, "fake_host: failed to write avatar %s\n", a.hash);
            return 1;
        }
    }

    if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        std::fprintf(stderr, "fake_host: socket path too long\n");
        return 1;
    }
    ::unlink(socket_path.c_str());

    int sfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { std::perror("socket"); return 1; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(sfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind"); ::close(sfd); return 1;
    }
    if (::listen(sfd, 8) < 0) { std::perror("listen"); ::close(sfd); return 1; }

    std::printf("fake_host: listening on %s (cache %s)\n", socket_path.c_str(), cache_dir.c_str());
    std::fflush(stdout);

    for (;;) {
        int cfd = ::accept(sfd, nullptr, nullptr);
        if (cfd < 0) { if (errno == EINTR) continue; std::perror("accept"); break; }
        serve_client(cfd, cache_dir);
        ::close(cfd);
        if (once) break;
    }

    ::close(sfd);
    ::unlink(socket_path.c_str());
    return 0;
}
