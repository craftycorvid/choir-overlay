# Emoji support in notification toasts

## Problem statement

Notification toasts render title/body as raw text through ImGui's default ASCII-only
font. Discord custom emojis arrive as literal `<:name:id>` / `<a:name:id>` markup and
render as that markup; Unicode emoji (😄, ZWJ sequences, skin tones, flags) render as
`?` missing-glyph boxes. For anyone whose messages contain emoji — i.e. everyone — toast
bodies are noisy or unreadable.

## Approach

Render both emoji kinds as **inline images riding the existing avatar pipeline**, under
namespaced cache keys:

- **Custom emojis** (`<:name:id>`, `<a:name:id>`): key `emoji.c.<id>`, fetched from
  `https://cdn.discordapp.com/emojis/<id>.png?size=64`. Animated emojis fetch the same
  `.png` URL, which serves a static first frame.
- **Unicode emoji**: key `emoji.u.<stem>`, fetched from
  `https://cdn.jsdelivr.net/gh/jdecked/twemoji@v17.0.3/assets/72x72/<stem>.png` (pinned
  tag; verified live). Twemoji is the emoji art Discord itself uses. Stem = unpadded
  lowercase hex codepoints joined by `-`, with `FE0F` stripped iff the sequence contains
  no `200D` (keycap `#️⃣` → `23-20e3`).

Three pieces:

1. **Shared parser** (`src/ipc/emoji.{hpp,cpp}`, linked by host and overlay via the
   existing `libchoir_ipc`): splits UTF-8 into text/emoji runs. Custom markup by grammar;
   Unicode emoji by a pragmatic codepoint-range table plus sequence assembly (VS16, ZWJ
   chains, skin-tone modifiers, regional-indicator pairs, keycaps). Misdetection in
   either direction degrades to today's rendering.
2. **Host fetch**: `AvatarCache::request_url(key, url)` (generalizes the existing
   `request`); on each notification the host scans title+body and fetches each distinct
   emoji image (capped at 16 per notification — the fetch is synchronous). Images flow to
   the overlay through the existing `.rgba` cache file + `AvatarReady{hash,path,w,h}`
   frame. **Zero IPC protocol or Snapshot schema changes.**
3. **Overlay layout** (`src/overlay/overlay_ui.cpp`): toast title/body layout moves from
   line-wrapped plain strings (`wrap_text`) to positioned mixed runs — text drawn via
   `AddText`, emoji drawn via `AddImage` as an em-square at line height, textures
   resolved exactly like toast avatar icons. Wrapping and last-line `"..."` ellipsis
   semantics are preserved. An emoji whose texture is missing (fetch failed, not yet
   arrived) lays out as fallback text (`:name:` for custom, the raw sequence for
   Unicode). Shared code → both the Vulkan layer and GL interposer get it.

### Rejected alternatives

- **Color emoji font** (Noto Color Emoji via imgui_freetype): adds a freetype dependency
  compiled into both injected backends (the Vulkan layer loads into every game, including
  gamescope), needs fontconfig discovery, doesn't match Discord's Twemoji look — and
  custom emojis need the whole image path anyway, so the font is strictly additional
  machinery covering only half the feature.
- **Custom emojis only**: smallest diff, but most messages use Unicode emoji; half the
  feature.

## Scope boundaries (non-goals)

- No animated emoji playback (static first frame only).
- No emoji in voice-panel participant names (toasts only; the run layout makes this easy
  later).
- No jumbo/enlarged rendering of emoji-only messages.
- No async host fetch rework (per-notification fetch cap instead).
- No full UCD emoji-data import (pragmatic range table; UCD codegen is the upgrade path).

## Acceptance criteria

- Parser unit test (`tests/ipc/test_emoji.cpp`) passes: markup (incl. animated +
  malformed passthrough), single emoji, VS16 stripping (`☂️` → `2602`), ZWJ retention
  (`❤️‍🔥` → `2764-fe0f-200d-1f525`), skin tones, flag pairs, keycaps, ZWJ family, mixed
  strings with text coalescing, digits not detected, invalid UTF-8 passthrough, URL
  mapping for both key namespaces.
- `AvatarCache` tests pass: `request_url` fetch/dedupe/disk-hit/failure-retry semantics
  identical to `request`; notification scan dedupes across title+body and caps at 16;
  existing avatar tests pass unchanged.
- Golden layer test: a toast whose body starts with `<:pog:9001> 😄` shows the seeded
  yellow (custom) and magenta (Unicode) emoji pixels inside the toast card, end-to-end
  through fake_host → IPC → texture upload → run layout.
- All pre-existing tests stay green; `scripts/verify.sh` passes.
- Manual: a real Discord message containing a custom emoji + 😄 while in voice shows both
  images in the toast (Vulkan layer; GL via `choir-run` optional).

## Resolved questions

- **Does NOTIFICATION_CREATE `body` carry raw `<:name:id>` markup?** No (confirmed live).
  The `body` is Discord's display-rendered text: custom emoji are collapsed to `:name:`
  (the id is dropped), while Unicode emoji survive as real glyphs. The id lives only in
  `message.content` as raw `<a?:name:id>` markup. So the host re-injects it before handing
  the body to the overlay: `emoji::restore_custom_markup(body, message.content)` maps each
  markup token back onto the matching `:name:` shortcode. Unmatched shortcodes and Unicode
  emoji are untouched; a no-op when content carries no markup (safe fallback = old behavior).
