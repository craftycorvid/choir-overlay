#pragma once

// RPC message builders + event parsers (Task 4).
//
// Pure functions, no sockets and no clock:
//   - build_*()   construct the Discord RPC command JSON the host sends
//                 (AUTHORIZE / AUTHENTICATE / SUBSCRIBE), each with a unique nonce.
//   - parse_event() turns an inbound DISPATCH frame into a typed RpcEvent.
//
// These feed the RPC connection FSM (Task 6) and the state reducer (Task 8).
// parse_event() is fully defensive: malformed/garbage JSON never throws out of it;
// it returns std::nullopt instead.

#include "ipc/state.hpp"   // choir::Notification

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace choir {

// A participant's voice state as carried by VOICE_STATE_* dispatch events.
struct VoiceState {
    std::string user_id;
    std::string nick;        // nick, else username
    std::string avatar_hash; // Discord avatar hash ("" if null/absent)
    bool mute = false;       // server mute
    bool deaf = false;       // server deaf
    bool self_mute = false;
    bool self_deaf = false;
};

// A parsed inbound RPC event.
struct RpcEvent {
    enum Kind {
        VoiceCreate,
        VoiceUpdate,
        VoiceDelete,
        SpeakingStart,
        SpeakingStop,
        ChannelSelect,
        Notification,
    } kind;

    std::string channel_id;     // ChannelSelect (empty if null) / Speaking*
    VoiceState voice;           // Voice* events
    std::string user_id;        // Speaking* events
    choir::Notification notif;  // Notification events (created_ms left 0)
};

// --- Builders --------------------------------------------------------------
//
// Nonce scheme: a process-local monotonically-incrementing 64-bit counter,
// rendered as a decimal string and prefixed by a one-time random hex seed
// generated at first use ("<seed>-<counter>"). The seed disambiguates across
// process restarts / multiple instances; the counter guarantees uniqueness
// within a process. Simple, allocation-light, and collision-free per process.

// {"cmd":"AUTHORIZE","args":{"client_id":...,"scopes":[...],"prompt":"none"},"nonce":...}
nlohmann::json build_authorize(const std::vector<std::string>& scopes,
                               const std::string& client_id);

// {"cmd":"AUTHENTICATE","args":{"access_token":...},"nonce":...}
nlohmann::json build_authenticate(const std::string& access_token);

// {"cmd":"SUBSCRIBE","evt":evt,"args":args,"nonce":...}
nlohmann::json build_subscribe(const std::string& evt, const nlohmann::json& args);

// --- Parser ----------------------------------------------------------------

// Parse an inbound DISPATCH frame ({"cmd":"DISPATCH","evt":...,"data":{...}}).
// Returns std::nullopt for non-DISPATCH frames, unknown events, or anything
// malformed. Never throws.
std::optional<RpcEvent> parse_event(const nlohmann::json& frame);

} // namespace choir
