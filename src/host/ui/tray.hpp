#pragma once

// System tray icon + menu (Task 11).
//
// The host lives in the tray (no main window by default). The menu offers
// "Open Settings", "Reconnect", and "Quit", surfaced to main() as Qt signals.
//
// Qt-using; compiled into the `choir` executable, never libchoir_host_core.

#include <QObject>
#include <QtGlobal>

QT_BEGIN_NAMESPACE
class QSystemTrayIcon;
class QMenu;
QT_END_NAMESPACE

namespace choir {

class Tray : public QObject {
    Q_OBJECT
public:
    explicit Tray(QObject* parent = nullptr);
    ~Tray() override;

    // Show the tray icon. Returns false if the platform has no system tray
    // (e.g. offscreen) — the host still runs, just without a visible icon.
    bool show();

signals:
    void open_settings_requested();
    void reconnect_requested();
    void quit_requested();

private:
    QSystemTrayIcon* icon_ = nullptr;
    QMenu* menu_ = nullptr;
};

}  // namespace choir
