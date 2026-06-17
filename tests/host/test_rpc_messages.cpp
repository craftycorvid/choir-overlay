// Tests for choir RPC message builders + event parsers (Task 4).
//
// These are pure functions: builders produce the Discord RPC command JSON the
// host sends; parse_event() turns inbound DISPATCH frames into typed RpcEvents.
// No sockets, no clock. Fixtures are realistic-shape inline JSON.

#include "discord/rpc_messages.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <optional>
#include <set>
#include <string>
#include <vector>

using nlohmann::json;
using namespace choir;

// ---- builders -------------------------------------------------------------

static void test_build_authorize() {
    std::vector<std::string> scopes = {"rpc", "rpc.voice.read"};
    json a = build_authorize(scopes, "1234567890");

    assert(a.at("cmd") == "AUTHORIZE");
    assert(a.at("args").at("client_id") == "1234567890");
    assert(a.at("args").at("scopes") == scopes);
    assert(a.at("args").at("prompt") == "none");
    assert(a.contains("nonce"));
    assert(a.at("nonce").is_string());
    assert(!a.at("nonce").get<std::string>().empty());

    // Two calls must produce distinct nonces.
    json b = build_authorize(scopes, "1234567890");
    assert(a.at("nonce") != b.at("nonce"));
}

static void test_build_authenticate() {
    json a = build_authenticate("tok_abc");
    assert(a.at("cmd") == "AUTHENTICATE");
    assert(a.at("args").at("access_token") == "tok_abc");
    assert(a.contains("nonce"));
    assert(a.at("nonce").is_string());

    json b = build_authenticate("tok_abc");
    assert(a.at("nonce") != b.at("nonce"));
}

static void test_build_subscribe() {
    json args = {{"channel_id", "987"}};
    json s = build_subscribe("VOICE_STATE_CREATE", args);
    assert(s.at("cmd") == "SUBSCRIBE");
    assert(s.at("evt") == "VOICE_STATE_CREATE");
    assert(s.at("args").at("channel_id") == "987");
    assert(s.contains("nonce"));
    assert(s.at("nonce").is_string());

    // Subscribe with empty args still valid.
    json s2 = build_subscribe("VOICE_CHANNEL_SELECT", json::object());
    assert(s2.at("cmd") == "SUBSCRIBE");
    assert(s2.at("evt") == "VOICE_CHANNEL_SELECT");
    assert(s.at("nonce") != s2.at("nonce"));
}

static void test_nonces_globally_unique() {
    // Mix builders; all nonces should be distinct.
    std::set<std::string> seen;
    auto add = [&](const json& j) {
        auto n = j.at("nonce").get<std::string>();
        assert(seen.insert(n).second && "nonce collision");
    };
    for (int i = 0; i < 50; ++i) {
        add(build_authorize({"rpc"}, "id"));
        add(build_authenticate("t"));
        add(build_subscribe("E", json::object()));
    }
}

// ---- parser: voice state --------------------------------------------------

static void test_parse_voice_state_update() {
    json frame = json::parse(R"({
        "cmd": "DISPATCH",
        "evt": "VOICE_STATE_UPDATE",
        "data": {
            "nick": "Corvid",
            "voice_state": {
                "mute": true,
                "deaf": false,
                "self_mute": false,
                "self_deaf": true,
                "suppress": false
            },
            "user": {
                "id": "111222333",
                "username": "corvid_raw",
                "avatar": "abc123hash"
            }
        }
    })");

    auto ev = parse_event(frame);
    assert(ev.has_value());
    assert(ev->kind == RpcEvent::VoiceUpdate);
    assert(ev->voice.user_id == "111222333");
    assert(ev->voice.nick == "Corvid");
    assert(ev->voice.avatar_hash == "abc123hash");
    assert(ev->voice.mute == true);
    assert(ev->voice.deaf == false);
    assert(ev->voice.self_mute == false);
    assert(ev->voice.self_deaf == true);
}

static void test_parse_voice_state_create() {
    json frame = json::parse(R"({
        "cmd": "DISPATCH",
        "evt": "VOICE_STATE_CREATE",
        "data": {
            "voice_state": {
                "mute": false, "deaf": false,
                "self_mute": true, "self_deaf": false
            },
            "user": {
                "id": "42",
                "username": "fallbackname",
                "avatar": null
            }
        }
    })");

    auto ev = parse_event(frame);
    assert(ev.has_value());
    assert(ev->kind == RpcEvent::VoiceCreate);
    assert(ev->voice.user_id == "42");
    // No nick -> fall back to username.
    assert(ev->voice.nick == "fallbackname");
    // Null avatar -> empty string.
    assert(ev->voice.avatar_hash == "");
    assert(ev->voice.self_mute == true);
    assert(ev->voice.mute == false);
}

static void test_parse_voice_state_delete() {
    json frame = json::parse(R"({
        "cmd": "DISPATCH",
        "evt": "VOICE_STATE_DELETE",
        "data": {
            "nick": "",
            "voice_state": { "mute": false, "deaf": false, "self_mute": false, "self_deaf": false },
            "user": { "id": "999", "username": "gone" }
        }
    })");

    auto ev = parse_event(frame);
    assert(ev.has_value());
    assert(ev->kind == RpcEvent::VoiceDelete);
    assert(ev->voice.user_id == "999");
    // Empty nick -> fall back to username.
    assert(ev->voice.nick == "gone");
}

// ---- parser: speaking -----------------------------------------------------

static void test_parse_speaking_start() {
    json frame = json::parse(R"({
        "cmd": "DISPATCH",
        "evt": "SPEAKING_START",
        "data": { "channel_id": "555", "user_id": "777" }
    })");
    auto ev = parse_event(frame);
    assert(ev.has_value());
    assert(ev->kind == RpcEvent::SpeakingStart);
    assert(ev->channel_id == "555");
    assert(ev->user_id == "777");
}

static void test_parse_speaking_stop() {
    json frame = json::parse(R"({
        "cmd": "DISPATCH",
        "evt": "SPEAKING_STOP",
        "data": { "channel_id": "555", "user_id": "777" }
    })");
    auto ev = parse_event(frame);
    assert(ev.has_value());
    assert(ev->kind == RpcEvent::SpeakingStop);
    assert(ev->channel_id == "555");
    assert(ev->user_id == "777");
}

// ---- parser: channel select ----------------------------------------------

static void test_parse_channel_select() {
    json frame = json::parse(R"({
        "cmd": "DISPATCH",
        "evt": "VOICE_CHANNEL_SELECT",
        "data": { "channel_id": "123456" }
    })");
    auto ev = parse_event(frame);
    assert(ev.has_value());
    assert(ev->kind == RpcEvent::ChannelSelect);
    assert(ev->channel_id == "123456");
}

static void test_parse_channel_select_null() {
    json frame = json::parse(R"({
        "cmd": "DISPATCH",
        "evt": "VOICE_CHANNEL_SELECT",
        "data": { "channel_id": null }
    })");
    auto ev = parse_event(frame);
    assert(ev.has_value());
    assert(ev->kind == RpcEvent::ChannelSelect);
    assert(ev->channel_id == "");
}

// ---- parser: notification -------------------------------------------------

static void test_parse_notification() {
    json frame = json::parse(R"({
        "cmd": "DISPATCH",
        "evt": "NOTIFICATION_CREATE",
        "data": {
            "channel_id": "chan9",
            "message": { "id": "msg42", "content": "hi" },
            "icon_url": "https://cdn.example/icon.png",
            "title": "New message",
            "body": "Hello world"
        }
    })");
    auto ev = parse_event(frame);
    assert(ev.has_value());
    assert(ev->kind == RpcEvent::Notification);
    assert(ev->notif.id == "msg42");
    assert(ev->notif.title == "New message");
    assert(ev->notif.body == "Hello world");
    assert(ev->notif.icon_hash == "https://cdn.example/icon.png");
    // Parser must NOT set a timestamp.
    assert(ev->notif.created_ms == 0);
}

static void test_parse_notification_no_message_id() {
    json frame = json::parse(R"({
        "cmd": "DISPATCH",
        "evt": "NOTIFICATION_CREATE",
        "data": {
            "channel_id": "chanFallback",
            "title": "T",
            "body": "B"
        }
    })");
    auto ev = parse_event(frame);
    assert(ev.has_value());
    assert(ev->kind == RpcEvent::Notification);
    // No message.id -> fall back to channel_id.
    assert(ev->notif.id == "chanFallback");
    assert(ev->notif.icon_hash == "");
}

// ---- parser: defensive / malformed ---------------------------------------

static void test_parse_non_dispatch() {
    json frame = json::parse(R"({ "cmd": "AUTHORIZE", "data": { "code": "x" } })");
    assert(!parse_event(frame).has_value());
}

static void test_parse_unknown_event() {
    json frame = json::parse(R"({ "cmd": "DISPATCH", "evt": "SOMETHING_ELSE", "data": {} })");
    assert(!parse_event(frame).has_value());
}

static void test_parse_missing_evt() {
    json frame = json::parse(R"({ "cmd": "DISPATCH", "data": {} })");
    assert(!parse_event(frame).has_value());
}

static void test_parse_missing_data() {
    // A voice event with no data object must not throw.
    json frame = json::parse(R"({ "cmd": "DISPATCH", "evt": "VOICE_STATE_CREATE" })");
    auto ev = parse_event(frame);
    // No data => no usable user.id; still must not throw. We accept either a
    // nullopt or an event with empty fields, but it must NOT throw.
    (void)ev;
}

static void test_parse_garbage_types() {
    // Fields present but wrong types should not throw.
    json frame = json::parse(R"({
        "cmd": "DISPATCH",
        "evt": "VOICE_STATE_UPDATE",
        "data": {
            "nick": 12345,
            "voice_state": "not_an_object",
            "user": { "id": 999, "username": null, "avatar": 7 }
        }
    })");
    auto ev = parse_event(frame);
    // Must not throw. Result is best-effort/defensive.
    (void)ev;
}

static void test_parse_not_object() {
    json frame = json::parse(R"([1, 2, 3])");
    assert(!parse_event(frame).has_value());

    json num = 42;
    assert(!parse_event(num).has_value());

    json nul = nullptr;
    assert(!parse_event(nul).has_value());
}

static void test_parse_speaking_missing_fields() {
    json frame = json::parse(R"({ "cmd": "DISPATCH", "evt": "SPEAKING_START", "data": {} })");
    auto ev = parse_event(frame);
    // Should still produce a SpeakingStart with empty ids, never throw.
    assert(ev.has_value());
    assert(ev->kind == RpcEvent::SpeakingStart);
    assert(ev->user_id == "");
    assert(ev->channel_id == "");
}

int main() {
    test_build_authorize();
    test_build_authenticate();
    test_build_subscribe();
    test_nonces_globally_unique();

    test_parse_voice_state_update();
    test_parse_voice_state_create();
    test_parse_voice_state_delete();

    test_parse_speaking_start();
    test_parse_speaking_stop();

    test_parse_channel_select();
    test_parse_channel_select_null();

    test_parse_notification();
    test_parse_notification_no_message_id();

    test_parse_non_dispatch();
    test_parse_unknown_event();
    test_parse_missing_evt();
    test_parse_missing_data();
    test_parse_garbage_types();
    test_parse_not_object();
    test_parse_speaking_missing_fields();

    return 0;
}
