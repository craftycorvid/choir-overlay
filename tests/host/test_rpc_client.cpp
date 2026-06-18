// End-to-end test for choir::RpcClient (Task 6).
//
// Drives the full connection FSM against a scripted mock Discord RPC server
// bound at "<tmp>/discord-ipc-0" (XDG_RUNTIME_DIR=<tmp>), op/len framed exactly
// like Task 3's transport test. The mock scripts the handshake:
//
//   accept -> read HANDSHAKE(op 0)
//          -> send READY (op 1, DISPATCH/READY)
//          -> expect AUTHORIZE,    reply {cmd:AUTHORIZE,data:{code:"C"}}
//          -> expect AUTHENTICATE, reply {cmd:AUTHENTICATE,data:{}}
//          -> expect SUBSCRIBE(VOICE_CHANNEL_SELECT)
//          -> (also expect SUBSCRIBE(NOTIFICATION_CREATE))
//          -> push DISPATCH VOICE_CHANNEL_SELECT {channel_id:"123"}
//          -> expect SUBSCRIBE(VOICE_STATE_CREATE, args.channel_id=="123")
//          -> push DISPATCH VOICE_STATE_CREATE
//
// A FakeHttp injects the token exchange (returns access_token:"abc"). The FSM is
// driven by repeatedly calling client.poll() with a controllable now_ms clock.
//
// A second sub-test verifies reconnect: after the socket closes the client goes
// Disconnected, and once the injected clock passes reconnect_delay_ms a fresh
// connection attempt lands on the mock's listener.

#include "discord/oauth.hpp"
#include "discord/rpc_client.hpp"
#include "discord/rpc_messages.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using nlohmann::json;
using namespace choir;

namespace {

// ---- op/len framing helpers (mock-server side) ----------------------------

bool read_exact(int fd, void* out, size_t n) {
    auto* p = static_cast<uint8_t*>(out);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, p + got, n - got);
        if (r > 0) {
            got += static_cast<size_t>(r);
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool read_frame(int fd, int32_t& op, std::string& payload) {
    int32_t hdr[2];
    if (!read_exact(fd, hdr, sizeof(hdr))) return false;
    op = hdr[0];
    int32_t len = hdr[1];
    if (len < 0) return false;
    payload.resize(static_cast<size_t>(len));
    if (len > 0 && !read_exact(fd, payload.data(), static_cast<size_t>(len))) return false;
    return true;
}

bool write_frame(int fd, int32_t op, const std::string& payload) {
    int32_t hdr[2] = {op, static_cast<int32_t>(payload.size())};
    std::vector<uint8_t> buf(sizeof(hdr) + payload.size());
    std::memcpy(buf.data(), hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), payload.data(), payload.size());
    size_t off = 0;
    while (off < buf.size()) {
        ssize_t w = ::write(fd, buf.data() + off, buf.size() - off);
        if (w > 0) { off += static_cast<size_t>(w); continue; }
        if (w < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

// FakeHttp: returns a canned token regardless of request (mirrors Task 5).
struct FakeHttp : HttpPost {
    HttpResponse resp{200, R"({"access_token":"abc"})"};
    std::atomic<int> call_count{0};
    std::string last_code;

    HttpResponse post(const std::string&,
                      const std::vector<std::pair<std::string, std::string>>& form,
                      const std::vector<std::pair<std::string, std::string>>&) override {
        ++call_count;
        for (const auto& kv : form) {
            if (kv.first == "code") last_code = kv.second;
        }
        return resp;
    }

    // Streamkit mode (the RpcConfig default) goes through post_json; pull the
    // code out of the JSON body so last_code still reflects the exchanged code.
    HttpResponse post_json(const std::string&, const std::string& json_body,
                           const std::vector<std::pair<std::string, std::string>>&) override {
        ++call_count;
        auto j = nlohmann::json::parse(json_body, nullptr, /*allow_exceptions=*/false);
        if (j.is_object() && j.contains("code") && j["code"].is_string())
            last_code = j["code"].get<std::string>();
        return resp;
    }
};

// Make a fresh listener bound at <dir>/discord-ipc-0.
int make_listener(const std::string& sock_path) {
    int lfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    assert(lfd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    assert(sock_path.size() < sizeof(addr.sun_path));
    std::strcpy(addr.sun_path, sock_path.c_str());
    ::unlink(sock_path.c_str());
    assert(::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(::listen(lfd, 4) == 0);
    return lfd;
}

// ---- Main handshake test --------------------------------------------------

void test_full_handshake_to_in_channel(int lfd) {
    // Records what the mock observed.
    std::atomic<bool> saw_authorize{false};
    std::atomic<bool> saw_authenticate{false};
    std::atomic<bool> saw_sub_voice_channel_select{false};
    std::atomic<bool> saw_sub_notification_create{false};
    std::atomic<bool> saw_sub_voice_state_create{false};
    std::atomic<bool> voice_state_create_channel_ok{false};
    std::atomic<bool> mock_done{false};

    std::thread mock([&] {
        int cfd = ::accept(lfd, nullptr, nullptr);
        assert(cfd >= 0);

        // 1. Read HANDSHAKE (op 0).
        int32_t op;
        std::string payload;
        assert(read_frame(cfd, op, payload));
        assert(op == 0);
        json hs = json::parse(payload);
        assert(hs.at("v") == 1);
        assert(hs.contains("client_id"));

        // 2. Send READY (op 1).
        json ready = {{"cmd", "DISPATCH"}, {"evt", "READY"}, {"data", json::object()}};
        assert(write_frame(cfd, 1, ready.dump()));

        // 3. Expect AUTHORIZE; reply with a code.
        assert(read_frame(cfd, op, payload));
        json auth = json::parse(payload);
        assert(auth.at("cmd") == "AUTHORIZE");
        saw_authorize = true;
        json auth_reply = {{"cmd", "AUTHORIZE"}, {"data", {{"code", "C"}}}};
        if (auth.contains("nonce")) auth_reply["nonce"] = auth.at("nonce");
        assert(write_frame(cfd, 1, auth_reply.dump()));

        // 4. Expect AUTHENTICATE; reply success.
        assert(read_frame(cfd, op, payload));
        json authn = json::parse(payload);
        assert(authn.at("cmd") == "AUTHENTICATE");
        assert(authn.at("args").at("access_token") == "abc");
        saw_authenticate = true;
        json authn_reply = {{"cmd", "AUTHENTICATE"}, {"data", json::object()}};
        if (authn.contains("nonce")) authn_reply["nonce"] = authn.at("nonce");
        assert(write_frame(cfd, 1, authn_reply.dump()));

        // 5. Expect the two top-level SUBSCRIBEs + the on-connect
        //    GET_SELECTED_VOICE_CHANNEL query (order not guaranteed; read three).
        //    Reply data:null (NOT already in a channel) so this test exercises the
        //    event-driven join path below.
        std::string get_nonce;
        for (int i = 0; i < 3; ++i) {
            assert(read_frame(cfd, op, payload));
            json m = json::parse(payload);
            const std::string c = m.value("cmd", std::string());
            if (c == "SUBSCRIBE") {
                std::string evt = m.at("evt");
                if (evt == "VOICE_CHANNEL_SELECT") saw_sub_voice_channel_select = true;
                else if (evt == "NOTIFICATION_CREATE") saw_sub_notification_create = true;
            } else if (c == "GET_SELECTED_VOICE_CHANNEL") {
                get_nonce = m.value("nonce", std::string());
            }
        }
        {
            json get_reply = {{"cmd", "GET_SELECTED_VOICE_CHANNEL"}, {"data", nullptr}};
            if (!get_nonce.empty()) get_reply["nonce"] = get_nonce;
            assert(write_frame(cfd, 1, get_reply.dump()));
        }

        // 6. Push a VOICE_CHANNEL_SELECT for channel "123".
        json sel = {{"cmd", "DISPATCH"}, {"evt", "VOICE_CHANNEL_SELECT"},
                    {"data", {{"channel_id", "123"}}}};
        assert(write_frame(cfd, 1, sel.dump()));

        // 7. Expect the channel-scoped SUBSCRIBEs. The FSM sends five
        //    (VOICE_STATE_CREATE/UPDATE/DELETE, SPEAKING_START/STOP). Read until we
        //    have seen VOICE_STATE_CREATE with args.channel_id=="123".
        for (int i = 0; i < 5; ++i) {
            assert(read_frame(cfd, op, payload));
            json sub = json::parse(payload);
            assert(sub.at("cmd") == "SUBSCRIBE");
            std::string evt = sub.at("evt");
            if (evt == "VOICE_STATE_CREATE") {
                saw_sub_voice_state_create = true;
                if (sub.at("args").at("channel_id") == "123") {
                    voice_state_create_channel_ok = true;
                }
            }
        }

        // 8. Push a VOICE_STATE_CREATE for a participant in channel 123.
        json vsc = {{"cmd", "DISPATCH"}, {"evt", "VOICE_STATE_CREATE"},
                    {"data", {
                        {"user", {{"id", "u9"}, {"username", "Alice"}, {"avatar", "ah"}}},
                        {"nick", "Ali"},
                        {"voice_state", {{"mute", false}, {"deaf", false},
                                         {"self_mute", true}, {"self_deaf", false}}},
                    }}};
        assert(write_frame(cfd, 1, vsc.dump()));

        // Keep the connection open until the driver tears down. Block on a read;
        // it returns when the client closes.
        char drain[64];
        ::read(cfd, drain, sizeof(drain));
        ::close(cfd);
        mock_done = true;
    });

    // ---- Driver side --------------------------------------------------------
    FakeHttp http;

    RpcConfig cfg;
    cfg.client_id = "207646673902501888";
    cfg.auth_mode = AuthMode::Streamkit;
    cfg.reconnect_delay_ms = 3000;

    int64_t fake_now = 0;
    RpcClient client(cfg, http, [&] { return fake_now; });

    std::atomic<bool> got_voice_state_create{false};
    std::atomic<bool> got_channel_select{false};
    std::string created_user_id;
    bool created_self_mute = false;

    client.set_event_handler([&](const RpcEvent& ev) {
        if (ev.kind == RpcEvent::ChannelSelect) got_channel_select = true;
        if (ev.kind == RpcEvent::VoiceCreate) {
            got_voice_state_create = true;
            created_user_id = ev.voice.user_id;
            created_self_mute = ev.voice.self_mute;
        }
    });

    ConnectionState last_state = ConnectionState::Disconnected;
    client.set_state_handler([&](ConnectionState s) { last_state = s; });

    client.start();

    // Pump until we reach InChannel and the voice event arrived.
    for (int i = 0; i < 5000 && !(client.state() == ConnectionState::InChannel &&
                                  got_voice_state_create); ++i) {
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    assert(saw_authorize);
    assert(saw_authenticate);
    assert(saw_sub_voice_channel_select);
    assert(saw_sub_notification_create);
    assert(saw_sub_voice_state_create);
    assert(voice_state_create_channel_ok);

    assert(http.call_count == 1);
    assert(http.last_code == "C");

    assert(got_channel_select);
    assert(got_voice_state_create);
    assert(created_user_id == "u9");
    assert(created_self_mute == true);

    assert(client.state() == ConnectionState::InChannel);
    assert(last_state == ConnectionState::InChannel);

    client.stop();
    mock.join();
    assert(mock_done);
}

// ---- Already-in-channel-on-connect test -----------------------------------
//
// If the user is ALREADY in a voice channel when the client connects, no
// VOICE_CHANNEL_SELECT event fires. The client must instead learn the current
// channel from its GET_SELECTED_VOICE_CHANNEL query and seed the existing
// participants from the reply's voice_states (a fresh subscription won't replay
// them). Reaching InChannel + emitting ChannelSelect + VoiceCreate proves it.

void test_already_in_channel_on_connect(int lfd) {
    std::atomic<bool> mock_done{false};

    std::thread mock([&] {
        int cfd = ::accept(lfd, nullptr, nullptr);
        assert(cfd >= 0);

        int32_t op;
        std::string payload;
        assert(read_frame(cfd, op, payload));  // HANDSHAKE
        assert(op == 0);

        json ready = {{"cmd", "DISPATCH"}, {"evt", "READY"}, {"data", json::object()}};
        assert(write_frame(cfd, 1, ready.dump()));

        assert(read_frame(cfd, op, payload));  // AUTHORIZE
        json a = json::parse(payload);
        assert(a.at("cmd") == "AUTHORIZE");
        json ar = {{"cmd", "AUTHORIZE"}, {"data", {{"code", "C"}}}};
        if (a.contains("nonce")) ar["nonce"] = a.at("nonce");
        assert(write_frame(cfd, 1, ar.dump()));

        assert(read_frame(cfd, op, payload));  // AUTHENTICATE
        json an = json::parse(payload);
        assert(an.at("cmd") == "AUTHENTICATE");
        json anr = {{"cmd", "AUTHENTICATE"}, {"data", json::object()}};
        if (an.contains("nonce")) anr["nonce"] = an.at("nonce");
        assert(write_frame(cfd, 1, anr.dump()));

        // Read the two top-level SUBSCRIBEs + the GET query; reply to GET with a
        // channel that ALREADY has a participant (u9, self_mute).
        std::string get_nonce;
        for (int i = 0; i < 3; ++i) {
            assert(read_frame(cfd, op, payload));
            json m = json::parse(payload);
            if (m.value("cmd", std::string()) == "GET_SELECTED_VOICE_CHANNEL")
                get_nonce = m.value("nonce", std::string());
        }
        json chan = {
            {"cmd", "GET_SELECTED_VOICE_CHANNEL"},
            {"data", {
                {"id", "123"}, {"name", "General"},
                {"voice_states", json::array({
                    json{{"user", {{"id", "u9"}, {"username", "Alice"}, {"avatar", "ah"}}},
                         {"nick", "Ali"},
                         {"voice_state", {{"mute", false}, {"deaf", false},
                                          {"self_mute", true}, {"self_deaf", false}}}},
                })},
            }},
        };
        if (!get_nonce.empty()) chan["nonce"] = get_nonce;
        assert(write_frame(cfd, 1, chan.dump()));

        // The FSM should now subscribe to the channel-scoped events for "123".
        bool saw_channel_sub = false;
        for (int i = 0; i < 5; ++i) {
            assert(read_frame(cfd, op, payload));
            json sub = json::parse(payload);
            if (sub.value("cmd", std::string()) == "SUBSCRIBE" &&
                sub.value("evt", std::string()) == "VOICE_STATE_CREATE" &&
                sub.at("args").at("channel_id") == "123") {
                saw_channel_sub = true;
            }
        }
        assert(saw_channel_sub);

        char drain[64];
        ::read(cfd, drain, sizeof(drain));
        ::close(cfd);
        mock_done = true;
    });

    FakeHttp http;
    RpcConfig cfg;
    cfg.client_id = "207646673902501888";
    cfg.auth_mode = AuthMode::Streamkit;

    int64_t fake_now = 0;
    RpcClient client(cfg, http, [&] { return fake_now; });

    bool got_channel_select = false, got_voice_create_u9 = false;
    client.set_event_handler([&](const RpcEvent& ev) {
        if (ev.kind == RpcEvent::ChannelSelect && ev.channel_id == "123") got_channel_select = true;
        if (ev.kind == RpcEvent::VoiceCreate && ev.voice.user_id == "u9" && ev.voice.self_mute)
            got_voice_create_u9 = true;
    });

    client.start();
    for (int i = 0; i < 5000 && !(client.state() == ConnectionState::InChannel &&
                                  got_voice_create_u9); ++i) {
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Reached InChannel and seeded the existing participant WITHOUT a VOICE_CHANNEL_SELECT
    // event from Discord.
    assert(client.state() == ConnectionState::InChannel);
    assert(got_channel_select);
    assert(got_voice_create_u9);

    client.stop();
    mock.join();
    assert(mock_done);
}

// ---- Reconnect test -------------------------------------------------------
//
// After a clean handshake to READY, the mock drops the connection. The client
// should go Disconnected and, once the injected clock advances past
// reconnect_delay_ms, attempt a fresh connection that the mock's listener
// accepts again.

void test_reconnect_after_drop(int lfd) {
    std::atomic<int> accepts{0};
    std::atomic<bool> mock_done{false};

    std::thread mock([&] {
        // First connection: complete handshake to READY, then drop it.
        int cfd = ::accept(lfd, nullptr, nullptr);
        assert(cfd >= 0);
        ++accepts;

        int32_t op;
        std::string payload;
        assert(read_frame(cfd, op, payload));   // HANDSHAKE
        assert(op == 0);
        json ready = {{"cmd", "DISPATCH"}, {"evt", "READY"}, {"data", json::object()}};
        assert(write_frame(cfd, 1, ready.dump()));
        // Read the AUTHORIZE the client sends in response to READY, then drop.
        read_frame(cfd, op, payload);
        ::close(cfd);  // unexpected drop -> OnClosed on the client

        // Second connection: just accept it (the reconnect attempt).
        int cfd2 = ::accept(lfd, nullptr, nullptr);
        assert(cfd2 >= 0);
        ++accepts;
        // Read its handshake to confirm it's a real RpcClient connect.
        read_frame(cfd2, op, payload);
        ::close(cfd2);
        mock_done = true;
    });

    FakeHttp http;
    RpcConfig cfg;
    cfg.client_id = "207646673902501888";
    cfg.reconnect_delay_ms = 3000;

    int64_t fake_now = 0;
    RpcClient client(cfg, http, [&] { return fake_now; });
    client.set_event_handler([](const RpcEvent&) {});

    bool saw_disconnected = false;
    client.set_state_handler([&](ConnectionState s) {
        if (s == ConnectionState::Disconnected) saw_disconnected = true;
    });

    client.start();

    // Pump until the first connection drops (Disconnected).
    for (int i = 0; i < 5000 && !(client.state() == ConnectionState::Disconnected &&
                                  accepts >= 1); ++i) {
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(accepts >= 1);
    assert(client.state() == ConnectionState::Disconnected);
    assert(saw_disconnected);

    // Reconnect must NOT fire before the delay elapses.
    fake_now = 2999;
    for (int i = 0; i < 50; ++i) {
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(accepts == 1);  // still only the first connection

    // Advance past the delay; the next poll() should reconnect.
    fake_now = 3001;
    for (int i = 0; i < 5000 && accepts < 2; ++i) {
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(accepts >= 2);  // a second connection attempt landed

    client.stop();
    mock.join();
    assert(mock_done);
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/choir_rpc_client_test_XXXXXX";
    char* dir = ::mkdtemp(tmpl);
    assert(dir != nullptr);
    setenv("XDG_RUNTIME_DIR", dir, 1);

    std::string sock_path = std::string(dir) + "/discord-ipc-0";

    {
        int lfd = make_listener(sock_path);
        test_full_handshake_to_in_channel(lfd);
        ::close(lfd);
    }
    {
        int lfd = make_listener(sock_path);
        test_already_in_channel_on_connect(lfd);
        ::close(lfd);
    }
    {
        int lfd = make_listener(sock_path);
        test_reconnect_after_drop(lfd);
        ::close(lfd);
    }

    ::unlink(sock_path.c_str());
    ::rmdir(dir);
    return 0;
}
