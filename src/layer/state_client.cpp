#include "state_client.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "ipc/framing.hpp"
#include "ipc/paths.hpp"
#include "ipc/protocol.hpp"

namespace choir {
namespace {

// Reconnect backoff: start short, cap so an absent host never busy-loops. The client
// thread spends the backoff in poll() on the wake pipe, so shutdown stays prompt.
constexpr int kBackoffStartMs = 200;
constexpr int kBackoffMaxMs = 2000;

// Read the process exe basename for the Hello payload. Prefer /proc/self/comm (the
// kernel-truncated command name); fall back to "unknown".
std::string self_exe_name() {
    std::ifstream f("/proc/self/comm");
    std::string name;
    if (f && std::getline(f, name) && !name.empty()) return name;
    return "unknown";
}

// Build the Hello JSON: {"pid":<getpid>,"exe":"<comm>","proto":1}.
std::string hello_payload() {
    nlohmann::json j;
    j["pid"] = static_cast<int>(::getpid());
    j["exe"] = self_exe_name();
    j["proto"] = 1;
    return j.dump();
}

// Set CLOEXEC + non-blocking on an fd. Returns false on failure.
bool set_nonblock_cloexec(int fd) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl < 0 || ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return false;
    int fd_fl = ::fcntl(fd, F_GETFD, 0);
    if (fd_fl < 0 || ::fcntl(fd, F_SETFD, fd_fl | FD_CLOEXEC) < 0) return false;
    return true;
}

}  // namespace

StateClient& StateClient::instance() {
    // Function-local static: constructed on first use, destroyed at process exit
    // (its destructor joins the thread). Thread-safe init per the C++ standard.
    static StateClient s;
    return s;
}

StateClient::StateClient() {
    // Self-pipe to wake the client thread out of poll() at shutdown.
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
        wake_r_ = fds[0];
        wake_w_ = fds[1];
    }
    thread_ = std::thread([this] { run(); });
}

StateClient::~StateClient() {
    stop_.store(true, std::memory_order_release);
    // Nudge the wake pipe so the thread leaves poll() immediately.
    if (wake_w_ >= 0) {
        const char b = 1;
        ssize_t n = ::write(wake_w_, &b, 1);
        (void)n;
    }
    if (thread_.joinable()) thread_.join();
    if (wake_r_ >= 0) ::close(wake_r_);
    if (wake_w_ >= 0) ::close(wake_w_);
}

std::vector<AvatarReq> StateClient::drain_avatar_requests() {
    std::lock_guard<std::mutex> g(avatar_mutex_);
    std::vector<AvatarReq> out;
    out.swap(avatar_queue_);
    return out;
}

std::optional<AvatarReq> StateClient::avatar_for(const std::string& hash) const {
    std::lock_guard<std::mutex> g(avatar_mutex_);
    auto it = avatars_.find(hash);
    if (it == avatars_.end()) return std::nullopt;
    return it->second;
}

void StateClient::publish(std::shared_ptr<const Snapshot> snap) {
    latest_.store(std::move(snap));
}

void StateClient::enqueue_avatar(AvatarReq req) {
    std::lock_guard<std::mutex> g(avatar_mutex_);
    // RETAIN every AvatarReady (hash -> req) for the process lifetime so a recreated/
    // second swapchain can resolve it on demand via avatar_for() (the host won't replay
    // AvatarReady over the persistent connection). Latest announcement for a hash wins.
    avatars_[req.hash] = req;
    // De-dup the eager-load queue by hash: a repeated AvatarReady for the same hash need
    // not re-enqueue (the render thread caches by hash anyway, but this keeps it small).
    for (const auto& r : avatar_queue_)
        if (r.hash == req.hash) return;
    avatar_queue_.push_back(std::move(req));
}

// Sleep for `ms`, but wake early (and return false) if a shutdown was requested.
// Implemented as a poll() on the wake pipe so the destructor's write unblocks it.
static bool backoff_sleep(int wake_r, std::atomic<bool>& stop, int ms) {
    if (stop.load(std::memory_order_acquire)) return false;
    if (wake_r < 0) {
        // No self-pipe: fall back to a short bounded sleep, re-checking stop.
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return !stop.load(std::memory_order_acquire);
    }
    pollfd pfd{wake_r, POLLIN, 0};
    int r = ::poll(&pfd, 1, ms);
    if (r > 0 && (pfd.revents & POLLIN)) {
        // Drain the wake byte(s); we are shutting down.
        char buf[64];
        while (::read(wake_r, buf, sizeof(buf)) > 0) {}
        return false;
    }
    return !stop.load(std::memory_order_acquire);
}

int StateClient::connect_once() {
    const std::string path = runtime_socket_path();
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return -1;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (!set_nonblock_cloexec(fd)) {
        ::close(fd);
        return -1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) return fd;  // immediate connect (AF_UNIX usually does)

    if (errno == EINPROGRESS) {
        // Wait for the connection to complete (or shutdown), bounded.
        pollfd pfds[2];
        pfds[0] = {fd, POLLOUT, 0};
        pfds[1] = {wake_r_, POLLIN, 0};
        int r = ::poll(pfds, 2, 1000);
        if (r > 0 && (pfds[0].revents & POLLOUT)) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
                return fd;
        }
    }
    ::close(fd);
    return -1;
}

void StateClient::serve(int fd) {
    // Send Hello immediately on connect. write_frame retries EAGAIN, so it works on
    // our non-blocking fd for these tiny control frames.
    if (!write_frame(fd, MsgType::Hello, hello_payload())) {
        ::close(fd);
        return;
    }

    // Once-only debug dump of the first received snapshot (Task 16 test hook).
    const char* dump_path = ::getenv("CHOIR_DEBUG_DUMP");
    bool dumped = false;

    FrameReader reader;
    for (;;) {
        if (stop_.load(std::memory_order_acquire)) break;

        // Drain whatever frames are already buffered before blocking again.
        DecodedFrame f;
        while (reader.next(f)) {
            switch (f.type) {
                case MsgType::Snapshot: {
                    auto snap = std::make_shared<Snapshot>();
                    if (from_json_str(f.payload, *snap)) {
                        if (dump_path && !dumped) {
                            // Re-serialize via to_json_str (round-trips the parsed
                            // snapshot) and write once, so the test can assert the
                            // layer received the right data without pixels.
                            std::string out;
                            to_json_str(*snap, out);
                            std::ofstream of(dump_path, std::ios::binary | std::ios::trunc);
                            if (of) {
                                of << out;
                                dumped = true;
                            }
                        }
                        publish(std::shared_ptr<const Snapshot>(std::move(snap)));
                    }
                    break;
                }
                case MsgType::AvatarReady: {
                    AvatarReq req;
                    try {
                        auto j = nlohmann::json::parse(f.payload);
                        req.hash = j.value("hash", std::string{});
                        req.path = j.value("path", std::string{});
                        req.w = j.value("w", 0u);
                        req.h = j.value("h", 0u);
                    } catch (...) {
                        break;  // malformed AvatarReady: ignore, never crash
                    }
                    if (!req.hash.empty() && !req.path.empty()) enqueue_avatar(std::move(req));
                    break;
                }
                case MsgType::Disabled:
                    // Latch overlay off for this process. The render thread stops
                    // drawing; we keep the connection so the host can re-enable later
                    // (a future Snapshot would still arrive, but disabled() gates it).
                    disabled_.store(true, std::memory_order_release);
                    break;
                case MsgType::Ping:
                    write_frame(fd, MsgType::Pong, f.payload);
                    break;
                default:
                    break;  // Pong / Hello / unknown from host: ignore
            }
        }
        if (reader.failed()) break;

        // Block until the socket has data, the wake pipe fires (shutdown), or a
        // timeout (so we re-check stop_ even with no traffic).
        pollfd pfds[2];
        pfds[0] = {fd, POLLIN, 0};
        pfds[1] = {wake_r_, POLLIN, 0};
        int r = ::poll(pfds, 2, 500);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;  // hard poll error -> drop the connection
        }
        if (pfds[1].revents & POLLIN) break;  // shutdown requested
        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (!reader.feed(fd)) break;  // EOF / hard error -> reconnect
        }
    }
    ::close(fd);
}

void StateClient::run() {
    int backoff = kBackoffStartMs;
    while (!stop_.load(std::memory_order_acquire)) {
        int fd = connect_once();
        if (fd < 0) {
            // Host absent: back off and retry. The overlay simply draws nothing.
            if (!backoff_sleep(wake_r_, stop_, backoff)) break;
            backoff = backoff < kBackoffMaxMs ? backoff * 2 : kBackoffMaxMs;
            if (backoff > kBackoffMaxMs) backoff = kBackoffMaxMs;
            continue;
        }
        backoff = kBackoffStartMs;  // reset on a successful connect
        serve(fd);                  // returns when the connection dies or on shutdown
        if (stop_.load(std::memory_order_acquire)) break;
        // Connection dropped: brief pause before reconnecting (host may be restarting).
        if (!backoff_sleep(wake_r_, stop_, kBackoffStartMs)) break;
    }
}

}  // namespace choir
