// `choir` host entry point (Task 11).
//
// Assembles the runnable host:
//   Config -> RpcConfig
//   QtHttpPost  + RpcClient        (Discord local RPC + OAuth)
//   OverlayState                   (RpcEvent -> Snapshot reducer)
//   QtAvatarSource + AvatarCache   (avatar fetch/decode -> .rgba cache)
//   StateServer                    (serves snapshots/avatars to layer clients)
//   Tray + SettingsWindow          (desktop UI)
//
// Wiring:
//   rpc events            -> state.apply(ev) (+ request avatars for participants)
//   state.on_change       -> server.set_snapshot + server.broadcast
//   avatars.ready         -> server.broadcast_avatar
//   a 40ms QTimer         -> rpc.poll()  (v1; QSocketNotifier is a future tweak)
//   tray menu             -> settings / reconnect / quit
//   settings config_changed -> push appearance into state, rebuild denylist gate
//   settings authorize    -> rpc.start()
//
// The app lives in the tray; no main window is shown by default.

#include "config/config.hpp"
#include "config/denylist.hpp"
#include "discord/qt_http.hpp"
#include "discord/rpc_client.hpp"
#include "discord/rpc_messages.hpp"
#include "ipc/paths.hpp"
#include "model/avatar_cache.hpp"
#include "model/overlay_state.hpp"
#include "server/state_server.hpp"
#include "ui/settings_window.hpp"
#include "ui/tray.hpp"

#include <QApplication>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {

choir::RpcConfig make_rpc_config(const choir::Config& cfg) {
    choir::RpcConfig rc;
    rc.client_id = cfg.client_id;
    rc.client_secret = cfg.client_secret;
    rc.auth_mode = cfg.auth_mode;
    // scopes left at RpcConfig defaults.
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    // --version must work before any GUI is constructed (no display needed).
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("choir %s\n", CHOIR_VERSION);
            return 0;
        }
    }

    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);  // live in the tray

    // --- Config + denylist gate ---
    choir::Config config = choir::Config::load(choir::config_path());

    // A shared denylist behind a pointer so SettingsWindow can swap it at runtime.
    auto denylist = std::make_shared<choir::Denylist>(config.denylist);

    // --- Components ---
    choir::QtHttpPost http;
    choir::RpcClient rpc(make_rpc_config(config), http);

    choir::OverlayState state;
    state.set_config(config.appearance);

    choir::QtAvatarSource avsrc;
    choir::AvatarCache avatars(avsrc, choir::avatar_cache_dir());

    choir::StateServer server(
        [denylist](const std::string& exe) { return denylist->blocks(exe); });
    if (!server.listen()) {
        std::fprintf(stderr, "choir: warning — state server failed to listen; "
                             "layers will not receive snapshots.\n");
    }
    // Seed the server with the initial (empty) snapshot.
    server.set_snapshot(state.current());

    choir::Tray tray;
    std::unique_ptr<choir::SettingsWindow> settings;  // created lazily on first open

    // --- Wiring: state changes -> server ---
    state.on_change = [&server](const choir::Snapshot& s) {
        server.set_snapshot(s);
        server.broadcast(s);
    };

    // --- Wiring: avatars ready -> server ---
    avatars.ready = [&server](const std::string& hash, const std::string& path,
                              uint32_t w, uint32_t h) {
        server.broadcast_avatar(hash, path, w, h);
    };

    // --- Wiring: RPC events -> state (+ avatar requests) ---
    rpc.set_event_handler([&state, &avatars](const choir::RpcEvent& ev) {
        state.apply(ev);
        // When a participant appears/updates with an avatar hash, fetch it.
        if ((ev.kind == choir::RpcEvent::VoiceCreate ||
             ev.kind == choir::RpcEvent::VoiceUpdate) &&
            !ev.voice.avatar_hash.empty()) {
            avatars.request(ev.voice.user_id, ev.voice.avatar_hash);
        }
        // A message notification carries its author's avatar hash + id; fetch it so the
        // toast shows the real avatar (same cache as voice participants).
        if (ev.kind == choir::RpcEvent::Notification &&
            !ev.notif.icon_hash.empty() && !ev.user_id.empty()) {
            avatars.request(ev.user_id, ev.notif.icon_hash);
        }
    });

    // --- Pump the RPC client (~40ms; QSocketNotifier is a future optimization) ---
    QTimer rpc_timer;
    QObject::connect(&rpc_timer, &QTimer::timeout, [&rpc]() { rpc.poll(); });
    rpc_timer.start(40);

    // --- Tray actions ---
    QObject::connect(&tray, &choir::Tray::reconnect_requested, [&rpc]() {
        rpc.stop();
        rpc.start();
    });
    QObject::connect(&tray, &choir::Tray::quit_requested, []() { QApplication::quit(); });
    QObject::connect(&tray, &choir::Tray::open_settings_requested, [&]() {
        if (!settings) {
            settings = std::make_unique<choir::SettingsWindow>(config);

            // Persisted settings -> live state + denylist.
            QObject::connect(
                settings.get(), &choir::SettingsWindow::config_changed,
                [&](const choir::Config& newcfg) {
                    config = newcfg;
                    state.set_config(newcfg.appearance);  // bumps revision + broadcasts
                    *denylist = choir::Denylist(newcfg.denylist);
                });

            // "Authorize with Discord" -> kick the RPC AUTHORIZE flow.
            QObject::connect(settings.get(), &choir::SettingsWindow::authorize_requested,
                             [&rpc]() {
                                 rpc.stop();
                                 rpc.start();
                             });
        }
        settings->show();
        settings->raise();
        settings->activateWindow();
    });

    tray.show();  // may return false under offscreen / no-tray — host still runs

    // --- Connect to Discord on launch ---
    rpc.start();

    return app.exec();
}
