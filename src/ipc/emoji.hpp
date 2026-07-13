// Emoji detection shared by the host (which fetches emoji images into the avatar
// cache) and the overlay (which lays them out as inline images in toasts). Both
// sides parse the SAME notification text with the SAME code, so the cache keys
// they derive always agree — that is the whole IPC contract; no protocol changes.
//
// Keys: "emoji.c.<id>" (Discord custom emoji, <a?:name:id> markup) and
// "emoji.u.<stem>" (unicode emoji, twemoji filename stem). url_for() maps a key
// to its CDN image URL for the host's fetch.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace choir::emoji {

// One run of notification text: plain text (key empty) or an inline emoji image
// (key set). For emoji runs `text` is the drawable fallback when the image is
// unavailable: ":name:" for custom emoji, the raw codepoint sequence for unicode.
struct Run {
    std::string text;
    std::string key;  // "" | "emoji.c.<id>" | "emoji.u.<stem>"
};

// Split UTF-8 `s` into text and emoji runs. Detects <a?:name:id> custom-emoji
// markup and unicode emoji sequences (VS16, ZWJ joins, skin tones, regional
// indicator pairs, keycaps). Adjacent text coalesces into one run; malformed
// markup and invalid UTF-8 pass through as text.
std::vector<Run> split_runs(const std::string& s);

// Twemoji filename stem: unpadded lowercase hex codepoints joined by '-';
// U+FE0F stripped iff the sequence contains no U+200D ZWJ. Exposed for tests.
std::string twemoji_stem(const std::vector<uint32_t>& cps);

// CDN image URL for an emoji cache key; "" for non-emoji keys. Animated custom
// emojis resolve to .png, which Discord's CDN serves as a static first frame.
std::string url_for(const std::string& key);

}  // namespace choir::emoji
