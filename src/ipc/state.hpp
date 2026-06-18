#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace choir {

// CenterLeft/CenterRight hug the left/right edge, vertically centered.
enum class Anchor : uint8_t { TopLeft, TopRight, BottomLeft, BottomRight, CenterLeft, CenterRight };

struct Participant {
    std::string user_id;        // Discord snowflake (string)
    std::string display_name;   // nick / global name
    std::string avatar_hash;    // key into the avatar cache ("" = no avatar yet)
    bool speaking  = false;
    bool mute      = false;     // server mute
    bool deaf      = false;     // server deaf
    bool self_mute = false;
    bool self_deaf = false;
};

struct Notification {
    std::string id;
    std::string title;
    std::string body;
    std::string icon_hash;      // "" = none
    int64_t     created_ms = 0; // wall-clock ms when created; layer expires via config.toast_duration_ms
};

struct AppearanceConfig {
    Anchor  anchor           = Anchor::TopRight;
    float   scale            = 1.0f;
    float   opacity          = 0.90f;
    bool    show_all_members = true;     // false => only currently-speaking members
    Anchor  toast_anchor     = Anchor::TopRight;
    int     toast_duration_ms = 5000;
};

struct Snapshot {
    bool                       in_voice = false;   // visibility gate
    std::string                channel_name;
    std::vector<Participant>   participants;
    std::vector<Notification>  notifications;
    AppearanceConfig           config;
    uint64_t                   revision = 0;       // monotonically increasing
};

// nlohmann (de)serialization via ADL. Declared here (so nested containers can find
// them) and defined in state.cpp. Anchor is serialized as its integer value.
void to_json(nlohmann::json&, const Anchor&);
void from_json(const nlohmann::json&, Anchor&);
void to_json(nlohmann::json&, const Participant&);
void from_json(const nlohmann::json&, Participant&);
void to_json(nlohmann::json&, const Notification&);
void from_json(const nlohmann::json&, Notification&);
void to_json(nlohmann::json&, const AppearanceConfig&);
void from_json(const nlohmann::json&, AppearanceConfig&);
void to_json(nlohmann::json&, const Snapshot&);
void from_json(const nlohmann::json&, Snapshot&);

// Convenience wrappers over the whole Snapshot.
void to_json_str(const Snapshot&, std::string& out);      // compact JSON
bool from_json_str(const std::string& in, Snapshot& out); // false on parse error

} // namespace choir
