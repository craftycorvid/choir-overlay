// Test for the host's icons (src/host/ui/icons.cpp).
//
// Two things can silently break here and only show up as an invisible tray icon
// on someone else's desktop:
//   1. the SVGs don't rasterize at all (qrc path typo, or Qt's svg image plugin
//      missing) — QIcon just hands back a null pixmap, no error anywhere;
//   2. the tray glyph is stamped the wrong colour for the panel (white on white).
// So: render both, assert non-empty, and assert the glyph's opaque pixels are
// black under a light colour scheme and white under a dark one.
//
// Runs under QT_QPA_PLATFORM=offscreen (set by meson) — no display needed.

#include "ui/icons.hpp"

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QPixmap>
#include <QSize>

#include <cassert>
#include <utility>

namespace {

// Colour of an arbitrary fully-opaque pixel, or transparent if the image has none.
QColor first_opaque(const QImage& img) {
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (c.alpha() == 255) return c;
        }
    }
    return QColor(Qt::transparent);
}

void test_app_icon_renders() {
    const QPixmap pm = choir::app_icon().pixmap(64, 64);
    assert(!pm.isNull());
    assert(pm.size() == QSize(64, 64));

    // The disc is blurple and covers the centre.
    const QColor centre = pm.toImage().pixelColor(32, 32);
    assert(centre.alpha() == 255);
    // Centre lands on the white note; sample the disc off to the side instead.
    const QColor edge = pm.toImage().pixelColor(6, 32);
    assert(edge == QColor(0x58, 0x65, 0xF2));
}

void test_tray_glyph_contrasts_with_the_panel() {
    for (const auto& [scheme, want] : {
             std::pair{Qt::ColorScheme::Dark, QColor(Qt::white)},
             std::pair{Qt::ColorScheme::Light, QColor(Qt::black)},
             // Unknown scheme falls back to the dark-panel assumption.
             std::pair{Qt::ColorScheme::Unknown, QColor(Qt::white)},
         }) {
        const QImage img = choir::tray_icon(scheme).pixmap(64, 64).toImage();
        assert(!img.isNull());
        const QColor got = first_opaque(img);
        assert(got.alpha() == 255);  // the glyph rasterized at all
        assert(got == want);
    }
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    test_app_icon_renders();
    test_tray_glyph_contrasts_with_the_panel();
    return 0;
}
