#include "ipc/state.hpp"
#include <cassert>
#include <string>

int main() {
    using namespace choir;

    Snapshot in;
    in.in_voice = true;
    in.channel_name = "General";

    Participant p0;
    p0.user_id = "111111111111111111";
    p0.display_name = "Alice";
    p0.avatar_hash = "abc123";
    p0.speaking = true;
    p0.self_mute = true;
    in.participants.push_back(p0);

    Participant p1;
    p1.user_id = "222222222222222222";
    p1.display_name = "Bob";
    p1.avatar_hash = "";
    p1.deaf = true;
    in.participants.push_back(p1);

    Notification n0;
    n0.id = "n1";
    n0.title = "New message";
    n0.body = "hello world";
    n0.icon_hash = "iconhash";
    n0.created_ms = 1718600000000LL;
    in.notifications.push_back(n0);

    in.config.anchor = Anchor::BottomLeft;
    in.config.scale = 1.5f;
    in.config.opacity = 0.5f;
    in.config.show_all_members = false;
    in.config.toast_anchor = Anchor::TopLeft;
    in.config.toast_duration_ms = 3000;

    in.revision = 7;

    std::string json;
    to_json_str(in, json);

    Snapshot out;
    bool ok = from_json_str(json, out);
    assert(ok);

    // Top-level
    assert(out.in_voice == in.in_voice);
    assert(out.channel_name == in.channel_name);
    assert(out.revision == in.revision);

    // Participants
    assert(out.participants.size() == 2);
    assert(out.participants[0].user_id == p0.user_id);
    assert(out.participants[0].display_name == p0.display_name);
    assert(out.participants[0].avatar_hash == p0.avatar_hash);
    assert(out.participants[0].speaking == true);
    assert(out.participants[0].self_mute == true);
    assert(out.participants[0].mute == false);
    assert(out.participants[0].deaf == false);
    assert(out.participants[0].self_deaf == false);

    assert(out.participants[1].user_id == p1.user_id);
    assert(out.participants[1].display_name == p1.display_name);
    assert(out.participants[1].avatar_hash == "");
    assert(out.participants[1].speaking == false);
    assert(out.participants[1].deaf == true);

    // Notifications
    assert(out.notifications.size() == 1);
    assert(out.notifications[0].id == n0.id);
    assert(out.notifications[0].title == n0.title);
    assert(out.notifications[0].body == n0.body);
    assert(out.notifications[0].icon_hash == n0.icon_hash);
    assert(out.notifications[0].created_ms == n0.created_ms);

    // Config (incl. enum)
    assert(out.config.anchor == Anchor::BottomLeft);
    assert(out.config.scale == 1.5f);
    assert(out.config.opacity == 0.5f);
    assert(out.config.show_all_members == false);
    assert(out.config.toast_anchor == Anchor::TopLeft);
    assert(out.config.toast_duration_ms == 3000);

    // Anchor serialized as its integer value.
    {
        std::string j2;
        Snapshot s;
        s.config.anchor = Anchor::BottomRight; // value 3
        to_json_str(s, j2);
        assert(j2.find("\"anchor\":3") != std::string::npos);
    }

    // from_json_str returns false on parse error.
    {
        Snapshot bad;
        assert(from_json_str("{not valid json", bad) == false);
    }

    return 0;
}
