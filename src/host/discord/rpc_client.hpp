#pragma once

// RpcClient: the Discord local-RPC connection state machine (Task 6).
//
// Orchestrates the lower layers built in Tasks 3-5 into one FSM:
//
//   Disconnected --start()--> connect() -> HANDSHAKE        (Connecting)
//   READY        --> send AUTHORIZE                          (Authorizing)
//   AUTHORIZE resp --> exchange_code() -> AUTHENTICATE       (Authorizing)
//   AUTHENTICATE resp --> SUBSCRIBE top-level events         (Ready)
//   VOICE_CHANNEL_SELECT(id) --> (re)subscribe channel events (InChannel)
//   VOICE_CHANNEL_SELECT(null) --> back to Ready
//   transport OnClosed --> Disconnected, schedule reconnect
//
// Every parsed RpcEvent (parse_event) is forwarded to the event handler. The
// class is Qt-free: the host pumps poll() from a QSocketNotifier/timer, the test
// pumps it in a loop. poll() is cheap and non-blocking.
//
// All time goes through an injectable now_ms() seam so reconnect timing is
// deterministic in tests; it defaults to a real steady clock in milliseconds.

#include "discord/ipc_transport.hpp"
#include "discord/oauth.hpp"
#include "discord/rpc_messages.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace choir {

enum class ConnectionState { Disconnected, Connecting, Authorizing, Ready, InChannel };

struct RpcConfig {
    std::string client_id;       // e.g. Streamkit "207646673902501888"
    std::string client_secret;   // empty in Streamkit mode
    AuthMode auth_mode = AuthMode::Streamkit;
    std::vector<std::string> scopes = {"rpc", "rpc.notifications.read", "messages.read"};
    int64_t reconnect_delay_ms = 3000;
};

class RpcClient {
public:
    // now_ms defaults to a real steady clock in milliseconds when left empty.
    RpcClient(RpcConfig cfg, HttpPost& http, std::function<int64_t()> now_ms = {});

    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;

    void set_event_handler(std::function<void(const RpcEvent&)> h);
    void set_state_handler(std::function<void(ConnectionState)> h);  // optional, for UI

    void start();  // begin connecting
    void poll();   // pump transport + drive FSM + perform a due reconnect
    ConnectionState state() const { return state_; }
    void stop();   // graceful disconnect, no reconnect

private:
    void set_state(ConnectionState s);

    // Attempt a transport connect + handshake. On failure, schedule a reconnect.
    void try_connect();

    // Handle one inbound op-framed message from the transport.
    void on_message(int op, const nlohmann::json& j);
    // Handle an unexpected transport drop.
    void on_closed();

    // FSM steps keyed off command-response frames.
    void handle_authorize_response(const nlohmann::json& frame);
    void handle_authenticate_response(const nlohmann::json& frame);
    void handle_selected_voice_channel(const nlohmann::json& frame);
    void handle_channel_select(const std::string& channel_id);

    void subscribe_channel(const std::string& channel_id);
    void unsubscribe_channel(const std::string& channel_id);

    void schedule_reconnect();
    void clear_channel_state();

    RpcConfig cfg_;
    HttpPost& http_;
    std::function<int64_t()> now_ms_;

    IpcTransport transport_;
    ConnectionState state_ = ConnectionState::Disconnected;

    std::function<void(const RpcEvent&)> on_event_;
    std::function<void(ConnectionState)> on_state_;

    // The voice channel we currently hold channel-scoped subscriptions for
    // ("" = none). Tracked so a channel switch unsubscribes the old one first.
    std::string subscribed_channel_;

    bool stopped_ = false;            // stop() called: no reconnect
    bool reconnect_pending_ = false;  // a reconnect is scheduled
    int64_t reconnect_at_ms_ = 0;     // when the scheduled reconnect is due
    bool in_poll_ = false;            // re-entrancy guard for poll() (see poll())
};

}  // namespace choir
