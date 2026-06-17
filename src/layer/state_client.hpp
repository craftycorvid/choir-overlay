// Background local-IPC client for the Choir layer (Task 16).
//
// The layer is a read-only client of the host's unix socket
// (choir::runtime_socket_path()). A single background std::thread owns the socket:
// it connects (non-blocking, with backoff if the host is absent), sends a Hello
// frame, then loops reading frames via choir::FrameReader and dispatching them:
//   * Snapshot    -> parse + publish the latest snapshot LOCK-FREE for the render
//                    thread (std::atomic<std::shared_ptr<const Snapshot>>).
//   * AvatarReady -> enqueue an AvatarReq{hash,path,w,h} for the render thread to
//                    turn into a Vulkan texture (the client thread has no device).
//   * Disabled    -> latch the overlay off for this process (stop drawing).
//   * Ping        -> reply Pong.
// On EOF / a failed reader / any socket error it closes, backs off, and reconnects.
//
// PROCESS-SINGLETON. The socket connection is per-process (the layer is one client
// regardless of how many Vulkan devices/swapchains exist), so there is exactly one
// StateClient, obtained via StateClient::instance(). It is created on first use and
// torn down (its thread joined) at process exit. Owning it per-swapchain would open
// a redundant connection per swapchain and race on Hello; the singleton avoids that.
//
// Threading contract:
//   * The client thread does ONLY socket I/O + JSON parsing. It never touches Vulkan.
//   * The render thread (swapchain.cpp present hook) calls latest(), disabled(), and
//     drain_avatar_requests() — all safe to call concurrently with the client thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ipc/state.hpp"

namespace choir {

// A pending avatar-load request, published by the client thread and consumed by the
// render thread. Mirrors the AvatarReady frame's JSON ({hash,path,w,h}).
struct AvatarReq {
    std::string hash;
    std::string path;
    uint32_t w = 0;
    uint32_t h = 0;
};

class StateClient {
public:
    // The process-wide singleton. The first call starts the background thread. Never
    // returns null; safe to call from any thread.
    static StateClient& instance();

    StateClient(const StateClient&) = delete;
    StateClient& operator=(const StateClient&) = delete;

    // Latest published snapshot (may be null if nothing received yet). Lock-free read
    // for the render thread.
    std::shared_ptr<const Snapshot> latest() const { return latest_.load(); }

    // True once the host sent a Disabled frame for this process (denylisted). Latches:
    // once disabled, stays disabled for the process lifetime. The render thread must
    // draw nothing when this is true.
    bool disabled() const { return disabled_.load(std::memory_order_acquire); }

    // Return and clear the pending avatar-load requests. Called by the render thread,
    // which then creates the Vulkan textures (it has the device). Thread-safe.
    std::vector<AvatarReq> drain_avatar_requests();

private:
    StateClient();
    ~StateClient();

    void run();                 // client-thread entry
    int connect_once();         // returns a connected non-blocking fd, or -1
    void serve(int fd);         // read+dispatch frames until the connection dies
    void publish(std::shared_ptr<const Snapshot> snap);
    void enqueue_avatar(AvatarReq req);

    std::thread thread_;
    std::atomic<bool> stop_{false};
    // Self-pipe: writing to wake_w_ unblocks the poll() in the client thread for a
    // prompt shutdown (no waiting out a poll timeout).
    int wake_r_ = -1;
    int wake_w_ = -1;

    std::atomic<std::shared_ptr<const Snapshot>> latest_;
    std::atomic<bool> disabled_{false};

    std::mutex avatar_mutex_;
    std::vector<AvatarReq> avatar_queue_;
};

}  // namespace choir
