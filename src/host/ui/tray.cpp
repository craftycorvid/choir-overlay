#include "ui/tray.hpp"

#include <QAction>
#include <QColor>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSystemTrayIcon>

namespace choir {

namespace {

// A simple generated icon (a filled circle) so we don't ship an asset file.
QIcon make_placeholder_icon() {
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(QColor(88, 101, 242));  // a friendly blurple-ish fill
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(8, 8, 48, 48);
    painter.end();
    return QIcon(pm);
}

}  // namespace

Tray::Tray(QObject* parent) : QObject(parent) {
    menu_ = new QMenu();

    QAction* settings = menu_->addAction(QStringLiteral("Open Settings"));
    QAction* reconnect = menu_->addAction(QStringLiteral("Reconnect"));
    menu_->addSeparator();
    QAction* quit = menu_->addAction(QStringLiteral("Quit"));

    connect(settings, &QAction::triggered, this, &Tray::open_settings_requested);
    connect(reconnect, &QAction::triggered, this, &Tray::reconnect_requested);
    connect(quit, &QAction::triggered, this, &Tray::quit_requested);

    icon_ = new QSystemTrayIcon(make_placeholder_icon(), this);
    icon_->setToolTip(QStringLiteral("Choir — overlay for Discord"));
    icon_->setContextMenu(menu_);

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
