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

#include "config/backends.hpp"
#include "config/config.hpp"
#include "config/denylist.hpp"
#include "discord/qt_http.hpp"
#include "discord/rpc_client.hpp"
#include "discord/rpc_messages.hpp"
#include "ipc/paths.hpp"
#include "model/avatar_cache.hpp"
#include "model/overlay_state.hpp"
#include "server/state_server.hpp"
#include "ui/icons.hpp"
#include "ui/settings_window.hpp"
#include "ui/tray.hpp"

#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {

choir::RpcConfig make_rpc_config(const choir::Config& cfg) {
    choir::RpcConfig rc;
    rc.client_id = cfg.client_id;
    // scopes left at RpcConfig defaults.
    return rc;
}

// Keep the injected backends in ~/.local in step with the AppImage's payload.
//
// An AppImage carries libchoir_overlay.so + libchoir_gl.so but cannot host them: games
// load them by absolute path long after /tmp/.mount_XXXXXX is gone. So they are copied
// out. That writes outside the image, which a portable app shouldn't do unasked — so we
// ask once, remember the answer, and re-sync silently forever after (which also means a
// newer AppImage can never pair with stale libraries).
//
// Entirely a no-op for source/pacman installs: $APPDIR is unset, so the payload dir is
// empty and we return before touching anything.
void sync_appimage_backends(choir::Config& config) {
    const std::string payload = choir::appimage_payload_dir();
    if (payload.empty()) return;
    if (choir::backends_up_to_date(payload, choir::backend_lib_dir())) return;

    if (config.backend_consent == choir::Config::kBackendUnasked) {
        QMessageBox box;
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(QStringLiteral("Choir"));
        box.setText(QStringLiteral("Install Choir's overlay libraries to ~/.local?"));
        box.setInformativeText(QStringLiteral(
            "Games load the overlay from a fixed path on disk, which this AppImage cannot "
            "provide — its contents exist only while Choir is running. Choir needs to copy "
            "two small libraries to ~/.local/lib/choir and register the Vulkan layer in "
            "~/.local/share.\n\n"
            "Without them the tray and settings still work, but no overlay appears in "
            "games. You can do this later from the settings window."));
        QPushButton* install =
            box.addButton(QStringLiteral("Install"), QMessageBox::AcceptRole);
        box.addButton(QStringLiteral("Not now"), QMessageBox::RejectRole);
        box.setDefaultButton(install);
        box.exec();

        config.backend_consent = (box.clickedButton() == install)
                                     ? choir::Config::kBackendGranted
                                     : choir::Config::kBackendDeclined;
        config.save(choir::config_path());
    }

    if (config.backend_consent != choir::Config::kBackendGranted) return;

    if (!choir::install_backends(payload)) {
        std::fprintf(stderr, "choir: failed to install overlay libraries into %s\n",
                     choir::backend_lib_dir().c_str());
        QMessageBox::warning(
            nullptr, QStringLiteral("Choir"),
            QStringLiteral("Could not write the overlay libraries to ~/.local.\n\n"
                           "Check that the filesystem is writable and has free space; "
                           "Choir itself will keep running without an in-game overlay."));
    }
}

}  // namespace

int main(int argc, char** argv) {
    // --version and --uninstall must work before any GUI is constructed (no display
    // needed, so they also work over ssh or from a .desktop action).
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("choir %s\n", CHOIR_VERSION);
            return 0;
        }
        if (std::strcmp(argv[i], "--uninstall") == 0) {
            // Deleting an AppImage leaves the overlay libraries and, worse, the implicit
            // layer manifest behind — the loader would keep injecting the layer into
            // every Vulkan application with nothing left to own it.
            const bool ok = choir::uninstall_backends();
            std::printf("Removed Choir's overlay libraries from %s\n",
                        choir::backend_lib_dir().c_str());
            std::printf("Removed the Vulkan layer manifest and choir-run.\n");
            if (!ok) {
                std::fprintf(stderr,
                             "choir: some files could not be removed; check permissions "
                             "on ~/.local\n");
            }
            // Forget the install consent so a later launch asks again rather than
            // silently reinstalling what was just deliberately removed.
            choir::Config cfg = choir::Config::load(choir::config_path());
            if (cfg.backend_consent != choir::Config::kBackendUnasked) {
                cfg.backend_consent = choir::Config::kBackendUnasked;
                cfg.save(choir::config_path());
            }
            std::printf("Settings kept at %s (delete it to remove them too).\n",
                        choir::config_path().c_str());
            return ok ? 0 : 1;
        }
    }

    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);  // live in the tray
    // Matches packaging/choir.desktop, so Wayland compositors pair our windows with
    // the installed desktop entry (app-id → icon, "Choir" in the app switcher).
    QApplication::setDesktopFileName(QStringLiteral("choir"));
    QApplication::setWindowIcon(choir::app_icon());

    // --- Config + denylist gate ---
    choir::Config config = choir::Config::load(choir::config_path());

    // Before anything else: under an AppImage the overlay backends have to be on the
    // real filesystem or no game can load them (no-op otherwise).
    sync_appimage_backends(config);

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
        // Fetch any emoji images the toast text references (custom + unicode).
        if (ev.kind == choir::RpcEvent::Notification) {
            choir::request_notification_emoji(avatars, ev.notif.title, ev.notif.body);
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
