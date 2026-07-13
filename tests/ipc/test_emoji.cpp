#include "ipc/emoji.hpp"
#include <cassert>
#include <string>
#include <vector>

int main() {
    using namespace choir::emoji;

    // Plain text: one coalesced run, empty key. Empty input: no runs.
    {
        auto r = split_runs("hello world");
        assert(r.size() == 1);
        assert(r[0].key.empty());
        assert(r[0].text == "hello world");
    }
    assert(split_runs("").empty());

    // Custom emoji markup; animated form maps to the same static key namespace.
    {
        auto r = split_runs("<:pog:123>");
        assert(r.size() == 1);
        assert(r[0].key == "emoji.c.123");
        assert(r[0].text == ":pog:");
    }
    {
        auto r = split_runs("<a:party:456>");
        assert(r.size() == 1);
        assert(r[0].key == "emoji.c.456");
        assert(r[0].text == ":party:");
    }

    // Malformed markup passes through as text, byte-for-byte.
    for (const char* s : {"<:oops", "<::1>", "<:name:abc>", "<:name:1x>", "<name:1>", "<>"}) {
        auto r = split_runs(s);
        assert(r.size() == 1);
        assert(r[0].key.empty());
        assert(r[0].text == s);
    }

    // Single unicode emoji.
    {
        auto r = split_runs("\U0001F604");  // 😄
        assert(r.size() == 1);
        assert(r[0].key == "emoji.u.1f604");
        assert(r[0].text == "\U0001F604");
    }

    // VS16 stripped from the stem when the sequence has no ZWJ.
    {
        auto r = split_runs("☂️");  // ☂️
        assert(r.size() == 1);
        assert(r[0].key == "emoji.u.2602");
    }

    // VS16 kept when a ZWJ is present.
    {
        auto r = split_runs("❤️‍\U0001F525");  // ❤️‍🔥
        assert(r.size() == 1);
        assert(r[0].key == "emoji.u.2764-fe0f-200d-1f525");
    }

    // Skin-tone modifier joins the sequence.
    {
        auto r = split_runs("\U0001F44D\U0001F3FD");  // 👍🏽
        assert(r.size() == 1);
        assert(r[0].key == "emoji.u.1f44d-1f3fd");
    }

    // Regional-indicator pair (flag).
    {
        auto r = split_runs("\U0001F1E9\U0001F1EA");  // 🇩🇪
        assert(r.size() == 1);
        assert(r[0].key == "emoji.u.1f1e9-1f1ea");
    }

    // Keycap: unpadded hex, VS16 stripped (23-20e3, NOT 0023-fe0f-20e3).
    {
        auto r = split_runs("#️⃣");  // #️⃣
        assert(r.size() == 1);
        assert(r[0].key == "emoji.u.23-20e3");
    }

    // ZWJ family chain.
    {
        auto r = split_runs("\U0001F468‍\U0001F469‍\U0001F467");  // 👨‍👩‍👧
        assert(r.size() == 1);
        assert(r[0].key == "emoji.u.1f468-200d-1f469-200d-1f467");
    }

    // Mixed content with text coalescing around emoji runs.
    {
        auto r = split_runs("go <:pog:1> now \U0001F604!");
        assert(r.size() == 5);
        assert(r[0].text == "go " && r[0].key.empty());
        assert(r[1].key == "emoji.c.1");
        assert(r[2].text == " now " && r[2].key.empty());
        assert(r[3].key == "emoji.u.1f604");
        assert(r[4].text == "!" && r[4].key.empty());
    }

    // Plain digits are not keycaps; bare text-default symbols (©) stay text.
    {
        auto r = split_runs("5 games");
        assert(r.size() == 1 && r[0].key.empty());
    }
    {
        auto r = split_runs("© 2026 Corp");  // © without VS16
        assert(r.size() == 1 && r[0].key.empty());
    }
    {
        auto r = split_runs("©️");  // ©️ with VS16 IS emoji
        assert(r.size() == 1);
        assert(r[0].key == "emoji.u.a9");
    }

    // Invalid UTF-8 passes through as text without crashing.
    {
        std::string bad = "ab\xFF\xFE cd";
        auto r = split_runs(bad);
        assert(r.size() == 1);
        assert(r[0].key.empty());
        assert(r[0].text == bad);
    }

    // twemoji_stem directly.
    assert(twemoji_stem({0x1F604}) == "1f604");
    assert(twemoji_stem({0x23, 0xFE0F, 0x20E3}) == "23-20e3");
    assert(twemoji_stem({0x2764, 0xFE0F, 0x200D, 0x1F525}) == "2764-fe0f-200d-1f525");

    // url_for: both namespaces; non-emoji keys map to "".
    assert(url_for("emoji.c.123") == "https://cdn.discordapp.com/emojis/123.png?size=64");
    assert(url_for("emoji.u.1f604") ==
           "https://cdn.jsdelivr.net/gh/jdecked/twemoji@v17.0.3/assets/72x72/1f604.png");
    assert(url_for("avatarA").empty());
    assert(url_for("").empty());

    return 0;
}
