// M0 auth spike (Task 7 gate) — Choir Wayland Discord overlay.
//
// This standalone binary validates the project's #1 risk: that the Streamkit
// client_id RPC flow (AUTHORIZE -> token exchange -> AUTHENTICATE -> voice
// SUBSCRIBE) actually works against a real, running Discord desktop client and
// yields live voice state. It is the deliverable the USER runs to close the M0
// gate; it is intentionally NOT a registered automated test (it needs a real
// Discord client + a human approving the one-time OAuth consent prompt).
//
// Usage:
//   auth_spike --help     print this banner and exit (no network, no Discord)
//   auth_spike            connect to the running Discord client and stream events
//
// If the spike fails (e.g. the placeholder kStreamkitTokenUrl in oauth.hpp is
// wrong, or Streamkit needs different params/headers), that is exactly the
// finding the gate exists to surface — correct kStreamkitTokenUrl (one constant)
// or switch RpcConfig to AuthMode::OwnApp and re-run.

#include "discord/qt_http.hpp"
#include "discord/rpc_client.hpp"
#include "discord/rpc_messages.hpp"

#include <QCoreApplication>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

const char* state_name(choir::ConnectionState s) {
    switch (s) {
        case choir::ConnectionState::Disconnected: return "Disconnected";
        case choir::ConnectionState::Connecting:   return "Connecting";
        case choir::ConnectionState::Authorizing:  return "Authorizing";
        case choir::ConnectionState::Ready:        return "Ready";
        case choir::ConnectionState::InChannel:    return "InChannel";
    }
    return "?";
}

void print_banner() {
    std::puts(
        "Choir — M0 auth spike (Streamkit RPC gate)\n"
        "------------------------------------------\n"
        "Validates the Discord local-RPC auth flow against your REAL Discord client.\n"
        "\n"
        "Before running (without --help):\n"
        "  1. The Discord desktop client (or Vesktop/Vencord) must be running.\n"
        "  2. Join a voice channel (talk, or have someone else talk, to see speaking events).\n"
        "  3. Approve the one-time authorization prompt that Discord will show.\n"
        "\n"
        "On success it prints the live voice roster + SPEAKING_START/STOP as people talk.\n"
        "If it never authorizes or errors at token exchange, the placeholder\n"
        "kStreamkitTokenUrl in src/host/discord/oauth.hpp likely needs correcting,\n"
        "or switch RpcConfig.auth_mode to OwnApp (register your own Discord app).\n"
        "\n"
        "Press Ctrl-C to quit.");
}

void print_event(const choir::RpcEvent& ev) {
    using K = choir::RpcEvent;
    switch (ev.kind) {
        case K::ChannelSelect:
            if (ev.channel_id.empty())
                std::printf("[voice] left voice channel\n");
            else
                std::printf("[voice] joined voice channel id=%s\n", ev.channel_id.c_str());
            break;
        case K::VoiceCreate:
        case K::VoiceUpdate:
            std::printf("[voice] %s user=%s nick=\"%s\" mute=%d deaf=%d self_mute=%d self_deaf=%d avatar=%s\n",
                        ev.kind == K::VoiceCreate ? "JOIN " : "STATE",
                        ev.voice.user_id.c_str(), ev.voice.nick.c_str(),
                        ev.voice.mute, ev.voice.deaf, ev.voice.self_mute, ev.voice.self_deaf,
                        ev.voice.avatar_hash.c_str());
            break;
        case K::VoiceDelete:
            std::printf("[voice] LEFT  user=%s\n", ev.voice.user_id.c_str());
            break;
        case K::SpeakingStart:
            std::printf("[speak] START user=%s\n", ev.user_id.c_str());
            break;
        case K::SpeakingStop:
            std::printf("[speak] STOP  user=%s\n", ev.user_id.c_str());
            break;
        case K::Notification:
            std::printf("[notif] %s: %s\n", ev.notif.title.c_str(), ev.notif.body.c_str());
            break;
    }
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_banner();
            return 0;
        }
    }

    QCoreApplication app(argc, argv);
    print_banner();
    std::puts("\nStarting… (connecting to the Discord client)\n");
    std::fflush(stdout);

    choir::QtHttpPost http;
    choir::RpcConfig cfg;  // Streamkit defaults: client_id 207646673902501888, AuthMode::Streamkit
    cfg.client_id = "207646673902501888";
    cfg.auth_mode = choir::AuthMode::Streamkit;

    choir::RpcClient rpc(cfg, http);

    rpc.set_state_handler([](choir::ConnectionState s) {
        std::printf(">> state: %s\n", state_name(s));
        if (s == choir::ConnectionState::Authorizing)
            std::puts(">> Approve the authorization prompt in your Discord client now.");
        std::fflush(stdout);
    });
    rpc.set_event_handler(&print_event);

    rpc.start();

    QTimer pump;
    QObject::connect(&pump, &QTimer::timeout, [&rpc]() { rpc.poll(); });
    pump.start(40);

    return app.exec();
}
