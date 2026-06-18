// Voice participant panel + notification toasts (Task 17).
//
// Drawn between ImGui::NewFrame() and ImGui::Render() on the render thread (see
// overlay_ui.hpp). We build everything as borderless, no-input, click-through windows
// and draw the visible content (background, avatars, rings, glyphs, text) via the
// window draw list so opacity/rounding look intentional and nothing grabs input.

#include "overlay_ui.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "imgui.h"

#include "avatar_textures.hpp"
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
constexpr float kRound = 8.0f;         // toast background corner rounding
constexpr float kNamePadX = 6.0f;      // per-name background "pill" horizontal padding
constexpr float kNamePadY = 2.0f;      // per-name background "pill" vertical padding
constexpr float kNameRound = 5.0f;     // per-name background corner rounding

constexpr float kToastWidth = 240.0f;
constexpr float kToastPad = 10.0f;
constexpr float kToastGap = 8.0f;
constexpr float kTitleBodyGap = 4.0f;

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
ImVec2 anchored_pos(Anchor anchor, VkExtent2D extent, ImVec2 size, float margin) {
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
void draw_mic_off(ImDrawList* dl, float cx, float cy, float r) {
    const ImU32 fg = IM_COL32(230, 230, 235, 255);
    const ImU32 slash = IM_COL32(235, 70, 70, 255);
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
void draw_deaf(ImDrawList* dl, float cx, float cy, float r) {
    const ImU32 fg = IM_COL32(230, 230, 235, 255);
    const ImU32 slash = IM_COL32(235, 70, 70, 255);
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

// Resolve a participant's avatar texture, loading it on demand from the retained map
// if not yet cached (this is what makes avatars survive a swapchain recreate).
ImTextureID resolve_avatar(const Participant& p, AvatarTextures& textures,
                           StateClient& client) {
    if (p.avatar_hash.empty()) return ImTextureID_Invalid;
    ImTextureID id = textures.lookup(p.avatar_hash);
    if (id != ImTextureID_Invalid) return id;
    // Not loaded yet on this swapchain's texture cache: pull the retained AvatarReq
    // (announced by the host at some earlier point over the persistent connection) and
    // upload it now, on the render thread.
    if (auto req = client.avatar_for(p.avatar_hash)) id = textures.get_or_load(*req);
    return id;
}

// --- Voice participant panel ---------------------------------------------------------
void draw_voice_panel(const Snapshot& snap, AvatarTextures& textures, StateClient& client,
                      VkExtent2D extent) {
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

    // Opacity applies to the whole panel.
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, cfg.opacity);

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

            ImTextureID tex = resolve_avatar(p, textures, client);
            if (tex != ImTextureID_Invalid) {
                // Circular avatar via a fully-rounded image (rounding == radius).
                dl->AddImageRounded(tex, ImVec2(ax, ay), ImVec2(ax + avatar, ay + avatar),
                                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, radius);
            } else {
                // Neutral placeholder circle when the texture isn't available.
                dl->AddCircleFilled(ImVec2(cx, cy), radius, IM_COL32(70, 74, 82, 255), 24);
            }

            // Speaking indicator: a bright green ring around the avatar.
            if (p.speaking) {
                dl->AddCircle(ImVec2(cx, cy), radius + kRing * s * 0.5f,
                              IM_COL32(60, 230, 90, 255), 32, kRing * s);
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
                              IM_COL32(24, 26, 32, 205), kNameRound * s);
            dl->AddText(ImVec2(tx, name_y), IM_COL32(235, 235, 240, 255), name);

            // Mute/deaf glyphs at the right edge of the row. Deaf implies no audio at
            // all, so prefer the headphones-off glyph; otherwise show mic-off if muted.
            const float gx = pos.x + size.x - pad - radius * 0.6f;
            const float gr = radius * 0.55f;
            if (p.deaf || p.self_deaf) {
                draw_deaf(dl, gx, cy, gr);
            } else if (p.mute || p.self_mute) {
                draw_mic_off(dl, gx, cy, gr);
            }

            cursor_y += row_h + row_gap;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();  // Alpha
}

// --- Notification toasts -------------------------------------------------------------
void draw_toasts(const Snapshot& snap, VkExtent2D extent, int64_t now_ms) {
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
    const float line_h = ImGui::GetTextLineHeight();
    const float margin = kMargin * s;
    const Anchor anchor = cfg.toast_anchor;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, cfg.opacity);

    // Stack from the anchor's edge inward. For top anchors we move downward as we add
    // older toasts; for bottom anchors we move upward.
    float stack_y = is_top(anchor) ? margin : (static_cast<float>(extent.height) - margin);
    const float x = is_left(anchor) ? margin
                                    : (static_cast<float>(extent.width) - margin - width);

    int idx = 0;
    for (const Notification* np : live) {
        const Notification& n = *np;
        // Toast height = padding + title line + (gap + body line if body present).
        const bool has_body = !n.body.empty();
        const float h = pad * 2.0f + line_h + (has_body ? (kTitleBodyGap * s + line_h) : 0.0f);

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
            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                              IM_COL32(28, 30, 38, 235), kRound * s);
            float ty = pos.y + pad;
            dl->AddText(ImVec2(pos.x + pad, ty), IM_COL32(255, 255, 255, 255),
                        n.title.c_str());
            if (has_body) {
                ty += line_h + kTitleBodyGap * s;
                dl->AddText(ImVec2(pos.x + pad, ty), IM_COL32(205, 205, 212, 255),
                            n.body.c_str());
            }
        }
        ImGui::End();
    }
    ImGui::PopStyleVar();  // Alpha
}

}  // namespace

void draw_overlay(const Snapshot& snap, AvatarTextures& textures, StateClient& client,
                  VkExtent2D extent, int64_t now_ms) {
    // Visibility is voice-state driven: draw NOTHING unless we are in a voice channel.
    if (!snap.in_voice) return;

    draw_voice_panel(snap, textures, client, extent);
    draw_toasts(snap, extent, now_ms);
}

}  // namespace choir
