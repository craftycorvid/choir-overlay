#include "discord/rpc_messages.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace choir {

namespace {

using nlohmann::json;

// Unique-nonce generator. See the header for the scheme: "<seed>-<counter>".
// The seed is a random hex string chosen once per process; the counter is a
// monotonically-increasing atomic so concurrent callers never collide.
std::string next_nonce() {
    static const std::string seed = [] {
        std::random_device rd;
        std::uniform_int_distribution<uint64_t> dist;
        // Mix two draws so a weak random_device still yields a wide seed.
        uint64_t s = (static_cast<uint64_t>(dist(rd)) ^ (static_cast<uint64_t>(dist(rd)) << 1));
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(s));
        return std::string(buf);
    }();
    static std::atomic<uint64_t> counter{0};
    uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    return seed + "-" + std::to_string(n);
}

// Read a string field defensively: returns `dflt` if the key is missing or the
// value is not a string. A JSON null also yields `dflt`.
std::string str_or(const json& obj, const char* key, const std::string& dflt = "") {
    if (!obj.is_object()) return dflt;
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return dflt;
    return it->get<std::string>();
}

// Read a bool field defensively.
bool bool_or(const json& obj, const char* key, bool dflt = false) {
    if (!obj.is_object()) return dflt;
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) return dflt;
    return it->get<bool>();
}

// Return the sub-object at `key`, or a reference to a shared empty object if it
// is missing / not an object. Lets callers chain str_or/bool_or safely.
const json& obj_or_empty(const json& obj, const char* key) {
    static const json kEmpty = json::object();
    if (!obj.is_object()) return kEmpty;
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_object()) return kEmpty;
    return *it;
}

RpcEvent parse_voice(RpcEvent::Kind kind, const json& data) {
    RpcEvent ev;
    ev.kind = kind;

    const json& user = obj_or_empty(data, "user");
    const json& vs = obj_or_empty(data, "voice_state");

    ev.voice.user_id = str_or(user, "id");

    std::string nick = str_or(data, "nick");
    if (nick.empty()) nick = str_or(user, "username");
    ev.voice.nick = nick;

    // avatar may be a string or null; str_or yields "" for null/absent.
    ev.voice.avatar_hash = str_or(user, "avatar");

    ev.voice.mute = bool_or(vs, "mute");
    ev.voice.deaf = bool_or(vs, "deaf");
    ev.voice.self_mute = bool_or(vs, "self_mute");
    ev.voice.self_deaf = bool_or(vs, "self_deaf");
    return ev;
}

RpcEvent parse_speaking(RpcEvent::Kind kind, const json& data) {
    RpcEvent ev;
    ev.kind = kind;
    ev.channel_id = str_or(data, "channel_id");
    ev.user_id = str_or(data, "user_id");
    return ev;
}

RpcEvent parse_channel_select(const json& data) {
    RpcEvent ev;
    ev.kind = RpcEvent::ChannelSelect;
    // channel_id is a string or null; str_or yields "" for null/absent.
    ev.channel_id = str_or(data, "channel_id");
    return ev;
}

RpcEvent parse_notification(const json& data) {
    RpcEvent ev;
    ev.kind = RpcEvent::Notification;

    const json& message = obj_or_empty(data, "message");
    std::string id = str_or(message, "id");
    if (id.empty()) id = str_or(data, "channel_id");
    ev.notif.id = id;

    ev.notif.title = str_or(data, "title");
    ev.notif.body = str_or(data, "body");
    ev.notif.icon_hash = str_or(data, "icon_url");
    // created_ms left at its default 0 — the host stamps it on receipt.
    return ev;
}

} // namespace

nlohmann::json build_authorize(const std::vector<std::string>& scopes,
                               const std::string& client_id) {
    return json{
        {"cmd", "AUTHORIZE"},
        {"args", {
            {"client_id", client_id},
            {"scopes", scopes},
            {"prompt", "none"},
        }},
        {"nonce", next_nonce()},
    };
}

nlohmann::json build_authenticate(const std::string& access_token) {
    return json{
        {"cmd", "AUTHENTICATE"},
        {"args", {{"access_token", access_token}}},
        {"nonce", next_nonce()},
    };
}

nlohmann::json build_subscribe(const std::string& evt, const nlohmann::json& args) {
    return json{
        {"cmd", "SUBSCRIBE"},
        {"evt", evt},
        {"args", args},
        {"nonce", next_nonce()},
    };
}

nlohmann::json build_unsubscribe(const std::string& evt, const nlohmann::json& args) {
    return json{
        {"cmd", "UNSUBSCRIBE"},
        {"evt", evt},
        {"args", args},
        {"nonce", next_nonce()},
    };
}

std::optional<RpcEvent> parse_event(const nlohmann::json& frame) {
    if (!frame.is_object()) return std::nullopt;
    if (str_or(frame, "cmd") != "DISPATCH") return std::nullopt;

    std::string evt = str_or(frame, "evt");
    if (evt.empty()) return std::nullopt;

    const json& data = obj_or_empty(frame, "data");

    if (evt == "VOICE_STATE_CREATE") return parse_voice(RpcEvent::VoiceCreate, data);
    if (evt == "VOICE_STATE_UPDATE") return parse_voice(RpcEvent::VoiceUpdate, data);
    if (evt == "VOICE_STATE_DELETE") return parse_voice(RpcEvent::VoiceDelete, data);
    if (evt == "SPEAKING_START") return parse_speaking(RpcEvent::SpeakingStart, data);
    if (evt == "SPEAKING_STOP") return parse_speaking(RpcEvent::SpeakingStop, data);
    if (evt == "VOICE_CHANNEL_SELECT") return parse_channel_select(data);
    if (evt == "NOTIFICATION_CREATE") return parse_notification(data);

    return std::nullopt;
}

} // namespace choir
