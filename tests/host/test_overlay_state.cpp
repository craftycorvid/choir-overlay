// Tests for the overlay state reducer (Task 8).
//
// OverlayState folds RpcEvents into a choir::Snapshot, bumping `revision` and
// firing `on_change` once per applied change. Pure: no sockets, injectable clock.
//
// The main test drives a synthetic sequence and asserts current() after each
// step, plus an on_change counter and the injected now_ms clock.

#include "model/overlay_state.hpp"

#include "discord/rpc_messages.hpp"
#include "ipc/state.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

using namespace choir;

// --- helpers ---------------------------------------------------------------

static RpcEvent channel_select(const std::string& channel_id) {
    RpcEvent ev;
    ev.kind = RpcEvent::ChannelSelect;
    ev.channel_id = channel_id;
    return ev;
}

static RpcEvent voice(RpcEvent::Kind kind, const std::string& user_id,
                      const std::string& nick, const std::string& avatar,
                      bool mute, bool deaf, bool self_mute, bool self_deaf) {
    RpcEvent ev;
    ev.kind = kind;
    ev.voice.user_id = user_id;
    ev.voice.nick = nick;
    ev.voice.avatar_hash = avatar;
    ev.voice.mute = mute;
    ev.voice.deaf = deaf;
    ev.voice.self_mute = self_mute;
    ev.voice.self_deaf = self_deaf;
    return ev;
}

static RpcEvent speaking(RpcEvent::Kind kind, const std::string& user_id) {
    RpcEvent ev;
    ev.kind = kind;
    ev.user_id = user_id;
    return ev;
}

static RpcEvent notification(const std::string& id, const std::string& title,
                             const std::string& body) {
    RpcEvent ev;
    ev.kind = RpcEvent::Notification;
    ev.notif.id = id;
    ev.notif.title = title;
    ev.notif.body = body;
    return ev;
}

// Find a participant by user_id; returns nullptr if absent.
static const Participant* find(const Snapshot& s, const std::string& uid) {
    for (const auto& p : s.participants) {
        if (p.user_id == uid) return &p;
    }
    return nullptr;
}

// --- the scripted sequence -------------------------------------------------

static void test_sequence() {
    int64_t fake_now = 1000;
    int change_count = 0;
    uint64_t last_seen_revision = 0;

    OverlayState st([&] { return fake_now; });
    st.on_change = [&](const Snapshot& s) {
        ++change_count;
        // on_change must fire AFTER the snapshot is updated: revision must have
        // advanced relative to the last one we saw.
        assert(s.revision > last_seen_revision);
        last_seen_revision = s.revision;
    };

    // Initial state: not in voice, empty.
    assert(!st.current().in_voice);
    assert(st.current().participants.empty());
    assert(st.current().revision == 0);
    assert(change_count == 0);

    // 1) ChannelSelect{"chan-1"} -> in_voice, channel_name set to the id, roster cleared.
    st.apply(channel_select("chan-1"));
    assert(st.current().in_voice);
    assert(st.current().channel_name == "chan-1");
    assert(st.current().participants.empty());
    assert(change_count == 1);
    uint64_t rev_after_select = st.current().revision;
    assert(rev_after_select > 0);

    // 2) VoiceCreate p0 (alice).
    st.apply(voice(RpcEvent::VoiceCreate, "u0", "alice", "av0",
                   /*mute*/ false, /*deaf*/ false, /*self_mute*/ false, /*self_deaf*/ false));
    assert(st.current().participants.size() == 1);
    {
        const Participant* p = find(st.current(), "u0");
        assert(p != nullptr);
        assert(p->display_name == "alice");
        assert(p->avatar_hash == "av0");
        assert(!p->speaking);
        assert(!p->mute && !p->deaf && !p->self_mute && !p->self_deaf);
    }
    assert(change_count == 2);

    // 3) VoiceCreate p1 (bob), self_deaf=true.
    st.apply(voice(RpcEvent::VoiceCreate, "u1", "bob", "av1",
                   /*mute*/ false, /*deaf*/ false, /*self_mute*/ false, /*self_deaf*/ true));
    assert(st.current().participants.size() == 2);
    {
        const Participant* p = find(st.current(), "u1");
        assert(p != nullptr);
        assert(p->display_name == "bob");
        assert(p->self_deaf);
        assert(!p->speaking);
    }
    assert(change_count == 3);

    // 4) SpeakingStart on p0 -> speaking=true.
    st.apply(speaking(RpcEvent::SpeakingStart, "u0"));
    assert(find(st.current(), "u0")->speaking);
    assert(change_count == 4);

    // 5) VoiceUpdate p0 with mute=true MUST preserve speaking=true.
    st.apply(voice(RpcEvent::VoiceUpdate, "u0", "alice", "av0",
                   /*mute*/ true, /*deaf*/ false, /*self_mute*/ false, /*self_deaf*/ false));
    assert(st.current().participants.size() == 2);  // upsert, not insert.
    {
        const Participant* p = find(st.current(), "u0");
        assert(p != nullptr);
        assert(p->mute);
        assert(p->speaking);  // <-- the load-bearing assertion.
    }
    assert(change_count == 5);

    // 6) SpeakingStart on an UNKNOWN user -> safe no-op (no bump, no callback).
    int before = change_count;
    uint64_t rev_before = st.current().revision;
    st.apply(speaking(RpcEvent::SpeakingStart, "ghost"));
    assert(change_count == before);
    assert(st.current().revision == rev_before);

    // 7) SpeakingStop on p0 -> speaking=false.
    st.apply(speaking(RpcEvent::SpeakingStop, "u0"));
    assert(!find(st.current(), "u0")->speaking);
    assert(change_count == 6);

    // 8) VoiceDelete p1 -> removed; index stays consistent (p0 still updatable).
    st.apply(voice(RpcEvent::VoiceDelete, "u1", "", "", false, false, false, false));
    assert(st.current().participants.size() == 1);
    assert(find(st.current(), "u1") == nullptr);
    assert(find(st.current(), "u0") != nullptr);
    assert(change_count == 7);

    // Index consistency check: update p0 again and confirm it still resolves to
    // the same (only) participant rather than a stale index.
    st.apply(voice(RpcEvent::VoiceUpdate, "u0", "alice2", "av0b",
                   false, false, true, false));
    assert(st.current().participants.size() == 1);
    {
        const Participant* p = find(st.current(), "u0");
        assert(p != nullptr);
        assert(p->display_name == "alice2");
        assert(p->avatar_hash == "av0b");
        assert(p->self_mute);
    }
    assert(change_count == 8);

    // 9) Notification -> prepended newest-first with created_ms from the clock.
    fake_now = 5555;
    st.apply(notification("n0", "title0", "body0"));
    assert(st.current().notifications.size() == 1);
    assert(st.current().notifications[0].id == "n0");
    assert(st.current().notifications[0].created_ms == 5555);
    assert(change_count == 9);

    fake_now = 6666;
    st.apply(notification("n1", "title1", "body1"));
    assert(st.current().notifications.size() == 2);
    // Newest-first: n1 in front.
    assert(st.current().notifications[0].id == "n1");
    assert(st.current().notifications[0].created_ms == 6666);
    assert(st.current().notifications[1].id == "n0");
    assert(change_count == 10);

    // 10) ChannelSelect{empty} -> in_voice=false, roster cleared.
    st.apply(channel_select(""));
    assert(!st.current().in_voice);
    assert(st.current().participants.empty());
    assert(change_count == 11);
    // Notifications are not voice-scoped; they persist across channel changes.
    assert(st.current().notifications.size() == 2);

    // Revision strictly increased over the run.
    assert(st.current().revision > rev_after_select);
}

// --- notification cap ------------------------------------------------------

static void test_notification_cap() {
    int64_t fake_now = 42;
    OverlayState st([&] { return fake_now; });

    // Push 25 notifications; only the newest 20 survive, newest-first.
    for (int i = 0; i < 25; ++i) {
        fake_now = 1000 + i;
        st.apply(notification("n" + std::to_string(i), "t", "b"));
    }
    assert(st.current().notifications.size() == 20);
    // Newest (n24) at the front, oldest survivor (n5) at the back.
    assert(st.current().notifications.front().id == "n24");
    assert(st.current().notifications.front().created_ms == 1024);
    assert(st.current().notifications.back().id == "n5");
    assert(st.current().notifications.back().created_ms == 1005);
}

// --- set_config ------------------------------------------------------------

static void test_set_config() {
    OverlayState st([] { return int64_t{0}; });
    int change_count = 0;
    st.on_change = [&](const Snapshot&) { ++change_count; };

    uint64_t rev_before = st.current().revision;
    AppearanceConfig cfg;
    cfg.anchor = Anchor::BottomLeft;
    cfg.scale = 2.0f;
    cfg.opacity = 0.5f;
    cfg.show_all_members = false;
    cfg.toast_duration_ms = 1234;
    st.set_config(cfg);

    assert(st.current().config.anchor == Anchor::BottomLeft);
    assert(st.current().config.scale == 2.0f);
    assert(st.current().config.show_all_members == false);
    assert(st.current().config.toast_duration_ms == 1234);
    assert(st.current().revision > rev_before);
    assert(change_count == 1);
}

// --- empty ChannelSelect while already out of voice still applies ----------

static void test_channel_select_empty_from_empty() {
    OverlayState st([] { return int64_t{0}; });
    int change_count = 0;
    st.on_change = [&](const Snapshot&) { ++change_count; };

    st.apply(channel_select(""));
    assert(!st.current().in_voice);
    // It clears the (already empty) roster and bumps revision: treated as applied.
    assert(change_count == 1);
}

int main() {
    test_sequence();
    test_notification_cap();
    test_set_config();
    test_channel_select_empty_from_empty();
    return 0;
}
