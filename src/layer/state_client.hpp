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
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
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
    //
    // NOTE: this is a convenience for eager loading; the robust path is avatar_for()
    // below. Every AvatarReady is ALSO retained in a hash->AvatarReq map (see
    // avatar_for) so a recreated/second swapchain — which gets a fresh AvatarTextures
    // but no replayed AvatarReady frames over the persistent connection — can still
    // resolve avatars on demand by hash.
    std::vector<AvatarReq> drain_avatar_requests();

    // Look up the retained AvatarReq for a hash (every AvatarReady the host ever sent
    // is kept here, accumulated, mutex-guarded). The render thread calls this when a
    // participant's texture isn't loaded yet, then feeds the result to
    // AvatarTextures::get_or_load. Returns nullopt if the host never announced `hash`.
    // Thread-safe; safe to call from the render thread concurrently with the client
    // thread receiving more AvatarReady frames.
    std::optional<AvatarReq> avatar_for(const std::string& hash) const;

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

    // mutable so avatar_for() (a const accessor for the render thread) can lock it.
    mutable std::mutex avatar_mutex_;
    std::vector<AvatarReq> avatar_queue_;
    // Retained map of every AvatarReady ever received (hash -> req). Accumulates for
    // the process lifetime; the render thread resolves textures from it on demand so
    // avatars survive a swapchain recreate (the host won't re-send AvatarReady over the
    // persistent connection). Guarded by avatar_mutex_ alongside the drain queue.
    std::unordered_map<std::string, AvatarReq> avatars_;
};

}  // namespace choir
