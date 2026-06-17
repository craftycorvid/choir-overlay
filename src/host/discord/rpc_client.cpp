#include "discord/rpc_client.hpp"

#include <chrono>
#include <cstdio>
#include <utility>

namespace choir {

namespace {

using nlohmann::json;

// Discord RPC opcodes used by the FSM.
constexpr int kOpHandshake = 0;
constexpr int kOpFrame = 1;

// The channel-scoped events we (un)subscribe per voice channel.
constexpr const char* kChannelEvents[] = {
    "VOICE_STATE_CREATE", "VOICE_STATE_UPDATE", "VOICE_STATE_DELETE",
    "SPEAKING_START",     "SPEAKING_STOP",
};

// Read a string field defensively (missing/non-string/null -> "").
std::string str_or(const json& obj, const char* key) {
    if (!obj.is_object()) return {};
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

int64_t steady_now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

RpcClient::RpcClient(RpcConfig cfg, HttpPost& http, std::function<int64_t()> now_ms)
    : cfg_(std::move(cfg)),
      http_(http),
      now_ms_(now_ms ? std::move(now_ms) : std::function<int64_t()>(&steady_now_ms)) {
    transport_.set_handlers(
        [this](int op, const json& j) { on_message(op, j); },
        [this] { on_closed(); });
}

void RpcClient::set_event_handler(std::function<void(const RpcEvent&)> h) {
    on_event_ = std::move(h);
}

void RpcClient::set_state_handler(std::function<void(ConnectionState)> h) {
    on_state_ = std::move(h);
}

void RpcClient::set_state(ConnectionState s) {
    if (state_ == s) return;
    state_ = s;
    if (on_state_) on_state_(s);
}

void RpcClient::start() {
    stopped_ = false;
    reconnect_pending_ = false;
    try_connect();
}

void RpcClient::stop() {
    stopped_ = true;
    reconnect_pending_ = false;
    clear_channel_state();
    transport_.close();  // caller-initiated: does NOT fire OnClosed
    set_state(ConnectionState::Disconnected);
}

void RpcClient::try_connect() {
    reconnect_pending_ = false;
    clear_channel_state();

    if (!transport_.connect()) {
        // Could not reach any discord-ipc-{0..9}; back off and retry.
        schedule_reconnect();
        return;
    }

    // Connected: send HANDSHAKE and move to Connecting.
    set_state(ConnectionState::Connecting);
    json handshake = {{"v", 1}, {"client_id", cfg_.client_id}};
    if (!transport_.send(kOpHandshake, handshake)) {
        // send() failure already closed the transport and fired OnClosed, which
        // schedules a reconnect; nothing more to do here.
        return;
    }
}

void RpcClient::poll() {
    // Drive a due reconnect first so a fresh connection is established before we
    // try to read from it.
    if (reconnect_pending_ && !stopped_ && now_ms_() >= reconnect_at_ms_) {
        try_connect();
    }
    // Pump the transport (reads available bytes, dispatches messages, auto-PONG).
    transport_.poll();
}

void RpcClient::on_message(int op, const json& j) {
    if (op != kOpFrame) return;  // only FRAME (op 1) carries RPC commands/events
    if (!j.is_object()) return;

    const std::string cmd = str_or(j, "cmd");

    // Command-response correlation by cmd. Discord echoes the cmd in the reply.
    if (cmd == "AUTHORIZE") {
        handle_authorize_response(j);
        return;
    }
    if (cmd == "AUTHENTICATE") {
        handle_authenticate_response(j);
        return;
    }
    if (cmd == "DISPATCH") {
        const std::string evt = str_or(j, "evt");

        // READY -> begin the OAuth handshake.
        if (evt == "READY") {
            set_state(ConnectionState::Authorizing);
            transport_.send(kOpFrame, build_authorize(cfg_.scopes, cfg_.client_id));
            return;
        }

        // Parse and forward every event we understand.
        if (auto ev = parse_event(j)) {
            if (on_event_) on_event_(*ev);
            if (ev->kind == RpcEvent::ChannelSelect) {
                handle_channel_select(ev->channel_id);
            }
        }
        return;
    }

    // Other command replies (SUBSCRIBE/UNSUBSCRIBE acks, errors) are ignored at
    // the FSM level for now; the M0 spike (Task 7) validates real wire shapes.
}

void RpcClient::handle_authorize_response(const json& frame) {
    if (state_ != ConnectionState::Authorizing) return;  // unexpected / stale

    const json& data = frame.is_object() && frame.contains("data") ? frame.at("data")
                                                                    : json::object();
    const std::string code = str_or(data, "code");
    if (code.empty()) {
        std::fprintf(stderr, "[choir] AUTHORIZE response missing code; reconnecting\n");
        transport_.close();
        schedule_reconnect();
        set_state(ConnectionState::Disconnected);
        return;
    }

    TokenResult tok =
        exchange_code(http_, cfg_.auth_mode, code, cfg_.client_id, cfg_.client_secret);
    if (!tok.ok) {
        std::fprintf(stderr, "[choir] token exchange failed: %s; reconnecting\n",
                     tok.error.c_str());
        transport_.close();
        schedule_reconnect();
        set_state(ConnectionState::Disconnected);
        return;
    }

    transport_.send(kOpFrame, build_authenticate(tok.access_token));
}

void RpcClient::handle_authenticate_response(const json& frame) {
    if (state_ != ConnectionState::Authorizing) return;  // unexpected / stale

    // Defensive: a top-level "evt" of "ERROR" (Discord's error envelope shape)
    // means authentication failed -> reconnect.
    if (str_or(frame, "evt") == "ERROR") {
        std::fprintf(stderr, "[choir] AUTHENTICATE returned ERROR; reconnecting\n");
        transport_.close();
        schedule_reconnect();
        set_state(ConnectionState::Disconnected);
        return;
    }

    // Success -> Ready, then subscribe to the top-level events.
    set_state(ConnectionState::Ready);
    transport_.send(kOpFrame, build_subscribe("VOICE_CHANNEL_SELECT", json::object()));
    transport_.send(kOpFrame, build_subscribe("NOTIFICATION_CREATE", json::object()));
}

void RpcClient::handle_channel_select(const std::string& channel_id) {
    if (channel_id.empty()) {
        // Left the channel: drop channel-scoped subscriptions, back to Ready.
        if (!subscribed_channel_.empty()) {
            unsubscribe_channel(subscribed_channel_);
            subscribed_channel_.clear();
        }
        set_state(ConnectionState::Ready);
        return;
    }

    if (channel_id == subscribed_channel_) {
        // Already subscribed to this channel; just (re)affirm InChannel.
        set_state(ConnectionState::InChannel);
        return;
    }

    // Switched channels: unsubscribe the previous one first, then subscribe new.
    if (!subscribed_channel_.empty()) {
        unsubscribe_channel(subscribed_channel_);
    }
    subscribe_channel(channel_id);
    subscribed_channel_ = channel_id;
    set_state(ConnectionState::InChannel);
}

void RpcClient::subscribe_channel(const std::string& channel_id) {
    const json args = {{"channel_id", channel_id}};
    for (const char* evt : kChannelEvents) {
        transport_.send(kOpFrame, build_subscribe(evt, args));
    }
}

void RpcClient::unsubscribe_channel(const std::string& channel_id) {
    const json args = {{"channel_id", channel_id}};
    for (const char* evt : kChannelEvents) {
        transport_.send(kOpFrame, build_unsubscribe(evt, args));
    }
}

void RpcClient::on_closed() {
    // Unexpected transport drop. Reset channel state and schedule a reconnect.
    clear_channel_state();
    set_state(ConnectionState::Disconnected);
    if (!stopped_) schedule_reconnect();
}

void RpcClient::schedule_reconnect() {
    if (stopped_) return;
    reconnect_pending_ = true;
    reconnect_at_ms_ = now_ms_() + cfg_.reconnect_delay_ms;
}

void RpcClient::clear_channel_state() {
    subscribed_channel_.clear();
}

}  // namespace choir
