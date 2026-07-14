#include "ipc/emoji.hpp"

#include <cctype>
#include <cstdio>
#include <string_view>
#include <unordered_map>

namespace choir::emoji {
namespace {

// Decode one UTF-8 codepoint at [p, end). Returns bytes consumed, 0 on invalid
// input (overlong forms are not rejected — the caller only routes bytes, never
// re-encodes, so a decoded-then-sliced run is always the original bytes).
size_t decode_utf8(const char* p, const char* end, uint32_t& cp) {
    const auto b = [&](size_t i) { return static_cast<unsigned char>(p[i]); };
    const size_t avail = static_cast<size_t>(end - p);
    if (avail == 0) return 0;
    if (b(0) < 0x80) { cp = b(0); return 1; }
    size_t len;
    if ((b(0) & 0xE0) == 0xC0) { len = 2; cp = b(0) & 0x1F; }
    else if ((b(0) & 0xF0) == 0xE0) { len = 3; cp = b(0) & 0x0F; }
    else if ((b(0) & 0xF8) == 0xF0) { len = 4; cp = b(0) & 0x07; }
    else return 0;
    if (avail < len) return 0;
    for (size_t i = 1; i < len; ++i) {
        if ((b(i) & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (b(i) & 0x3F);
    }
    return len;
}

// ponytail: pragmatic emoji tables, not UCD-complete — misses degrade to today's
// text rendering and CDN 404s fall back the same way; generate from Unicode
// emoji-data.txt if coverage complaints arrive.
//
// Codepoints that are emoji on their own (emoji-presentation by default).
bool always_emoji(uint32_t cp) {
    return (cp >= 0x1F000 && cp <= 0x1FAFF)   // pictograph superblocks (incl. RI)
        || (cp >= 0x2600 && cp <= 0x27BF)     // misc symbols + dingbats
        || (cp >= 0x2B05 && cp <= 0x2B07)     // ⬅⬆⬇
        || cp == 0x2B1B || cp == 0x2B1C || cp == 0x2B50 || cp == 0x2B55
        || cp == 0x231A || cp == 0x231B       // ⌚⌛
        || (cp >= 0x23E9 && cp <= 0x23FA);    // media-control block
}

// Text-default symbols that count as emoji only when followed by VS16 (U+FE0F),
// so bare ©/®/™/arrows in prose stay text.
bool vs16_gated(uint32_t cp) {
    return cp == 0x00A9 || cp == 0x00AE || cp == 0x2122
        || cp == 0x203C || cp == 0x2049 || cp == 0x2139
        || (cp >= 0x2194 && cp <= 0x2199) || cp == 0x21A9 || cp == 0x21AA
        || cp == 0x2328 || cp == 0x23CF || cp == 0x24C2
        || cp == 0x25AA || cp == 0x25AB || cp == 0x25B6 || cp == 0x25C0
        || (cp >= 0x25FB && cp <= 0x25FE)
        || cp == 0x2934 || cp == 0x2935
        || cp == 0x3030 || cp == 0x303D || cp == 0x3297 || cp == 0x3299;
}

bool is_regional_indicator(uint32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }
bool is_skin_tone(uint32_t cp) { return cp >= 0x1F3FB && cp <= 0x1F3FF; }
bool is_tag(uint32_t cp) { return cp >= 0xE0020 && cp <= 0xE007F; }  // subdivision flags
constexpr uint32_t kVS16 = 0xFE0F;
constexpr uint32_t kZWJ = 0x200D;
constexpr uint32_t kKeycap = 0x20E3;

// Match <a?:name:id> at p. On success sets name/id and returns bytes consumed.
size_t match_custom(const char* p, const char* end, std::string& name, std::string& id) {
    const char* q = p;
    if (q == end || *q != '<') return 0;
    ++q;
    if (q != end && *q == 'a') ++q;
    if (q == end || *q != ':') return 0;
    ++q;
    const char* name_beg = q;
    while (q != end && (std::isalnum(static_cast<unsigned char>(*q)) || *q == '_')) ++q;
    if (q == name_beg || q == end || *q != ':') return 0;
    name.assign(name_beg, q);
    ++q;
    const char* id_beg = q;
    while (q != end && std::isdigit(static_cast<unsigned char>(*q))) ++q;
    if (q == id_beg || q == end || *q != '>') return 0;
    id.assign(id_beg, q);
    ++q;
    return static_cast<size_t>(q - p);
}

// Match one unicode emoji sequence at p. On success fills cps and returns bytes
// consumed (0 = not an emoji here).
size_t match_unicode(const char* p, const char* end, std::vector<uint32_t>& cps) {
    cps.clear();
    uint32_t cp;
    size_t n = decode_utf8(p, end, cp);
    if (n == 0) return 0;
    const char* q = p + n;

    // Keycaps: [0-9#*] + optional VS16 + U+20E3. Bare digits stay text.
    if ((cp >= '0' && cp <= '9') || cp == '#' || cp == '*') {
        uint32_t cp2;
        const char* r = q;
        size_t n2 = decode_utf8(r, end, cp2);
        if (n2 && cp2 == kVS16) {
            r += n2;
            n2 = decode_utf8(r, end, cp2);
        }
        if (!n2 || cp2 != kKeycap) return 0;
        cps = {cp, kVS16, cp2};  // stem strips the VS16 anyway
        return static_cast<size_t>(r + n2 - p);
    }

    if (is_regional_indicator(cp)) {
        cps.push_back(cp);
        uint32_t cp2;
        const size_t n2 = decode_utf8(q, end, cp2);
        if (n2 && is_regional_indicator(cp2)) {  // pair = flag; single RI stands alone
            cps.push_back(cp2);
            q += n2;
        }
        return static_cast<size_t>(q - p);
    }

    if (always_emoji(cp)) {
        cps.push_back(cp);
    } else if (vs16_gated(cp)) {
        uint32_t cp2;
        const size_t n2 = decode_utf8(q, end, cp2);
        if (!n2 || cp2 != kVS16) return 0;
        cps.push_back(cp);
        cps.push_back(kVS16);
        q += n2;
    } else {
        return 0;
    }

    // Extend: skin tones, VS16, tag characters, ZWJ + further emoji.
    for (;;) {
        uint32_t cp2;
        size_t n2 = decode_utf8(q, end, cp2);
        if (!n2) break;
        if (is_skin_tone(cp2) || cp2 == kVS16 || is_tag(cp2)) {
            cps.push_back(cp2);
            q += n2;
            continue;
        }
        if (cp2 == kZWJ) {
            uint32_t cp3;
            const size_t n3 = decode_utf8(q + n2, end, cp3);
            if (n3 && (always_emoji(cp3) || vs16_gated(cp3))) {
                cps.push_back(kZWJ);
                cps.push_back(cp3);
                q += n2 + n3;
                continue;
            }
        }
        break;
    }
    return static_cast<size_t>(q - p);
}

}  // namespace

std::string twemoji_stem(const std::vector<uint32_t>& cps) {
    bool has_zwj = false;
    for (uint32_t cp : cps)
        if (cp == kZWJ) has_zwj = true;
    std::string stem;
    char buf[16];
    for (uint32_t cp : cps) {
        if (cp == kVS16 && !has_zwj) continue;
        std::snprintf(buf, sizeof(buf), "%x", cp);
        if (!stem.empty()) stem += '-';
        stem += buf;
    }
    return stem;
}

std::vector<Run> split_runs(const std::string& s) {
    std::vector<Run> runs;
    std::string text;
    const auto flush = [&] {
        if (!text.empty()) {
            runs.push_back({std::move(text), ""});
            text.clear();
        }
    };

    const char* p = s.data();
    const char* end = p + s.size();
    while (p != end) {
        if (*p == '<') {
            std::string name, id;
            if (const size_t n = match_custom(p, end, name, id)) {
                flush();
                runs.push_back({":" + name + ":", "emoji.c." + id});
                p += n;
                continue;
            }
        }
        std::vector<uint32_t> cps;
        if (const size_t n = match_unicode(p, end, cps)) {
            flush();
            runs.push_back({std::string(p, n), "emoji.u." + twemoji_stem(cps)});
            p += n;
            continue;
        }
        uint32_t cp;
        const size_t n = decode_utf8(p, end, cp);
        text.append(p, n ? n : 1);  // invalid UTF-8: pass the byte through
        p += n ? n : 1;
    }
    flush();
    return runs;
}

std::string url_for(const std::string& key) {
    constexpr std::string_view kCustom = "emoji.c.";
    constexpr std::string_view kUnicode = "emoji.u.";
    if (key.compare(0, kCustom.size(), kCustom) == 0 && key.size() > kCustom.size())
        return "https://cdn.discordapp.com/emojis/" + key.substr(kCustom.size()) +
               ".png?size=64";
    if (key.compare(0, kUnicode.size(), kUnicode) == 0 && key.size() > kUnicode.size())
        return "https://cdn.jsdelivr.net/gh/jdecked/twemoji@v17.0.3/assets/72x72/" +
               key.substr(kUnicode.size()) + ".png";
    return "";
}

std::string restore_custom_markup(const std::string& display, const std::string& raw) {
    // Harvest name -> full "<a?:name:id>" markup from the raw message content.
    std::unordered_map<std::string, std::string> markup;
    for (const char* p = raw.data(), *end = p + raw.size(); p != end;) {
        std::string name, id;
        if (const size_t n = match_custom(p, end, name, id)) {
            markup[name] = std::string(p, n);  // last wins on duplicate names
            p += n;
        } else {
            ++p;
        }
    }
    if (markup.empty()) return display;

    // Rewrite each ":name:" shortcode in the display body back to its markup.
    std::string out;
    out.reserve(display.size());
    const char* p = display.data();
    const char* end = p + display.size();
    while (p != end) {
        if (*p == ':') {
            const char* q = p + 1;
            while (q != end && (std::isalnum(static_cast<unsigned char>(*q)) || *q == '_')) ++q;
            if (q != end && *q == ':' && q > p + 1) {
                auto it = markup.find(std::string(p + 1, q));
                if (it != markup.end()) {
                    out += it->second;
                    p = q + 1;
                    continue;
                }
            }
        }
        out += *p++;
    }
    return out;
}

}  // namespace choir::emoji
