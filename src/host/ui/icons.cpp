#include "ui/icons.hpp"

#include <QPainter>
#include <QPixmap>

namespace choir {

QIcon app_icon() { return QIcon(QStringLiteral(":/choir/choir.svg")); }

QIcon tray_icon(Qt::ColorScheme scheme) {
    // Render the SVG large enough for HiDPI panels, then stamp it a flat colour
    // through its own alpha (SourceIn keeps the glyph shape, replaces the fill).
    QPixmap pm = QIcon(QStringLiteral(":/choir/choir-symbolic.svg")).pixmap(128, 128);
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), scheme == Qt::ColorScheme::Light ? Qt::black : Qt::white);
    p.end();
    return QIcon(pm);
}

}  // namespace choir
