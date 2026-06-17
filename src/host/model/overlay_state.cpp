#include "model/overlay_state.hpp"

#include <chrono>
#include <utility>

namespace choir {

OverlayState::OverlayState(std::function<int64_t()> now_ms)
    : now_ms_(std::move(now_ms)) {
    if (!now_ms_) {
        now_ms_ = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        };
    }
}

void OverlayState::commit() {
    ++snap_.revision;
    if (on_change) on_change(snap_);
}

void OverlayState::apply(const RpcEvent& ev) {
    switch (ev.kind) {
        case RpcEvent::ChannelSelect: {
            // Non-empty id -> entering/switching channels: visible, name = id
            // (a later task resolves a friendly name). Empty id -> left voice.
            // Either way the roster + index are cleared (the new channel's
            // VOICE_STATE_CREATEs repopulate it). Notifications are NOT
            // voice-scoped and are preserved.
            snap_.in_voice = !ev.channel_id.empty();
            snap_.channel_name = ev.channel_id;
            snap_.participants.clear();
            index_.clear();
            commit();
            return;
        }

        case RpcEvent::VoiceCreate:
        case RpcEvent::VoiceUpdate: {
            const VoiceState& v = ev.voice;
            auto it = index_.find(v.user_id);
            Participant* p = nullptr;
            if (it == index_.end()) {
                Participant np;
                np.user_id = v.user_id;
                np.speaking = false;  // VoiceState carries no speaking flag.
                index_[v.user_id] = snap_.participants.size();
                snap_.participants.push_back(std::move(np));
                p = &snap_.participants.back();
            } else {
                p = &snap_.participants[it->second];
            }
            // Update everything EXCEPT speaking, which is owned solely by the
            // SpeakingStart/Stop events and must survive an update.
            p->display_name = v.nick;
            p->avatar_hash = v.avatar_hash;
            p->mute = v.mute;
            p->deaf = v.deaf;
            p->self_mute = v.self_mute;
            p->self_deaf = v.self_deaf;
            commit();
            return;
        }

        case RpcEvent::VoiceDelete: {
            auto it = index_.find(ev.voice.user_id);
            if (it == index_.end()) return;  // unknown -> no-op.
            snap_.participants.erase(snap_.participants.begin() +
                                     static_cast<std::ptrdiff_t>(it->second));
            // Full rebuild of the index is simplest and bug-proof after erase.
            index_.clear();
            for (size_t i = 0; i < snap_.participants.size(); ++i) {
                index_[snap_.participants[i].user_id] = i;
            }
            commit();
            return;
        }

        case RpcEvent::SpeakingStart:
        case RpcEvent::SpeakingStop: {
            auto it = index_.find(ev.user_id);
            if (it == index_.end()) return;  // unknown user -> safe no-op.
            snap_.participants[it->second].speaking =
                (ev.kind == RpcEvent::SpeakingStart);
            commit();
            return;
        }

        case RpcEvent::Notification: {
            Notification n = ev.notif;
            n.created_ms = now_ms_();
            snap_.notifications.insert(snap_.notifications.begin(),
                                       std::move(n));  // newest-first.
            if (snap_.notifications.size() > kMaxNotifications) {
                snap_.notifications.resize(kMaxNotifications);  // drop oldest.
            }
            commit();
            return;
        }
    }
}

void OverlayState::set_config(const AppearanceConfig& cfg) {
    snap_.config = cfg;
    commit();
}

}  // namespace choir
