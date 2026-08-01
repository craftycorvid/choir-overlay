#include "ui/tray.hpp"

#include <QAction>
#include <QGuiApplication>
#include <QMenu>
#include <QStyleHints>
#include <QSystemTrayIcon>

#include "ui/icons.hpp"

namespace choir {

Tray::Tray(QObject* parent) : QObject(parent) {
    menu_ = new QMenu();

    QAction* settings = menu_->addAction(QStringLiteral("Open Settings"));
    QAction* reconnect = menu_->addAction(QStringLiteral("Reconnect"));
    menu_->addSeparator();
    QAction* quit = menu_->addAction(QStringLiteral("Quit"));

    connect(settings, &QAction::triggered, this, &Tray::open_settings_requested);
    connect(reconnect, &QAction::triggered, this, &Tray::reconnect_requested);
    connect(quit, &QAction::triggered, this, &Tray::quit_requested);

    QStyleHints* hints = QGuiApplication::styleHints();
    icon_ = new QSystemTrayIcon(tray_icon(hints->colorScheme()), this);
    icon_->setToolTip(QStringLiteral("Choir — overlay for Discord"));
    icon_->setContextMenu(menu_);

    // Follow the DE flipping between light and dark mode.
    connect(hints, &QStyleHints::colorSchemeChanged, this,
            [this](Qt::ColorScheme scheme) { icon_->setIcon(tray_icon(scheme)); });

    // Left-clicking the tray icon opens settings too.
    connect(icon_, &QSystemTrayIcon::activated,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger) emit open_settings_requested();
            });
}

Tray::~Tray() { delete menu_; }

bool Tray::show() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return false;
    icon_->show();
    return true;
}

}  // namespace choir
