#pragma once
// Choir's icons, compiled in via packaging/icons/choir.qrc (so they work in the
// build tree, before any install). Rendering SVG needs Qt's svg image/icon plugins
// (qt6-svg) — the host links Qt6::Svg so a missing one fails at build, not at runtime.

#include <QIcon>
#include <Qt>

namespace choir {

// The full app icon: music note on a blurple disc. For the window/taskbar.
QIcon app_icon();

// The flat tray glyph, filled to contrast with the panel: white under a dark
// colour scheme, black under a light one (Unknown → white; panels skew dark).
QIcon tray_icon(Qt::ColorScheme scheme);

}  // namespace choir
