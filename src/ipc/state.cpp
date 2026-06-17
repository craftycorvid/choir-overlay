#include "ipc/state.hpp"

#include <nlohmann/json.hpp>

namespace choir {

using nlohmann::json;

// Anchor <-> integer value.
void to_json(json& j, const Anchor& a) {
    j = static_cast<uint8_t>(a);
}
void from_json(const json& j, Anchor& a) {
    a = static_cast<Anchor>(j.get<uint8_t>());
}

void to_json(json& j, const Participant& p) {
    j = json{
        {"user_id", p.user_id},
        {"display_name", p.display_name},
        {"avatar_hash", p.avatar_hash},
        {"speaking", p.speaking},
        {"mute", p.mute},
        {"deaf", p.deaf},
        {"self_mute", p.self_mute},
        {"self_deaf", p.self_deaf},
    };
}
void from_json(const json& j, Participant& p) {
    j.at("user_id").get_to(p.user_id);
    j.at("display_name").get_to(p.display_name);
    j.at("avatar_hash").get_to(p.avatar_hash);
    j.at("speaking").get_to(p.speaking);
    j.at("mute").get_to(p.mute);
    j.at("deaf").get_to(p.deaf);
    j.at("self_mute").get_to(p.self_mute);
    j.at("self_deaf").get_to(p.self_deaf);
}

void to_json(json& j, const Notification& n) {
    j = json{
        {"id", n.id},
        {"title", n.title},
        {"body", n.body},
        {"icon_hash", n.icon_hash},
        {"created_ms", n.created_ms},
    };
}
void from_json(const json& j, Notification& n) {
    j.at("id").get_to(n.id);
    j.at("title").get_to(n.title);
    j.at("body").get_to(n.body);
    j.at("icon_hash").get_to(n.icon_hash);
    j.at("created_ms").get_to(n.created_ms);
}

void to_json(json& j, const AppearanceConfig& c) {
    j = json{
        {"anchor", c.anchor},
        {"scale", c.scale},
        {"opacity", c.opacity},
        {"show_all_members", c.show_all_members},
        {"toast_anchor", c.toast_anchor},
        {"toast_duration_ms", c.toast_duration_ms},
    };
}
void from_json(const json& j, AppearanceConfig& c) {
    j.at("anchor").get_to(c.anchor);
    j.at("scale").get_to(c.scale);
    j.at("opacity").get_to(c.opacity);
    j.at("show_all_members").get_to(c.show_all_members);
    j.at("toast_anchor").get_to(c.toast_anchor);
    j.at("toast_duration_ms").get_to(c.toast_duration_ms);
}

void to_json(json& j, const Snapshot& s) {
    j = json{
        {"in_voice", s.in_voice},
        {"channel_name", s.channel_name},
        {"participants", s.participants},
        {"notifications", s.notifications},
        {"config", s.config},
        {"revision", s.revision},
    };
}
void from_json(const json& j, Snapshot& s) {
    j.at("in_voice").get_to(s.in_voice);
    j.at("channel_name").get_to(s.channel_name);
    j.at("participants").get_to(s.participants);
    j.at("notifications").get_to(s.notifications);
    j.at("config").get_to(s.config);
    j.at("revision").get_to(s.revision);
}

void to_json_str(const Snapshot& s, std::string& out) {
    json j = s;
    out = j.dump(); // compact
}

bool from_json_str(const std::string& in, Snapshot& out) {
    try {
        json j = json::parse(in);
        out = j.get<Snapshot>();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace choir
