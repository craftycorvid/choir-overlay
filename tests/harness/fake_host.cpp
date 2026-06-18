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
//                  [--corrupt-avatars] [--corrupt-snapshot] [--flap N]
//
// Fault-injection flags (Task 18):
//   --corrupt-avatars   write truncated/bad-magic .rgba files at the advertised paths
//                       so the layer's read_avatar_rgba fails -> placeholder, no crash.
//   --corrupt-snapshot  send a Snapshot frame whose payload is invalid JSON (the layer
//                       must drop it: never publish, keep presenting).
//   --flap N            after the initial snapshot, send N more snapshots alternating
//                       in_voice true/false (~every 8ms) to exercise show/hide without
//                       per-toggle overlay teardown/realloc.

#include "ipc/avatar_file.hpp"
#include "ipc/framing.hpp"
#include "ipc/paths.hpp"
#include "ipc/protocol.hpp"
#include "ipc/state.hpp"

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
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

// Runtime config (parsed from argv). The fault-injection knobs default off so the
// existing golden/state tests keep their well-behaved fake host.
struct Config {
    std::string name;  // abstract socket name
    std::string cache_dir;
    bool once = false;
    bool corrupt_avatars = false;
    bool corrupt_snapshot = false;
    bool all_speaking = false;  // mark every participant speaking (full-alpha indicators)
    int flap = 0;  // 0 = no flapping; N = N alternating in_voice snapshots after the first
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

// Fault injection: write a deliberately-corrupt .rgba at the avatar path so the layer's
// read_avatar_rgba rejects it (bad magic + far-too-short for the advertised 64x64).
// The layer must skip the texture (placeholder) and NOT cache the failed load.
bool write_corrupt_avatar(const std::string& dir, const TestAvatar& a) {
    const std::string path = (fs::path(dir) / (std::string(a.hash) + ".rgba")).string();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    // Wrong magic ("XXXX" instead of "CHAV") and only a few bytes — read_avatar_rgba
    // validates the magic and the width*height*4 size, so this fails cleanly.
    const char garbage[] = {'X', 'X', 'X', 'X', 0x01, 0x02, 0x03};
    f.write(garbage, sizeof(garbage));
    return static_cast<bool>(f);
}

choir::Snapshot make_snapshot(bool in_voice, uint64_t revision, bool all_speaking = false) {
    choir::Snapshot s;
    s.in_voice = in_voice;
    s.channel_name = "Test Voice";
    s.revision = revision;

    choir::Participant p0;
    p0.user_id = "1"; p0.display_name = "Alice"; p0.avatar_hash = "avatarA";
    p0.speaking = all_speaking;
    choir::Participant p1;
    p1.user_id = "2"; p1.display_name = "Bob"; p1.avatar_hash = "avatarB"; p1.speaking = true;
    choir::Participant p2;
    p2.user_id = "3"; p2.display_name = "Carol"; p2.avatar_hash = "avatarC"; p2.self_mute = true;
    p2.speaking = all_speaking;
    s.participants = {p0, p1, p2};
    return s;
}

bool send_snapshot_and_avatars(int fd, const Config& cfg) {
    if (cfg.corrupt_snapshot) {
        // Fault injection: a Snapshot frame whose payload is NOT valid JSON. The layer's
        // from_json_str must fail -> drop it (never publish) -> keep presenting.
        const std::string garbage = "{ this is not valid json ]]] \x01\x02";
        std::printf("fake_host: sending CORRUPT snapshot frame\n");
        std::fflush(stdout);
        if (!choir::write_frame(fd, choir::MsgType::Snapshot, garbage)) return false;
        // Don't bother with avatars; the test only checks the layer survives.
        return true;
    }

    std::string snap_json;
    choir::to_json_str(make_snapshot(/*in_voice=*/true, /*revision=*/1, cfg.all_speaking), snap_json);
    if (!choir::write_frame(fd, choir::MsgType::Snapshot, snap_json)) return false;

    for (const auto& a : kAvatars) {
        nlohmann::json j;
        j["hash"] = a.hash;
        j["path"] = (fs::path(cfg.cache_dir) / (std::string(a.hash) + ".rgba")).string();
        j["w"] = 64;
        j["h"] = 64;
        if (!choir::write_frame(fd, choir::MsgType::AvatarReady, j.dump())) return false;
    }
    return true;
}

// Send `count` snapshots alternating in_voice false/true so the overlay shows/hides
// repeatedly. Lazy-init is correct iff this allocates the overlay exactly once (on the
// first in_voice) and never tears down on a subsequent !in_voice.
bool send_flapping_snapshots(int fd, int count) {
    for (int i = 0; i < count; ++i) {
        const bool in_voice = (i % 2 == 1);  // false, true, false, true, ...
        std::string snap_json;
        choir::to_json_str(make_snapshot(in_voice, /*revision=*/2 + i), snap_json);
        if (!choir::write_frame(fd, choir::MsgType::Snapshot, snap_json)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    return true;
}

// Serve one connected client until it disconnects. Responds to Hello with the
// scripted snapshot + avatars, and to Ping with Pong.
void serve_client(int cfd, const Config& cfg) {
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
                        if (!send_snapshot_and_avatars(cfd, cfg)) return;
                        if (cfg.flap > 0 && !send_flapping_snapshots(cfd, cfg.flap)) return;
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
    Config cfg;
    cfg.name = choir::abstract_socket_name();
    cfg.cache_dir = choir::avatar_cache_dir();

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--name") == 0 && i + 1 < argc) cfg.name = argv[++i];
        else if (std::strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) cfg.cache_dir = argv[++i];
        else if (std::strcmp(argv[i], "--once") == 0) cfg.once = true;
        else if (std::strcmp(argv[i], "--corrupt-avatars") == 0) cfg.corrupt_avatars = true;
        else if (std::strcmp(argv[i], "--corrupt-snapshot") == 0) cfg.corrupt_snapshot = true;
        else if (std::strcmp(argv[i], "--all-speaking") == 0) cfg.all_speaking = true;
        else if (std::strcmp(argv[i], "--flap") == 0 && i + 1 < argc) cfg.flap = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::puts("fake_host [--name ABSTRACT_NAME] [--cache-dir DIR] [--once] "
                      "[--corrupt-avatars] [--corrupt-snapshot] [--all-speaking] [--flap N]");
            return 0;
        }
    }

    std::error_code ec;
    fs::create_directories(cfg.cache_dir, ec);
    for (const auto& a : kAvatars) {
        const bool ok = cfg.corrupt_avatars ? write_corrupt_avatar(cfg.cache_dir, a)
                                            : write_test_avatar(cfg.cache_dir, a);
        if (!ok) {
            std::fprintf(stderr, "fake_host: failed to write avatar %s\n", a.hash);
            return 1;
        }
    }

    // Abstract-namespace socket (matches the host StateServer + the layer client).
    sockaddr_un addr{};
    socklen_t addrlen = choir::make_abstract_addr(addr, cfg.name);
    if (addrlen == 0) { std::fprintf(stderr, "fake_host: socket name too long\n"); return 1; }

    int sfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { std::perror("socket"); return 1; }
    if (::bind(sfd, reinterpret_cast<sockaddr*>(&addr), addrlen) < 0) {
        std::perror("bind"); ::close(sfd); return 1;
    }
    if (::listen(sfd, 8) < 0) { std::perror("listen"); ::close(sfd); return 1; }

    std::printf("fake_host: listening on @%s (cache %s)%s%s%s\n", cfg.name.c_str(),
                cfg.cache_dir.c_str(),
                cfg.corrupt_avatars ? " [corrupt-avatars]" : "",
                cfg.corrupt_snapshot ? " [corrupt-snapshot]" : "",
                cfg.flap ? " [flap]" : "");
    std::fflush(stdout);

    for (;;) {
        int cfd = ::accept(sfd, nullptr, nullptr);
        if (cfd < 0) { if (errno == EINTR) continue; std::perror("accept"); break; }
        serve_client(cfd, cfg);
        ::close(cfd);
        if (cfg.once) break;
    }

    ::close(sfd);  // abstract socket: auto-released on close, no file to unlink
    return 0;
}
