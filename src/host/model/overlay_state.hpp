#pragma once

// Overlay state reducer (Task 8).
//
// OverlayState is a pure, Qt-free component that folds inbound RpcEvents (from
// the RPC client, Task 6) into the choir::Snapshot wire state (Task 2) that the
// host serves to the overlay layer. After each applied change it bumps
// snap_.revision and fires on_change(current()) exactly once.
//
// It owns no clock of its own: a now_ms() callable is injected (defaulting to
// real wall-clock milliseconds) so notification timestamps are deterministic in
// tests.
//
// Reduction is documented per-event in overlay_state.cpp. A no-op event (e.g.
// SpeakingStart for an unknown user, VoiceDelete of a missing user) does NOT
// bump the revision or fire on_change.

#include "discord/rpc_messages.hpp"  // choir::RpcEvent
#include "ipc/state.hpp"             // choir::Snapshot, AppearanceConfig

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace choir {

class OverlayState {
public:
    // now_ms defaults to real wall-clock milliseconds; inject for tests.
    explicit OverlayState(std::function<int64_t()> now_ms = {});

    // Fold one RpcEvent into the snapshot. If it changed state, bumps the
    // revision and fires on_change once (after the snapshot is updated).
    void apply(const RpcEvent& ev);

    // Replace the appearance config; always bumps revision + fires on_change.
    void set_config(const AppearanceConfig& cfg);

    const Snapshot& current() const { return snap_; }

    // Fired once per applied change, after the snapshot has been updated.
    std::function<void(const Snapshot&)> on_change;

private:
    // Mark a change as applied: bump revision and fire on_change.
    void commit();

    Snapshot snap_;
    std::unordered_map<std::string, size_t> index_;  // user_id -> participants index
    std::function<int64_t()> now_ms_;

    static constexpr size_t kMaxNotifications = 20;
};

}  // namespace choir
