// Font coverage: the overlay must render the punctuation and non-Latin text that shows
// up in real Discord names/messages. ImGui's built-in ProggyClean only covers
// U+0020..U+00FF, so a bullet, curly quote, em dash or any Cyrillic/Greek name draws as
// the "?" fallback glyph. load_overlay_font() replaces it with a system Unicode TTF.
//
// Exits 77 (meson SKIP) on a machine with none of the candidate fonts installed.

#include "overlay/overlay_ui.hpp"

#include "imgui.h"

#include <cassert>
#include <cstdio>

int main() {
    ImGui::CreateContext();
    if (!choir::load_overlay_font()) {
        std::puts("no system Unicode font found; skipping");
        return 77;
    }

    ImFont* font = ImGui::GetIO().Fonts->Fonts[0];
    // Sizes the overlay actually bakes at: base 13 plus a scaled/title size.
    for (float size : {13.0f, 20.8f}) {
        ImFontBaked* baked = font->GetFontBaked(size);
        const struct { unsigned cp; const char* what; } kWanted[] = {
            {0x00B7, "U+00B7 MIDDLE DOT"},
            {0x2022, "U+2022 BULLET"},
            {0x2019, "U+2019 RIGHT SINGLE QUOTE"},
            {0x201C, "U+201C LEFT DOUBLE QUOTE"},
            {0x2014, "U+2014 EM DASH"},
            {0x2026, "U+2026 HORIZONTAL ELLIPSIS"},
            {0x00E9, "U+00E9 e-acute"},
            {0x0100, "U+0100 A-macron (Latin Extended-A)"},
            {0x0410, "U+0410 CYRILLIC A"},
            {0x03B1, "U+03B1 GREEK ALPHA"},
        };
        for (const auto& w : kWanted) {
            if (!baked->FindGlyphNoFallback(static_cast<ImWchar>(w.cp))) {
                std::printf("missing glyph at size %.1f: %s\n", size, w.what);
                assert(false && "overlay font lacks a glyph that Discord text uses");
                return 1;
            }
        }
    }

    std::puts("ok");
    return 0;
}
