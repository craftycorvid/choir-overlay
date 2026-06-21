// Voice participant panel + notification toasts (Task 17).
//
// Drawn between ImGui::NewFrame() and ImGui::Render() on the render thread (see
// overlay_ui.hpp). We build everything as borderless, no-input, click-through windows
// and draw the visible content (background, avatars, rings, glyphs, text) via the
// window draw list so opacity/rounding look intentional and nothing grabs input.

#include "overlay_ui.hpp"

#include <algorithm>
#include <cfloat>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "imgui.h"

#include "iavatar_textures.hpp"
#include "fade.hpp"
#include "ipc/state.hpp"
#include "state_client.hpp"

namespace choir {
namespace {

// --- Layout constants (logical px at scale 1.0; multiplied by config.scale). ---
constexpr float kMargin = 16.0f;       // gap from the screen edge to the panel
constexpr float kPad = 10.0f;          // panel inner padding
constexpr float kAvatar = 32.0f;       // avatar diameter
constexpr float kRowGap = 8.0f;        // vertical gap between participant rows
constexpr float kAvatarTextGap = 10.0f;
constexpr float kPanelWidth = 220.0f;  // fixed panel width (name truncates if longer)
constexpr float kRowHeight = kAvatar;  // a row is as tall as its avatar
constexpr float kRing = 2.5f;          // speaking-ring thickness
constexpr float kNamePadX = 6.0f;      // per-name background "pill" horizontal padding
constexpr float kNamePadY = 2.0f;      // per-name background "pill" vertical padding
constexpr float kNameRound = 5.0f;     // per-name background corner rounding

// Per-participant opacity easing: dim when idle, quick fade up when speaking.
constexpr float kIdleAlpha = 0.40f;    // opacity of a non-speaking indicator
constexpr float kTauUp = 0.05f;        // quick fade-up time constant (s)
constexpr float kTauDown = 0.18f;      // gentler fade-down time constant (s)

// Multiply the alpha channel of an ImGui packed color by `m` (clamped to [0,1]).
ImU32 scale_alpha(ImU32 col, float m) {
    if (m < 0.0f) m = 0.0f; else if (m > 1.0f) m = 1.0f;
    const float a = static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFFu) * m;
    const ImU32 ai = static_cast<ImU32>(a + 0.5f) & 0xFFu;
    return (col & ~(0xFFu << IM_COL32_A_SHIFT)) | (ai << IM_COL32_A_SHIFT);
}

// Notification toasts, styled after the official Discord overlay: a dark rounded card
// with a circular icon on the left, a bright title, and a muted body that wraps to two
// lines (ellipsis on overflow).
constexpr float kToastWidth = 320.0f;
constexpr float kToastPad = 14.0f;       // card inner padding
constexpr float kToastGap = 8.0f;        // vertical gap between stacked cards
constexpr float kToastIcon = 40.0f;      // icon diameter
constexpr float kToastIconGap = 12.0f;   // gap between icon and text
constexpr float kToastRound = 12.0f;     // card corner radius
constexpr float kTitleBodyGap = 4.0f;    // gap between title and body
constexpr float kTitleScale = 1.2f;      // title font size relative to the base font
constexpr int   kToastBodyLines = 2;     // max body lines before truncating with "..."

constexpr float kPi = 3.14159265358979323846f;

// Common click-through, no-decoration, no-input window flags. NoBackground because we
// paint our own translucent rounded rect via the draw list (so opacity + rounding are
// under our control). These flags ensure the window can never capture mouse/keyboard.
constexpr ImGuiWindowFlags kOverlayWindowFlags =
    ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground |
    ImGuiWindowFlags_NoBringToFrontOnFocus;

bool is_top(Anchor a) { return a == Anchor::TopLeft || a == Anchor::TopRight; }
bool is_left(Anchor a) {
    return a == Anchor::TopLeft || a == Anchor::BottomLeft || a == Anchor::CenterLeft;
}
bool is_center(Anchor a) { return a == Anchor::CenterLeft || a == Anchor::CenterRight; }

// Compute the top-left position of a panel of `size` for `anchor` within `extent`,
// inset by a scaled margin. The window pivot is its top-left, so we subtract size on
// the right/bottom anchors. Center anchors hug the left/right edge, vertically centered.
ImVec2 anchored_pos(Anchor anchor, Extent2D extent, ImVec2 size, float margin) {
    const float W = static_cast<float>(extent.width);
    const float H = static_cast<float>(extent.height);
    float x = is_left(anchor) ? margin : (W - margin - size.x);
    float y = is_center(anchor) ? (H - size.y) * 0.5f
                                : (is_top(anchor) ? margin : (H - margin - size.y));
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    return ImVec2(x, y);
}

// Draw a small mic-off glyph (a mic capsule + stand crossed by a red slash) centered
// near (cx,cy) at radius r. Pure draw-list primitives — no font/icon assets.
void draw_mic_off(ImDrawList* dl, float cx, float cy, float r, float a) {
    const ImU32 fg = scale_alpha(IM_COL32(230, 230, 235, 255), a);
    const ImU32 slash = scale_alpha(IM_COL32(235, 70, 70, 255), a);
    // Mic body: a vertical rounded capsule.
    const float bw = r * 0.7f, bh = r * 1.1f;
    dl->AddRectFilled(ImVec2(cx - bw * 0.5f, cy - bh * 0.6f),
                      ImVec2(cx + bw * 0.5f, cy + bh * 0.2f), fg, bw * 0.5f);
    // Stand: a short vertical line below the body.
    dl->AddLine(ImVec2(cx, cy + bh * 0.2f), ImVec2(cx, cy + bh * 0.55f), fg, 1.5f);
    // Red diagonal slash = "muted".
    dl->AddLine(ImVec2(cx - r, cy - r), ImVec2(cx + r, cy + r), slash, 2.0f);
}

// Draw a small headphones-off glyph (a headband arc + two earcups crossed by a red
// slash) centered near (cx,cy) at radius r. Distinct from the mic-off glyph so muted
// vs deafened are visually distinguishable.
void draw_deaf(ImDrawList* dl, float cx, float cy, float r, float a) {
    const ImU32 fg = scale_alpha(IM_COL32(230, 230, 235, 255), a);
    const ImU32 slash = scale_alpha(IM_COL32(235, 70, 70, 255), a);
    // Headband: a half-circle arc across the top.
    dl->PathArcTo(ImVec2(cx, cy), r * 0.85f, kPi, 2.0f * kPi, 12);
    dl->PathStroke(fg, ImDrawFlags_None, 1.8f);
    // Two earcups (filled rects) at the arc ends.
    const float ew = r * 0.35f, eh = r * 0.55f;
    dl->AddRectFilled(ImVec2(cx - r * 0.85f - ew * 0.5f, cy - eh * 0.2f),
                      ImVec2(cx - r * 0.85f + ew * 0.5f, cy + eh * 0.8f), fg, ew * 0.4f);
    dl->AddRectFilled(ImVec2(cx + r * 0.85f - ew * 0.5f, cy - eh * 0.2f),
                      ImVec2(cx + r * 0.85f + ew * 0.5f, cy + eh * 0.8f), fg, ew * 0.4f);
    // Red diagonal slash = "deafened".
    dl->AddLine(ImVec2(cx - r, cy - r), ImVec2(cx + r, cy + r), slash, 2.0f);
}

// Draw a generic "person" silhouette (head + shoulders) centered at (cx,cy) within
// radius r — the placeholder shown when a participant has no avatar image. Pure
// draw-list primitives, tinted lighter than the disc behind it so it reads as a figure.
void draw_user_glyph(ImDrawList* dl, float cx, float cy, float r, float a) {
    const ImU32 fg = scale_alpha(IM_COL32(176, 181, 191, 255), a);
    // Head: a filled circle in the upper half.
    dl->AddCircleFilled(ImVec2(cx, cy - r * 0.28f), r * 0.32f, fg, 24);
    // Shoulders: the top half of a wider disc (a filled dome) just below the head. The
    // arc runs left->top->right; PathFillConvex closes it with the bottom chord. Both
    // shapes fit inside radius r, so they never spill past the placeholder disc.
    const float sr = r * 0.58f;
    const ImVec2 sc(cx, cy + r * 0.62f);
    dl->PathArcTo(sc, sr, kPi, 2.0f * kPi, 24);
    dl->PathFillConvex(fg);
}

// Resolve an avatar/icon texture by hash, loading it on demand from the retained map if
// not yet cached on this swapchain (this is what makes images survive a swapchain
// recreate). Returns ImTextureID_Invalid for an empty hash or any load failure.
ImTextureID resolve_icon(const std::string& hash, IAvatarTextures& textures,
                         StateClient& client) {
    if (hash.empty()) return ImTextureID_Invalid;
    ImTextureID id = textures.lookup(hash);
    if (id != ImTextureID_Invalid) return id;
    // Not loaded yet on this swapchain's texture cache: pull the retained AvatarReq
    // (announced by the host at some earlier point over the persistent connection) and
    // upload it now, on the render thread.
    if (auto req = client.avatar_for(hash)) id = textures.get_or_load(*req);
    return id;
}

ImTextureID resolve_avatar(const Participant& p, IAvatarTextures& textures,
                           StateClient& client) {
    return resolve_icon(p.avatar_hash, textures, client);
}

// Draw a circular avatar/icon of diameter `d` with its top-left at (x,y), faded by `a`.
// A valid `tex` is drawn as a circle-cropped image; otherwise the neutral placeholder
// disc + generic person silhouette is drawn instead.
void draw_circle_icon(ImDrawList* dl, float x, float y, float d, ImTextureID tex, float a) {
    const float r = d * 0.5f;
    const float cx = x + r, cy = y + r;
    if (tex != ImTextureID_Invalid) {
        dl->AddImageRounded(tex, ImVec2(x, y), ImVec2(x + d, y + d), ImVec2(0, 0),
                            ImVec2(1, 1), scale_alpha(IM_COL32_WHITE, a), r);
    } else {
        dl->AddCircleFilled(ImVec2(cx, cy), r, scale_alpha(IM_COL32(70, 74, 82, 255), a), 24);
        draw_user_glyph(dl, cx, cy, r, a);
    }
}

// Word-wrap `text` (measured at font `size`) to `wrap_width`, clamped to `max_lines`. If
// text remains after the last allowed line, that line is truncated to fit a trailing
// "..." ellipsis. Returns the lines as owned strings (the last may end in "...").
std::vector<std::string> wrap_text(const std::string& text, float size, float wrap_width,
                                   int max_lines) {
    std::vector<std::string> lines;
    if (text.empty() || wrap_width <= 1.0f || max_lines <= 0) return lines;
    ImFont* font = ImGui::GetFont();
    const char* p = text.c_str();
    const char* end = p + text.size();
    for (int i = 0; i < max_lines && p < end; ++i) {
        const char* fit = font->CalcWordWrapPosition(size, p, end, wrap_width);
        if (fit <= p) fit = p + 1;             // guarantee progress on a too-narrow column
        if (fit >= end) {                       // remainder fits on this line
            lines.emplace_back(p, end);
            break;
        }
        if (i == max_lines - 1) {               // overflow on the last line: ellipsize
            const float ell = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "...").x;
            const char* cut = font->CalcWordWrapPosition(size, p, end, wrap_width - ell);
            if (cut <= p) cut = fit;            // ellipsis wider than the column: fall back
            std::string line(p, cut);
            while (!line.empty() && line.back() == ' ') line.pop_back();
            line += "...";
            lines.push_back(std::move(line));
            break;
        }
        lines.emplace_back(p, fit);
        p = fit;
        while (p < end && *p == ' ') ++p;       // swallow the break's leading spaces
    }
    return lines;
}

// --- Voice participant panel ---------------------------------------------------------
void draw_voice_panel(const Snapshot& snap, IAvatarTextures& textures, StateClient& client,
                      Extent2D extent, int64_t now_ms) {
    const AppearanceConfig& cfg = snap.config;
    const float s = cfg.scale > 0.05f ? cfg.scale : 1.0f;

    // Collect the rows we will draw (respecting show_all_members).
    std::vector<const Participant*> rows;
    rows.reserve(snap.participants.size());
    for (const Participant& p : snap.participants) {
        if (!cfg.show_all_members && !p.speaking) continue;
        rows.push_back(&p);
    }
    if (rows.empty()) return;  // nothing to show (e.g. speakers-only with no speakers)

    // Per-participant opacity easing (persists across frames; single render thread).
    // Each indicator sits at kIdleAlpha and quickly fades up to 1.0 while speaking,
    // easing back down when it stops. dt comes from the present timestamp.
    static std::unordered_map<std::string, float> g_anim;  // user_id -> current alpha
    static int64_t g_last_ms = 0;
    float dt = (g_last_ms == 0) ? 0.0f : static_cast<float>(now_ms - g_last_ms) / 1000.0f;
    g_last_ms = now_ms;
    dt = std::clamp(dt, 0.0f, 0.25f);  // ignore negatives / clamp big gaps (post-stall)

    std::unordered_set<std::string> seen;
    for (const Participant* pp : rows) {
        seen.insert(pp->user_id);
        const float target = pp->speaking ? 1.0f : kIdleAlpha;
        auto it = g_anim.find(pp->user_id);
        if (it == g_anim.end()) {
            g_anim.emplace(pp->user_id, target);  // first sight: no fade-in on appear
        } else {
            const float tau = (target > it->second) ? kTauUp : kTauDown;
            it->second = fade_toward(it->second, target, dt, tau);
        }
    }
    // Drop entries for participants who left so the map stays bounded.
    for (auto it = g_anim.begin(); it != g_anim.end();) {
        if (seen.count(it->first)) ++it;
        else it = g_anim.erase(it);
    }

    const float pad = kPad * s;
    const float avatar = kAvatar * s;
    const float row_h = kRowHeight * s;
    const float row_gap = kRowGap * s;
    const float width = kPanelWidth * s;

    // Participant rows stack from the top (no channel header).
    const float line_h = ImGui::GetTextLineHeight();
    const float rows_h =
        rows.size() * row_h + (rows.size() > 1 ? (rows.size() - 1) * row_gap : 0.0f);
    const ImVec2 size(width, pad * 2.0f + rows_h);

    const ImVec2 pos = anchored_pos(cfg.anchor, extent, size, kMargin * s);

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    if (ImGui::Begin("##choir_voice_panel", nullptr, kOverlayWindowFlags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // No panel background and no channel header: the panel is fully transparent and
        // each participant's NAME gets its own translucent "pill" background instead.
        float cursor_y = pos.y + pad;
        const float left = pos.x + pad;

        for (const Participant* pp : rows) {
            const Participant& p = *pp;
            const float ax = left;
            const float ay = cursor_y;
            const float cx = ax + avatar * 0.5f;
            const float cy = ay + avatar * 0.5f;
            const float radius = avatar * 0.5f;

            // The whole indicator fades with this participant's eased alpha (dim when
            // idle, full while speaking). Multiplied by the panel-wide style Alpha.
            const float a = g_anim[p.user_id];

            // Circular avatar, or the neutral disc + person silhouette when there's none.
            draw_circle_icon(dl, ax, ay, avatar, resolve_avatar(p, textures, client), a);

            // Speaking indicator: a bright green ring around the avatar (fades in too).
            if (p.speaking) {
                dl->AddCircle(ImVec2(cx, cy), radius + kRing * s * 0.5f,
                              scale_alpha(IM_COL32(60, 230, 90, 255), a), 32, kRing * s);
            }

            // Display name, vertically centered against the avatar, with its own
            // translucent rounded "pill" background behind just the text.
            const char* name = p.display_name.c_str();
            const float tx = ax + avatar + kAvatarTextGap * s;
            const float name_y = cy - line_h * 0.5f;
            const ImVec2 tsz = ImGui::CalcTextSize(name);
            const float nbx = kNamePadX * s, nby = kNamePadY * s;
            dl->AddRectFilled(ImVec2(tx - nbx, name_y - nby),
                              ImVec2(tx + tsz.x + nbx, name_y + line_h + nby),
                              scale_alpha(IM_COL32(24, 26, 32, 205), a), kNameRound * s);
            dl->AddText(ImVec2(tx, name_y), scale_alpha(IM_COL32(235, 235, 240, 255), a), name);

            // Mute/deaf glyphs at the right edge of the row. Deaf implies no audio at
            // all, so prefer the headphones-off glyph; otherwise show mic-off if muted.
            const float gx = pos.x + size.x - pad - radius * 0.6f;
            const float gr = radius * 0.55f;
            if (p.deaf || p.self_deaf) {
                draw_deaf(dl, gx, cy, gr, a);
            } else if (p.mute || p.self_mute) {
                draw_mic_off(dl, gx, cy, gr, a);
            }

            cursor_y += row_h + row_gap;
        }
    }
    ImGui::End();
}

// --- Notification toasts -------------------------------------------------------------
// Styled after the official Discord overlay: a dark rounded card carrying a circular icon
// on the left, a bright (slightly larger) title, and a muted body wrapped to two lines.
void draw_toasts(const Snapshot& snap, IAvatarTextures& textures, StateClient& client,
                 Extent2D extent, int64_t now_ms) {
    const AppearanceConfig& cfg = snap.config;
    const float s = cfg.scale > 0.05f ? cfg.scale : 1.0f;
    const int64_t dur = cfg.toast_duration_ms > 0 ? cfg.toast_duration_ms : 5000;

    // Live toasts, newest first (so the newest sits at the anchor). Notifications arrive
    // appended; iterate in reverse for newest-first.
    std::vector<const Notification*> live;
    for (auto it = snap.notifications.rbegin(); it != snap.notifications.rend(); ++it) {
        const int64_t age = now_ms - it->created_ms;
        if (age >= 0 && age < dur) live.push_back(&*it);
    }
    if (live.empty()) return;

    const float width = kToastWidth * s;
    const float pad = kToastPad * s;
    const float icon = kToastIcon * s;
    const float icon_gap = kToastIconGap * s;
    const float round = kToastRound * s;
    const float gap = kTitleBodyGap * s;
    const float line_h = ImGui::GetTextLineHeight();         // body line height
    const float base = ImGui::GetFontSize();
    const float title_sz = base * kTitleScale;               // title is a touch larger
    const float margin = kMargin * s;
    const Anchor anchor = cfg.toast_anchor;
    ImFont* font = ImGui::GetFont();

    // Stack from the anchor's edge inward. For top anchors we move downward as we add
    // older toasts; for bottom anchors we move upward.
    float stack_y = is_top(anchor) ? margin : (static_cast<float>(extent.height) - margin);
    const float x = is_left(anchor) ? margin
                                    : (static_cast<float>(extent.width) - margin - width);

    int idx = 0;
    for (const Notification* np : live) {
        const Notification& n = *np;
        const bool has_icon = !n.icon_hash.empty();
        const float text_x = pad + (has_icon ? icon + icon_gap : 0.0f);
        const float text_w = width - text_x - pad;

        // Lay out the title (one line, ellipsized) and body (up to two lines) up front so
        // the card height fits the text — or the icon, whichever is taller.
        const auto title_lines = wrap_text(n.title, title_sz, text_w, 1);
        const auto body_lines = wrap_text(n.body, base, text_w, kToastBodyLines);
        const float text_h =
            title_sz + (body_lines.empty() ? 0.0f : gap + body_lines.size() * line_h);
        const float inner_h = std::max(has_icon ? icon : 0.0f, text_h);
        const float h = inner_h + pad * 2.0f;

        float y;
        if (is_top(anchor)) {
            y = stack_y;
            stack_y += h + kToastGap * s;
        } else {
            stack_y -= h;
            y = stack_y;
            stack_y -= kToastGap * s;
        }
        if (y < 0.0f) break;  // ran off the screen — stop stacking

        const ImVec2 pos(x < 0.0f ? 0.0f : x, y);
        const ImVec2 size(width, h);

        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        const std::string id = "##choir_toast_" + std::to_string(idx++);
        if (ImGui::Begin(id.c_str(), nullptr, kOverlayWindowFlags)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Soft drop shadow: a few translucent rounded rects fanning outward, drawn
            // outside the window's own clip rect (which is the card) so they aren't
            // clipped away. Gives the card a sense of elevation over the game.
            dl->PushClipRectFullScreen();
            for (int i = 1; i <= 3; ++i) {
                const float g = i * 2.0f * s;
                dl->AddRectFilled(ImVec2(pos.x - g, pos.y - g + 3.0f * s),
                                  ImVec2(pos.x + size.x + g, pos.y + size.y + g + 3.0f * s),
                                  IM_COL32(0, 0, 0, 24 - i * 6), round + g);
            }
            dl->PopClipRect();

            // Card background.
            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                              IM_COL32(32, 34, 40, 245), round);

            // Icon (resolved image, or disc + person silhouette), centered vertically.
            if (has_icon) {
                const float iy = pos.y + pad + (inner_h - icon) * 0.5f;
                draw_circle_icon(dl, pos.x + pad, iy, icon,
                                 resolve_icon(n.icon_hash, textures, client), 1.0f);
            }

            // Title + body, vertically centered against the icon when the icon is taller.
            const float tx = pos.x + text_x;
            float ty = pos.y + pad + (inner_h - text_h) * 0.5f;
            if (!title_lines.empty()) {
                // Faux-bold: draw the title twice, offset 1px, for extra weight.
                const ImU32 tc = IM_COL32(255, 255, 255, 255);
                dl->AddText(font, title_sz, ImVec2(tx, ty), tc, title_lines[0].c_str());
                dl->AddText(font, title_sz, ImVec2(tx + 1.0f, ty), tc, title_lines[0].c_str());
            }
            ty += title_sz + gap;
            const ImU32 bc = IM_COL32(185, 187, 196, 255);
            for (const std::string& ln : body_lines) {
                dl->AddText(ImVec2(tx, ty), bc, ln.c_str());
                ty += line_h;
            }
        }
        ImGui::End();
    }
}

}  // namespace

void draw_overlay(const Snapshot& snap, IAvatarTextures& textures, StateClient& client,
                  Extent2D extent, int64_t now_ms) {
    // Visibility is voice-state driven: draw NOTHING unless we are in a voice channel.
    if (!snap.in_voice) return;

    draw_voice_panel(snap, textures, client, extent, now_ms);
    draw_toasts(snap, textures, client, extent, now_ms);
}

}  // namespace choir
