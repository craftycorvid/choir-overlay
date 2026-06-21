// The real Choir overlay drawing (Task 17): voice participant panel + toasts.
//
// draw_overlay() turns the current choir::Snapshot into Dear ImGui draw commands:
//   * a voice participant panel (avatar circle, name, speaking ring, mute/deaf glyphs)
//   * transient notification toasts (title + body), expiring by age.
//
// It is called by the present hook (swapchain.cpp) BETWEEN ImGui::NewFrame() and
// ImGui::Render(), on the render thread, with the ImGui context already current and
// under the swapchain lock. It records NO Vulkan commands — only ImGui draw lists.
//
// Click-through by construction: every window uses NoInputs|NoNav|NoDecoration|
// NoBackground|... so the overlay can never grab mouse/keyboard from the game. (There
// is no platform/input backend wired anyway; this keeps it correct regardless.)
//
// Visibility is voice-state driven: if !snap.in_voice it draws NOTHING and returns.
#pragma once

#include "iavatar_textures.hpp"

#include <cstdint>

namespace choir {

struct Snapshot;       // ipc/state.hpp
class StateClient;     // src/overlay/state_client.hpp

// Draw the overlay for `snap` into the current ImGui frame.
//   * `textures`  resolves participant avatar ImTextureIDs by hash (render thread).
//   * `client`    supplies the retained AvatarReq for any not-yet-loaded avatar, so a
//                 recreated swapchain still shows avatars (on-demand load by hash).
//   * `extent`    the swapchain extent — anchor math is computed from it.
//   * `now_ms`    a wall-clock (std::chrono::system_clock) ms timestamp, used to expire
//                 toasts against Notification.created_ms.
//
// Never throws; on any missing data it simply draws less. Draws nothing when
// !snap.in_voice.
void draw_overlay(const Snapshot& snap, IAvatarTextures& textures, StateClient& client,
                  Extent2D extent, int64_t now_ms);

}  // namespace choir
