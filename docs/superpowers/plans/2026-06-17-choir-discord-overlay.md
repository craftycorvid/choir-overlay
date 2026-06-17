# Choir — Wayland Discord Overlay: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Wayland-only, display-only Discord overlay that renders a voice panel + notification toasts *inside* Vulkan games (native / DXVK / VKD3D) via an injected Vulkan implicit layer, fed by a Qt host that talks to the Discord desktop client's local RPC.

**Architecture:** Two binaries + one shared lib. **`choir`** (Qt6 host, tray) owns the Discord RPC connection, OAuth, the state model, the avatar cache, and a local unix-socket **state server**. **`libchoir_overlay.so`** (Vulkan implicit layer) is injected into each game, connects to the host as a read-only client, and draws the overlay with Dear ImGui by hooking `vkQueuePresentKHR`. **`libchoir_ipc`** (Qt-free static lib) defines the wire contract shared by both. Networking/secrets live only in the host; the layer is read-only, click-through, and never crashes the game.

**Tech Stack:** C++20, Meson; Qt6 (Widgets + Network) for the host; Dear ImGui (MIT) + Vulkan loader/headers for the layer; nlohmann/json (MIT, header-only) for all JSON; POSIX unix sockets for the local IPC. 64-bit, Vulkan-only for v1. License: MIT.

**Reference spec:** `docs/superpowers/specs/2026-06-17-discord-overlay-design.md` (read it first — it holds the locked decisions, risks, and verified research references).

---

## Conventions

- **Namespace:** all non-layer C++ lives in `namespace choir`. Layer Vulkan entrypoints are `extern "C"`.
- **Build:** every task ends green with `meson compile -C build` and (where applicable) `meson test -C build`.
- **TDD:** within each code task, write the failing test first, then the implementation. Red → green → refactor → commit. Commit at the end of each task.
- **Commit message trailer:** end every commit body with
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- **Two manual gates (Tasks 7 and 20)** require a real Discord client / real game and human observation — they are run by the user, not a subagent. Everything else is automated (unit/mock/golden tests).

---

## File structure (locked during planning)

```
discord-overlay/
├── meson.build                       # top-level: subprojects, options, subdirs
├── meson_options.txt                 # -Dbuild_tests, -Dprefix for per-user install
├── LICENSE                           # MIT
├── README.md                         # build/install/usage + "not affiliated with Discord"
├── subprojects/                      # imgui.wrap, nlohmann_json.wrap (meson wrapdb)
├── src/
│   ├── ipc/                          # libchoir_ipc  (Qt-free, links into host AND layer)
│   │   ├── meson.build
│   │   ├── state.hpp                 # Participant, Notification, AppearanceConfig, Snapshot, Anchor
│   │   ├── state.cpp                 # JSON (de)serialization of the above
│   │   ├── protocol.hpp             # MsgType, frame encode/decode helpers
│   │   ├── protocol.cpp
│   │   ├── framing.hpp / framing.cpp # length-prefixed frame read/write over an fd
│   │   ├── avatar_file.hpp / .cpp    # on-disk RGBA cache file format (shared by host+layer)
│   │   └── paths.hpp / paths.cpp     # XDG runtime/cache/config dir resolution
│   ├── host/                         # `choir` binary (Qt6)
│   │   ├── meson.build
│   │   ├── main.cpp
│   │   ├── discord/
│   │   │   ├── ipc_transport.hpp/.cpp   # connect discord-ipc-{0..9}, op/len framing
│   │   │   ├── rpc_messages.hpp/.cpp    # build commands / parse events (JSON)
│   │   │   ├── oauth.hpp/.cpp           # code→token (Streamkit + own-app modes)
│   │   │   └── rpc_client.hpp/.cpp      # handshake→authorize→authenticate→subscribe FSM
│   │   ├── model/
│   │   │   ├── overlay_state.hpp/.cpp   # RPC events → choir::Snapshot reducer
│   │   │   └── avatar_cache.hpp/.cpp    # fetch+decode (QImage) → .rgba cache
│   │   ├── server/
│   │   │   └── state_server.hpp/.cpp    # QLocalServer on choir.sock; serve snapshot/avatars
│   │   ├── config/
│   │   │   ├── config.hpp/.cpp          # load/save config.json; token storage (0600)
│   │   │   └── denylist.hpp/.cpp        # process-name glob matching + defaults
│   │   └── ui/
│   │       ├── tray.hpp/.cpp
│   │       └── settings_window.hpp/.cpp
│   └── layer/                        # libchoir_overlay.so  (no Qt)
│       ├── meson.build
│       ├── layer_entry.cpp           # vkGetInstanceProcAddr + Negotiate + export table
│       ├── dispatch.hpp/.cpp         # instance/device dispatch tables + create/destroy hooks
│       ├── gating.hpp/.cpp           # DISABLE_CHOIR_OVERLAY env check
│       ├── swapchain.hpp/.cpp        # CreateSwapchainKHR / QueuePresentKHR hooks
│       ├── imgui_renderer.hpp/.cpp   # ImGui Vulkan backend wiring
│       ├── overlay_ui.hpp/.cpp       # voice panel + toasts ImGui drawing
│       ├── state_client.hpp/.cpp     # POSIX client of choir.sock; double-buffered snapshot
│       ├── avatar_textures.hpp/.cpp  # .rgba → VkImage/descriptor, keyed by hash
│       └── manifest/choir_overlay.json.in  # implicit layer manifest template
├── tests/
│   ├── meson.build
│   ├── ipc/        test_state.cpp, test_protocol.cpp, test_framing.cpp, test_avatar_file.cpp
│   ├── host/       test_rpc_messages.cpp, test_oauth.cpp, test_rpc_client.cpp,
│   │               test_overlay_state.cpp, test_avatar_cache.cpp, test_state_server.cpp,
│   │               test_config.cpp, test_denylist.cpp
│   └── harness/
│       ├── fake_host.cpp             # standalone choir.sock server with scripted state
│       ├── vk_min_present.cpp        # headless Vulkan present app (EXT_headless_surface)
│       └── test_layer_golden.cpp     # loads layer into vk_min_present, golden-image compares
└── packaging/                        # (Task 19) install script / meson install hooks
```

---

## The wire contract (defined in Task 2, referenced everywhere — do not redefine)

```cpp
// src/ipc/state.hpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace choir {

enum class Anchor : uint8_t { TopLeft, TopRight, BottomLeft, BottomRight };

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

// JSON (de)serialization (nlohmann adl_serializer specializations) declared here, defined in state.cpp.
void to_json_str(const Snapshot&, std::string& out);     // returns compact JSON
bool from_json_str(const std::string& in, Snapshot& out);// false on parse error

} // namespace choir
```

```cpp
// src/ipc/protocol.hpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace choir {

enum class MsgType : uint8_t {
    Hello       = 1,  // layer -> host : {"pid":int,"exe":string,"proto":1}
    Snapshot    = 2,  // host  -> layer: full choir::Snapshot as JSON  (v1: full state every change)
    AvatarReady = 3,  // host  -> layer: {"hash":string,"path":string,"w":int,"h":int}
    Disabled    = 4,  // host  -> layer: this process is denylisted; stay inert
    Ping        = 5,
    Pong        = 6,
};

// A frame on the wire = [uint32 LE total_len][uint8 type][payload bytes(total_len-1)].
// encode_frame appends to `out`; decode pulls one frame from a buffer.
void encode_frame(MsgType type, const std::string& payload, std::vector<uint8_t>& out);

struct DecodedFrame { MsgType type; std::string payload; };
// Returns number of bytes consumed (0 if a full frame isn't buffered yet), sets `frame`.
size_t try_decode_frame(const uint8_t* buf, size_t len, DecodedFrame& frame);

} // namespace choir
```

```cpp
// src/ipc/avatar_file.hpp  (shared on-disk format so host writes / layer reads identically)
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace choir {
// File layout: magic "CHAV"(4) | uint32 LE width | uint32 LE height | width*height*4 RGBA8 bytes.
bool write_avatar_rgba(const std::string& path, uint32_t w, uint32_t h, const uint8_t* rgba);
bool read_avatar_rgba (const std::string& path, uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgba);
} // namespace choir
```

These three headers are the contract. Every later task uses these exact names/fields.

---

### Task 1: Project scaffold + build skeleton

**Goal:** A Meson project that configures and builds an empty `choir` host stub, an empty `libchoir_overlay.so` stub, the `libchoir_ipc` static lib stub, and a test runner — all green, committed.

**Files:**
- Create: `meson.build`, `meson_options.txt`, `LICENSE` (MIT), `README.md`
- Create: `subprojects/imgui.wrap`, `subprojects/nlohmann_json.wrap`
- Create: `src/ipc/meson.build`, `src/ipc/paths.hpp`, `src/ipc/paths.cpp`
- Create: `src/host/meson.build`, `src/host/main.cpp` (prints version, exits)
- Create: `src/layer/meson.build`, `src/layer/layer_entry.cpp` (empty `vkGetInstanceProcAddr` returning nullptr for now)
- Create: `tests/meson.build`, `tests/ipc/test_paths.cpp`

**Acceptance Criteria:**
- [ ] `meson setup build -Dbuild_tests=true` succeeds and resolves the imgui + nlohmann_json subprojects.
- [ ] `meson compile -C build` builds `choir`, `libchoir_overlay.so`, `libchoir_ipc.a`.
- [ ] `meson test -C build` runs and passes the `paths` test.
- [ ] `./build/src/host/choir --version` prints `choir <version>`.

**Verify:** `meson setup build -Dbuild_tests=true && meson compile -C build && meson test -C build` → all tests OK; `./build/src/host/choir --version` → `choir 0.0.1`.

**Steps:**

- [ ] **Step 1: Write the failing test** for the one piece of real logic in this task — XDG path resolution.

```cpp
// tests/ipc/test_paths.cpp
#include "ipc/paths.hpp"
#include <cassert>
#include <cstdlib>
#include <string>
int main() {
    setenv("XDG_RUNTIME_DIR", "/run/user/test", 1);
    assert(choir::runtime_socket_path() == "/run/user/test/choir.sock");
    setenv("XDG_CACHE_HOME", "/tmp/c", 1);
    assert(choir::avatar_cache_dir() == "/tmp/c/choir/avatars");
    // Fallback: no XDG_CACHE_HOME -> $HOME/.cache
    unsetenv("XDG_CACHE_HOME"); setenv("HOME", "/home/u", 1);
    assert(choir::avatar_cache_dir() == "/home/u/.cache/choir/avatars");
    return 0;
}
```

- [ ] **Step 2: Run, expect FAIL** (paths.cpp not implemented): `meson test -C build paths`.

- [ ] **Step 3: Implement `paths`**.

```cpp
// src/ipc/paths.hpp
#pragma once
#include <string>
namespace choir {
std::string runtime_dir();        // $XDG_RUNTIME_DIR or /tmp
std::string runtime_socket_path();// runtime_dir()/choir.sock
std::string cache_home();         // $XDG_CACHE_HOME or $HOME/.cache
std::string avatar_cache_dir();   // cache_home()/choir/avatars
std::string config_home();        // $XDG_CONFIG_HOME or $HOME/.config
std::string config_path();        // config_home()/choir/config.json
}
```
Implement in `paths.cpp` with `getenv` + documented fallbacks (runtime → `/tmp`; cache → `$HOME/.cache`; config → `$HOME/.config`).

- [ ] **Step 4: Meson wiring.** Top-level `meson.build`:
```meson
project('choir', 'cpp', version: '0.0.1', default_options: ['cpp_std=c++20', 'warning_level=2'])
imgui_dep = dependency('imgui', fallback: ['imgui', 'imgui_dep'])
json_dep  = dependency('nlohmann_json', fallback: ['nlohmann_json', 'nlohmann_json_dep'])
subdir('src/ipc')
subdir('src/host')
subdir('src/layer')
if get_option('build_tests')
  subdir('tests')
endif
```
`meson_options.txt`: `option('build_tests', type: 'boolean', value: false)`.
Add `subprojects/imgui.wrap` and `subprojects/nlohmann_json.wrap` from wrapdb (`meson wrap install imgui`, `meson wrap install nlohmann_json`). `src/ipc/meson.build` builds a static lib from `paths.cpp` (+ later sources), `include_directories('..')` so headers resolve as `ipc/...`. `src/host/meson.build` builds `choir` from `main.cpp` linking `libchoir_ipc`. `src/layer/meson.build` builds `shared_library('choir_overlay', 'layer_entry.cpp', name_prefix:'lib')`. `tests/meson.build` registers `test('paths', executable('t_paths','ipc/test_paths.cpp', link_with: libchoir_ipc, include_directories: ipc_inc))`.

- [ ] **Step 5: Run, expect PASS**, then `./build/src/host/choir --version`.

- [ ] **Step 6: Commit** `feat: project scaffold + meson build + XDG paths`.

---

### Task 2: `libchoir_ipc` — wire contract (state, protocol, framing, avatar file)

**Goal:** Implement the shared types and (de)serialization that both host and layer depend on, with exhaustive round-trip tests.

**Files:**
- Create: `src/ipc/state.hpp` (as in "The wire contract" above), `src/ipc/state.cpp`
- Create: `src/ipc/protocol.hpp` (as above), `src/ipc/protocol.cpp`
- Create: `src/ipc/framing.hpp`, `src/ipc/framing.cpp`
- Create: `src/ipc/avatar_file.hpp` (as above), `src/ipc/avatar_file.cpp`
- Test: `tests/ipc/test_state.cpp`, `tests/ipc/test_protocol.cpp`, `tests/ipc/test_framing.cpp`, `tests/ipc/test_avatar_file.cpp`
- Modify: `src/ipc/meson.build` (add new sources + tests)

**Acceptance Criteria:**
- [ ] `Snapshot` → JSON → `Snapshot` round-trips losslessly (all fields incl. nested participants/notifications/config/enum).
- [ ] `encode_frame` then `try_decode_frame` recovers the same `(type,payload)`; `try_decode_frame` returns 0 on a partial buffer and handles two concatenated frames.
- [ ] `write_avatar_rgba`/`read_avatar_rgba` round-trip a 2×2 RGBA image, and `read_avatar_rgba` rejects a wrong-magic file.
- [ ] `framing` read/write works over a `socketpair(2)` fd including short reads.

**Verify:** `meson test -C build state protocol framing avatar_file` → 4/4 PASS.

**Steps:**

- [ ] **Step 1: Failing tests.** Write `test_state.cpp` (build a `Snapshot` with 2 participants — one speaking+self_mute — one notification, non-default config; serialize; deserialize; assert field-by-field equality and `revision` preserved). Write `test_protocol.cpp` (encode `Snapshot` payload `"{\"x\":1}"`; decode; assert; then feed first 3 bytes only → expect consume==0; then full → consume==frame length; then two frames concatenated → decode both). Write `test_framing.cpp` (`socketpair`, `write_frame` a payload on one fd, `read_frame` on the other in a loop tolerating EAGAIN). Write `test_avatar_file.cpp` (2×2 RGBA round-trip + corrupt-magic rejection).

- [ ] **Step 2: Run, expect FAIL.**

- [ ] **Step 3: Implement.**
  - `state.cpp`: nlohmann `to_json/from_json` for each struct + `Anchor` (as int), then `to_json_str`/`from_json_str` wrappers (compact dump; `from_json_str` wraps `json::parse` in try/catch → false).
  - `protocol.cpp`: `encode_frame` writes `uint32 LE (1+payload.size())`, then the type byte, then payload. `try_decode_frame`: if `len<4` return 0; read `total`; if `len < 4+total` return 0; else set `frame.type`, `frame.payload = bytes[5 .. 4+total]`, return `4+total`.
  - `framing.hpp/.cpp`: free functions `bool write_frame(int fd, MsgType, const std::string& payload)` (encode then write-all, retry on EINTR/EAGAIN for blocking fds) and a `FrameReader` class holding a `std::vector<uint8_t> buf` with `bool feed(int fd)` (append available bytes) + `bool next(DecodedFrame&)` (pop one complete frame using `try_decode_frame`, erase consumed bytes). This decouples byte I/O from frame boundaries — the layer's non-blocking reader reuses it.
  - `avatar_file.cpp`: write magic `CHAV` + dims + pixels; read validates magic+size.

- [ ] **Step 4: Run, expect PASS (4/4).**

- [ ] **Step 5: Commit** `feat(ipc): shared state, protocol framing, avatar cache format`.

---

### Task 3: Discord IPC transport (connect + op/len framing)

**Goal:** Connect to the Discord desktop client's local RPC socket and exchange opcode-framed JSON messages, verified against an in-test mock server.

**Files:**
- Create: `src/host/discord/ipc_transport.hpp`, `src/host/discord/ipc_transport.cpp`
- Test: `tests/host/test_ipc_transport.cpp`
- Modify: `src/host/meson.build`, `tests/meson.build`

**Acceptance Criteria:**
- [ ] `connect()` probes `$XDG_RUNTIME_DIR/discord-ipc-0..9` and connects to the first that accepts.
- [ ] Sends opcode `0` HANDSHAKE and op `1` FRAME with little-endian `[op][len]` headers; receives + reassembles op-framed responses.
- [ ] Against a mock server bound to `discord-ipc-0` that echoes a `READY`, the transport surfaces the decoded `{op, json}` to a callback.
- [ ] Responds to op `3` PING with op `4` PONG automatically.

**Verify:** `meson test -C build ipc_transport` → PASS.

**Steps:**

- [ ] **Step 1: Failing test.** `test_ipc_transport.cpp`: set `XDG_RUNTIME_DIR` to a temp dir; create a listening unix socket at `<tmp>/discord-ipc-0` on a background thread; the mock reads the handshake frame, replies with op `1` + `{"cmd":"DISPATCH","evt":"READY","data":{}}`. Drive `IpcTransport` (its own thread or a `poll` loop), assert the READY callback fires with the parsed JSON.

- [ ] **Step 2: Run, expect FAIL.**

- [ ] **Step 3: Implement** `IpcTransport`:
```cpp
// src/host/discord/ipc_transport.hpp
namespace choir {
class IpcTransport {
public:
    using OnMessage = std::function<void(int op, const nlohmann::json&)>;
    using OnClosed  = std::function<void()>;
    bool connect();                                   // probe -0..-9
    void set_handlers(OnMessage, OnClosed);
    bool send(int op, const nlohmann::json& payload); // little-endian [op][len]+json
    void poll();                                      // read available; dispatch; auto-PONG
    void close();
    int  fd() const { return fd_; }
private:
    int fd_ = -1;
    std::vector<uint8_t> rbuf_;
    OnMessage on_msg_; OnClosed on_closed_;
};
}
```
Framing: 8-byte header = two `int32` little-endian (`op`, `len`), then `len` bytes of JSON. `send` builds that. `poll` appends to `rbuf_`, peels complete messages, auto-replies to op `3` with op `4`, forwards others to `on_msg_`. The Qt host will pump `poll()` via a `QSocketNotifier` on `fd()`; the test pumps it on a thread.

- [ ] **Step 4: Run, expect PASS.**

- [ ] **Step 5: Commit** `feat(discord): local IPC transport with opcode framing`.

---

### Task 4: RPC message builders + event parsers

**Goal:** Pure functions that build the RPC command JSON (AUTHORIZE/AUTHENTICATE/SUBSCRIBE) and parse the inbound events into typed structs, fully unit-tested without any socket.

**Files:**
- Create: `src/host/discord/rpc_messages.hpp`, `src/host/discord/rpc_messages.cpp`
- Test: `tests/host/test_rpc_messages.cpp`
- Modify: `src/host/meson.build`, `tests/meson.build`

**Acceptance Criteria:**
- [ ] `build_authorize(scopes, client_id)` produces `{cmd:"AUTHORIZE", args:{client_id, scopes, prompt:"none"}, nonce}` with a unique nonce.
- [ ] `build_authenticate(token)`, `build_subscribe(evt, args)` match the RPC schema.
- [ ] Parsers turn real-shape `VOICE_STATE_CREATE/UPDATE/DELETE`, `SPEAKING_START/STOP`, `VOICE_CHANNEL_SELECT`, `NOTIFICATION_CREATE` JSON (captured fixtures) into typed events with correct `mute/deaf/self_mute/self_deaf/user_id/nick/avatar`.

**Verify:** `meson test -C build rpc_messages` → PASS.

**Steps:**

- [ ] **Step 1: Failing test** using inline JSON fixtures shaped like the documented RPC payloads (a `VOICE_STATE_UPDATE` with `voice_state.{mute,deaf,self_mute,self_deaf}`, `user.{id,username,avatar}`, `nick`; a `SPEAKING_START` with `{user_id, channel_id}`; a `VOICE_CHANNEL_SELECT` with `{channel_id}` and null). Assert parsed structs.

- [ ] **Step 2: Run, expect FAIL.**

- [ ] **Step 3: Implement** builders + a `parse_event(const json&) -> std::optional<RpcEvent>` where:
```cpp
struct VoiceState { std::string user_id, nick, avatar_hash; bool mute,deaf,self_mute,self_deaf; };
struct RpcEvent {
    enum Kind { VoiceCreate, VoiceUpdate, VoiceDelete, SpeakingStart, SpeakingStop,
                ChannelSelect, Notification } kind;
    std::string channel_id;           // for ChannelSelect / speaking
    VoiceState voice;                  // for Voice*
    std::string user_id;               // for Speaking*
    choir::Notification notif;         // for Notification
};
```
Map Discord's `user.avatar` + `user.id` into an `avatar_hash` (we use Discord's avatar hash directly; the cache builds the CDN URL).

- [ ] **Step 4: Run, expect PASS.**

- [ ] **Step 5: Commit** `feat(discord): RPC command builders + event parsers`.

---

### Task 5: OAuth token exchange (Streamkit + own-app modes)

**Goal:** Given an authorization `code` from AUTHORIZE, obtain an access token — via the Streamkit hosted endpoint (default) or the standard Discord token endpoint (own-app) — behind a mockable HTTP interface.

**Files:**
- Create: `src/host/discord/oauth.hpp`, `src/host/discord/oauth.cpp`
- Test: `tests/host/test_oauth.cpp`
- Modify: `src/host/meson.build`, `tests/meson.build`

**Acceptance Criteria:**
- [ ] An `HttpPost` interface is injectable; tests pass a fake that returns canned JSON.
- [ ] Streamkit mode POSTs the `code` to the Streamkit token URL and returns `access_token` from the response.
- [ ] Own-app mode POSTs `client_id`/`client_secret`/`grant_type=authorization_code`/`code` to `https://discord.com/api/oauth2/token` and returns the token (+ refresh token).
- [ ] A non-200 / malformed response yields a typed error, not a crash.

**Verify:** `meson test -C build oauth` → PASS.

**Steps:**

- [ ] **Step 1: Failing test** with a `FakeHttp` returning `{"access_token":"abc","refresh_token":"r"}` for the expected URL/body; assert `exchange_code(...)` returns `abc`. Add a 401 case → expect `TokenError`.

- [ ] **Step 2: Run, expect FAIL.**

- [ ] **Step 3: Implement.**
```cpp
struct HttpResponse { int status; std::string body; };
struct HttpPost { virtual HttpResponse post(const std::string& url,
                   const std::vector<std::pair<std::string,std::string>>& form,
                   const std::vector<std::pair<std::string,std::string>>& headers) = 0; };
enum class AuthMode { Streamkit, OwnApp };
struct TokenResult { bool ok; std::string access_token, refresh_token, error; };
TokenResult exchange_code(HttpPost&, AuthMode, const std::string& code,
                          const std::string& client_id, const std::string& client_secret);
```
The production `HttpPost` is implemented in Task 11 with `QNetworkAccessManager` (kept out of this task so the logic stays Qt-free and unit-testable). **Note:** the exact Streamkit token URL/params are confirmed by Gate 7 (M0) — leave a single `constexpr` for the URL so the spike can correct it in one place.

- [ ] **Step 4: Run, expect PASS.**

- [ ] **Step 5: Commit** `feat(discord): OAuth code→token exchange (streamkit/own-app)`.

---

### Task 6: RPC client state machine

**Goal:** Orchestrate transport + messages + oauth into a connection FSM (handshake → authorize → token → authenticate → subscribe) that emits `RpcEvent`s and reconnects on drop — tested end-to-end against a scripted mock RPC server.

**Files:**
- Create: `src/host/discord/rpc_client.hpp`, `src/host/discord/rpc_client.cpp`
- Test: `tests/host/test_rpc_client.cpp`
- Modify: `src/host/meson.build`, `tests/meson.build`

**Acceptance Criteria:**
- [ ] Drives: HANDSHAKE → on READY send AUTHORIZE → on AUTHORIZE response exchange code (via injected `HttpPost`) → AUTHENTICATE → on success SUBSCRIBE to `VOICE_CHANNEL_SELECT` + `NOTIFICATION_CREATE`; on `VOICE_CHANNEL_SELECT` (re)subscribe the channel-scoped voice/speaking events for that `channel_id`.
- [ ] Emits parsed `RpcEvent`s to a callback; exposes a `ConnectionState` enum (Disconnected/Connecting/Authorizing/Ready/InChannel).
- [ ] On socket close, transitions to Disconnected and schedules a reconnect (verified by a state callback, no real timer needed in tests — inject a tick).

**Verify:** `meson test -C build rpc_client` → PASS.

**Steps:**

- [ ] **Step 1: Failing test.** Mock RPC server (unix socket, op-framed) scripted to: accept handshake → send READY → expect AUTHORIZE, reply with `{cmd:AUTHORIZE,data:{code:"C"}}` → expect AUTHENTICATE, reply `{cmd:AUTHENTICATE,data:{...}}` → expect SUBSCRIBE(VOICE_CHANNEL_SELECT) → push a `VOICE_CHANNEL_SELECT{channel_id:"123"}` event → expect SUBSCRIBE(VOICE_STATE_CREATE, args.channel_id=="123") → push a `VOICE_STATE_CREATE`. Inject a `FakeHttp` returning token `abc`. Assert: final state == InChannel, and the voice event reached the callback.

- [ ] **Step 2: Run, expect FAIL.**

- [ ] **Step 3: Implement** the FSM driving `IpcTransport::send` with nonce/cmd correlation, calling `exchange_code` on the AUTHORIZE reply, and resubscribing voice events keyed on the active channel. Reconnect via an injected `now()`/`schedule()` seam.

- [ ] **Step 4: Run, expect PASS.**

- [ ] **Step 5: Commit** `feat(discord): RPC connection state machine`.

---

### Task 7 — 🚩 MANUAL GATE (M0 auth spike): prove the Streamkit RPC flow against a real Discord client

**Goal (USER-RUN):** Confirm end-to-end that the Streamkit `client_id` path actually yields a working token + voice subscriptions on a real, running Discord desktop client — the project's #1 risk. If it fails, switch the default `AuthMode` to OwnApp and document the registration steps.

> **USER-ORDERED GATE — NON-SKIPPABLE.** This task validates the load-bearing auth assumption from the spec. It MUST NOT be closed by walking around it, declaring it "verified inline", or substituting a cheaper check. Close only after the spike binary has printed live voice data from a real channel (or the OwnApp fallback has), with output captured. This step is inherently manual (real Discord account + OAuth consent click) and is run by the user, not a subagent.

**Files:**
- Create: `tests/harness/auth_spike.cpp` (a tiny `main()` that builds an `IpcTransport` + `RpcClient` with a real `QNetworkAccessManager`-backed `HttpPost`, default Streamkit mode, and prints every `RpcEvent`).
- Modify: `tests/meson.build` (build `auth_spike` only when `-Dbuild_tests=true`).

**Acceptance Criteria:**
- [ ] With the Discord desktop client running and the user in a voice channel, `./build/tests/harness/auth_spike` completes AUTHORIZE (a one-time Discord consent dialog appears), AUTHENTICATE, and prints the live participant list + `SPEAKING_START/STOP` as people talk.
- [ ] The working Streamkit token URL/params are confirmed and, if different from the placeholder in Task 5, corrected in the single `constexpr` (a follow-up code commit).
- [ ] If Streamkit fails: `README.md` gains an "own-app setup" section and the config default flips to `AuthMode::OwnApp` (a follow-up code commit), and the spike is re-run in own-app mode to satisfy the criteria above.

**Verify (manual):** Run `./build/tests/harness/auth_spike`; observe a consent prompt in Discord, then live voice/speaking lines in the terminal. Capture the terminal output into the task as evidence.

**Steps:**
- [ ] **Step 1:** Ensure the Discord desktop client (or Vesktop/Vencord) is running and you have joined a voice channel with at least one other person (or talk yourself to trigger `SPEAKING_START`).
- [ ] **Step 2:** Build with tests and run `./build/tests/harness/auth_spike`.
- [ ] **Step 3:** Approve the one-time Discord authorization prompt.
- [ ] **Step 4:** Confirm participants + speaking events print. Paste the output as the gate's evidence.
- [ ] **Step 5:** If it failed, flip to own-app mode (register an app at the Discord developer portal, paste `client_id`/`client_secret`, re-run) and document it. Commit any code/doc corrections: `fix(discord): confirm auth flow from M0 spike`.

```json:metadata
{"userGate": true, "tags": ["user-gate","manual","milestone-m0"], "gateScope": "one", "verifyCommand": "./build/tests/harness/auth_spike", "acceptanceCriteria": ["auth_spike completes AUTHORIZE+AUTHENTICATE against a real Discord client and prints live participants + SPEAKING_START/STOP", "Streamkit token URL/params confirmed (or own-app fallback documented and default flipped)"]}
```

---

### Task 8: Overlay state reducer

**Goal:** A pure component that folds `RpcEvent`s into a `choir::Snapshot` and fires a change callback, unit-tested with synthetic event sequences.

**Files:**
- Create: `src/host/model/overlay_state.hpp`, `src/host/model/overlay_state.cpp`
- Test: `tests/host/test_overlay_state.cpp`
- Modify: `src/host/meson.build`, `tests/meson.build`

**Acceptance Criteria:**
- [ ] `ChannelSelect{id}` sets `in_voice=true` + clears the roster; `ChannelSelect{null}` sets `in_voice=false`.
- [ ] `VoiceCreate/Update` upsert a `Participant` (by `user_id`) with correct mute/deaf flags; `VoiceDelete` removes it.
- [ ] `SpeakingStart/Stop` toggle `Participant.speaking` by `user_id` (no-op if unknown).
- [ ] `Notification` appends to `notifications` (capped, newest-first).
- [ ] `revision` increments on every applied change; the change callback fires once per applied event.

**Verify:** `meson test -C build overlay_state` → PASS.

**Steps:**
- [ ] **Step 1: Failing test** — feed a sequence (ChannelSelect→2×VoiceCreate→SpeakingStart→VoiceUpdate mute→SpeakingStop→VoiceDelete→ChannelSelect null) and assert the snapshot after each step.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** `OverlayState` holding the current `Snapshot` + a `user_id → index` map; `apply(const RpcEvent&)`; `set_config(AppearanceConfig)` (also bumps revision); `const Snapshot& current() const`; `std::function<void(const Snapshot&)> on_change`.
- [ ] **Step 4: Run, expect PASS.**
- [ ] **Step 5: Commit** `feat(model): overlay state reducer`.

---

### Task 9: Avatar cache

**Goal:** Resolve a participant's avatar to a cached `.rgba` file (fetch from Discord CDN, decode + resize via QImage, write via `write_avatar_rgba`), emitting an "avatar ready" signal; skip work when already cached.

**Files:**
- Create: `src/host/model/avatar_cache.hpp`, `src/host/model/avatar_cache.cpp`
- Test: `tests/host/test_avatar_cache.cpp`
- Modify: `src/host/meson.build`, `tests/meson.build`

**Acceptance Criteria:**
- [ ] `request(user_id, avatar_hash)` builds the CDN URL `https://cdn.discordapp.com/avatars/<id>/<hash>.png?size=64`; on first request fetches+decodes+writes `<avatar_cache_dir>/<avatar_hash>.rgba` (64×64) and emits `ready(hash, path, 64, 64)`.
- [ ] A second `request` for a cached hash emits `ready` immediately without an HTTP call.
- [ ] Decode/network failure is logged and does not crash; no partial file remains.

**Verify:** `meson test -C build avatar_cache` → PASS.

**Steps:**
- [ ] **Step 1: Failing test** — inject a fake fetcher returning the bytes of a known 4×4 PNG (embedded as a byte array); call `request("1","h")`; assert a `<dir>/h.rgba` exists, is readable via `read_avatar_rgba` at 64×64, and `ready` fired once; second `request("1","h")` fires `ready` with no fetch.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** with an injectable `ImageFetcher` interface (`std::optional<QByteArray> fetch(QUrl)`); decode with `QImage::loadFromData` → `scaled(64,64, KeepAspectRatioByExpanding)` → `convertToFormat(RGBA8888)` → `write_avatar_rgba`. Maintain an in-memory set of known hashes. The production fetcher (QNetworkAccessManager) is wired in Task 11.
- [ ] **Step 4: Run, expect PASS.**
- [ ] **Step 5: Commit** `feat(model): avatar cache (fetch/decode/store rgba)`.

---

### Task 10: Config + denylist

**Goal:** Load/save `config.json` (appearance, auth mode + client_id/secret, denylist) with `0600` perms for secret-bearing data, and a process-name denylist matcher with sane defaults.

**Files:**
- Create: `src/host/config/config.hpp/.cpp`, `src/host/config/denylist.hpp/.cpp`
- Test: `tests/host/test_config.cpp`, `tests/host/test_denylist.cpp`
- Modify: `src/host/meson.build`, `tests/meson.build`

**Acceptance Criteria:**
- [ ] `Config::load(path)` returns defaults when the file is absent; `save` then `load` round-trips appearance + auth + denylist.
- [ ] The saved config file mode is `0600`.
- [ ] `Denylist::blocks("steamwebhelper")` is true with defaults; `blocks("MyGame.exe")` is false; glob entries like `*launcher*` match case-insensitively.
- [ ] Default denylist includes at least: `Discord`, `steam`, `steamwebhelper`, `gamescope`, `obs`, `firefox`, `chrome`, `chromium`.

**Verify:** `meson test -C build config denylist` → PASS.

**Steps:**
- [ ] **Step 1: Failing tests** for round-trip + file mode (`stat` the file, assert `st_mode & 0777 == 0600`) and for denylist matching.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** `Config` (nlohmann round-trip of `AppearanceConfig` + `{auth_mode, client_id, client_secret, access_token, refresh_token}` + `std::vector<std::string> denylist`), writing with `open(O_CREAT|O_WRONLY|O_TRUNC, 0600)`. `Denylist::blocks(exe)` lowercases and `fnmatch`-matches each pattern against the basename.
- [ ] **Step 4: Run, expect PASS.**
- [ ] **Step 5: Commit** `feat(config): config.json + process denylist`.

---

### Task 11: State server + Qt host assembly (`choir` binary)

**Goal:** Implement the `QLocalServer`-based state server and wire RPC client + reducer + avatar cache + config + tray + settings into the runnable `choir` host that serves snapshots/avatars to layer clients and applies the denylist per connecting process.

**Files:**
- Create: `src/host/server/state_server.hpp/.cpp`
- Create: `src/host/ui/tray.hpp/.cpp`, `src/host/ui/settings_window.hpp/.cpp`
- Create: production `HttpPost` + `ImageFetcher` (QNetworkAccessManager) — `src/host/discord/qt_http.hpp/.cpp`
- Modify: `src/host/main.cpp` (assemble everything), `src/host/meson.build` (link Qt6 Widgets+Network), `tests/meson.build`
- Test: `tests/host/test_state_server.cpp`

**Acceptance Criteria:**
- [ ] `StateServer` listens on `runtime_socket_path()`, accepts `libchoir_ipc` clients, and on `Hello{pid,exe}` either (a) sends the current `Snapshot` (+ `AvatarReady` for known avatars) if not denylisted, or (b) sends `Disabled` if `Denylist::blocks(exe)`.
- [ ] `broadcast(snapshot)` pushes a `Snapshot` frame to every non-disabled client; `broadcast_avatar(...)` pushes `AvatarReady`.
- [ ] A test client (built on `libchoir_ipc` framing) connecting to the server and sending `Hello` for an allowed exe receives the seeded snapshot; for `steam` receives `Disabled`.
- [ ] `choir` launches: tray icon appears, "Authorize" triggers the RPC AUTHORIZE flow, settings persist via `Config`, and state changes propagate to connected layers. (Manual smoke.)

**Verify:** `meson test -C build state_server` → PASS; manual `./build/src/host/choir` shows a tray icon.

**Steps:**
- [ ] **Step 1: Failing test** for `StateServer`: start it on a temp `XDG_RUNTIME_DIR`, connect a raw client via `socket(AF_UNIX)` + `libchoir_ipc` framing, send `Hello{exe:"MyGame"}` → expect a `Snapshot` frame; new client `Hello{exe:"steam"}` → expect `Disabled`.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** `StateServer` with `QLocalServer` (call `setSocketOptions(QLocalServer::UserAccessOption)`, remove a stale socket file first). Per `QLocalSocket`, read frames with a `FrameReader`, handle `Hello`, track allowed clients, write frames. Implement `qt_http` (QNetworkAccessManager-backed `HttpPost` + `ImageFetcher`, synchronous via a local `QEventLoop` or async with callbacks). Wire `main.cpp`: construct `Config`, `RpcClient` (pump `IpcTransport::poll` via `QSocketNotifier`), `OverlayState` (`on_change` → `server.broadcast`), `AvatarCache` (`ready` → `server.broadcast_avatar`; request avatars when participants appear), `StateServer`, `Tray`, `SettingsWindow`. Tray menu: Open Settings / Reconnect / Quit.
- [ ] **Step 4: Run, expect PASS** (server test) and manually confirm the tray.
- [ ] **Step 5: Commit** `feat(host): state server + Qt tray/settings assembly`.

```json:metadata
{"files": ["src/host/server/state_server.cpp","src/host/ui/tray.cpp","src/host/ui/settings_window.cpp","src/host/discord/qt_http.cpp","src/host/main.cpp"], "verifyCommand": "meson test -C build state_server", "acceptanceCriteria": ["StateServer serves Snapshot to allowed clients and Disabled to denylisted exes","choir launches with a tray icon and persists settings"]}
```

---

### Task 12: Test harness — headless Vulkan present app + fake host

**Goal:** Build the infrastructure the layer tests need: a minimal Vulkan app that creates a swapchain on a headless surface and presents frames, plus a standalone `fake_host` that serves scripted snapshots/avatars on `choir.sock`.

**Files:**
- Create: `tests/harness/vk_min_present.cpp` (+ small helper header), `tests/harness/fake_host.cpp`
- Modify: `tests/meson.build`

**Acceptance Criteria:**
- [ ] `vk_min_present --frames N --readback out.ppm` creates instance+device, a `VK_EXT_headless_surface` swapchain (fallback: print "no headless surface" and exit 77 = skip), renders a solid clear color, presents N frames, and writes the last presented image as PPM.
- [ ] `fake_host` creates `choir.sock`, accepts a client, on `Hello` sends a scripted `Snapshot` (channel "Test", 3 participants — index 1 speaking, index 2 self_mute) + `AvatarReady` for 3 generated test avatars (solid-color `.rgba` written to the cache dir).
- [ ] Both build under `-Dbuild_tests=true`.

**Verify:** `./build/tests/harness/vk_min_present --frames 3 --readback /tmp/f.ppm` writes a PPM (or exits 77 where no Vulkan/headless surface); `./build/tests/harness/fake_host &` then a manual `nc`-style client gets a snapshot frame.

**Steps:**
- [ ] **Step 1:** Implement `vk_min_present`: standard Vulkan boilerplate; prefer `VK_EXT_headless_surface` (no compositor needed), else `vkGetPhysicalDeviceSurfaceSupport` via a hidden surface is out of scope → exit 77. Add `--readback` that copies the last swapchain image to a host-visible buffer and dumps PPM. This app is what the layer is loaded *into* for tests.
- [ ] **Step 2:** Implement `fake_host` using `libchoir_ipc` server-side framing over a plain `AF_UNIX` listener (no Qt — keeps the harness dependency-light), writing test avatars via `write_avatar_rgba`.
- [ ] **Step 3:** Wire both into `tests/meson.build`; mark `vk_min_present` runnable (its exit-77 path lets CI skip on no-GPU).
- [ ] **Step 4: Commit** `test(harness): headless vulkan present app + fake host`.

---

### Task 13: Vulkan layer skeleton (manifest, entry, dispatch, gating)

**Goal:** A loadable implicit layer that wires instance/device dispatch tables and passes everything through (drawing nothing yet), gated by `DISABLE_CHOIR_OVERLAY`, verified by loading it into `vk_min_present`.

**Files:**
- Create: `src/layer/layer_entry.cpp`, `src/layer/dispatch.hpp/.cpp`, `src/layer/gating.hpp/.cpp`
- Create: `src/layer/manifest/choir_overlay.json.in`
- Modify: `src/layer/meson.build` (configure the manifest with the install path + arch suffix)
- Test: `tests/harness/test_layer_loads.cpp` (or a meson test that runs `vk_min_present` with the layer forced on)

**Acceptance Criteria:**
- [ ] `vkNegotiateLoaderLayerInterfaceVersion` reports loader interface v2 and sets `pfnGetInstanceProcAddr`/`pfnGetDeviceProcAddr`.
- [ ] `vkCreateInstance`/`vkCreateDevice` build dispatch tables from `pNext` `VkLayer*CreateInfo` chains and call down correctly; destroy hooks tear down per-handle data.
- [ ] Running `vk_min_present` with `VK_INSTANCE_LAYERS=VK_LAYER_choir_overlay_x86_64` (manifest dir on `VK_LAYER_PATH`) succeeds and exits 0 (layer is a pass-through).
- [ ] With `DISABLE_CHOIR_OVERLAY=1`, the layer chains through without initializing overlay state.
- [ ] The generated manifest has the arch-suffixed `name`, a low `api_version` (e.g. `1.1.0`), `"disable_environment": {"DISABLE_CHOIR_OVERLAY": "1"}`.

**Verify:** `VK_LAYER_PATH=build/src/layer VK_INSTANCE_LAYERS=VK_LAYER_choir_overlay_x86_64 ./build/tests/harness/vk_min_present --frames 2` → exit 0 (or 77 if no Vulkan device).

**Steps:**
- [ ] **Step 1:** Implement the canonical layer boilerplate. Export:
```cpp
extern "C" VK_LAYER_EXPORT VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* p) {
    if (p->loaderLayerInterfaceVersion > 2) p->loaderLayerInterfaceVersion = 2;
    p->pfnGetInstanceProcAddr = choir_GetInstanceProcAddr;
    p->pfnGetDeviceProcAddr   = choir_GetDeviceProcAddr;
    return VK_SUCCESS;
}
```
`choir_GetInstanceProcAddr` returns our hooks for `vkCreateInstance/vkDestroyInstance/vkCreateDevice/vkGetInstanceProcAddr` (+ later swapchain entrypoints) and otherwise chains down. In `vkCreateInstance`, walk `pNext` for `VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO`, grab `pLayerInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr`, advance the chain (`pLayerInfo = pLayerInfo->pNext`), call down, then build the instance dispatch table. Mirror for `vkCreateDevice` with `LOADER_DEVICE_CREATE_INFO`. Store per-instance/per-device data in maps keyed by the dispatchable handle's loader key (`*(void**)instance`). (This is the standard Khronos sample wiring — preserve MIT notice if copied from MangoHud.)
- [ ] **Step 2:** `gating.cpp`: read `DISABLE_CHOIR_OVERLAY` once; expose `bool overlay_disabled()`.
- [ ] **Step 3:** `choir_overlay.json.in` configured by meson:
```json
{ "file_format_version": "1.2.0",
  "layer": { "name": "VK_LAYER_choir_overlay_@ARCH@",
    "type": "GLOBAL", "library_path": "@LIBPATH@",
    "api_version": "1.1.0", "implementation_version": "1",
    "description": "Choir — an overlay for Discord",
    "disable_environment": { "DISABLE_CHOIR_OVERLAY": "1" } } }
```
- [ ] **Step 4:** Add a meson test that runs `vk_min_present` with the layer forced on (treat exit 77 as skip).
- [ ] **Step 5: Commit** `feat(layer): implicit layer skeleton + dispatch + gating`.

---

### Task 14: Swapchain tracking + present hook (test rectangle)

**Goal:** Hook `vkCreateSwapchainKHR`/`vkDestroySwapchainKHR`/`vkQueuePresentKHR`; for each presented image, record+submit a tiny render that clears a small rectangle (proof the draw path works), handling recreate. No ImGui yet.

**Files:**
- Create: `src/layer/swapchain.hpp/.cpp`
- Modify: `src/layer/layer_entry.cpp`/`dispatch.cpp` (advertise the new device entrypoints)
- Test: extend `tests/harness/test_layer_golden.cpp` (golden: a known rectangle of color appears in the readback)

**Acceptance Criteria:**
- [ ] `vkCreateSwapchainKHR` stores per-swapchain `{format, extent, images[], image-views, a render pass, framebuffers, a command pool, per-image command buffers, a per-frame fence/semaphore}`.
- [ ] `vkQueuePresentKHR` reads `pPresentInfo->pImageIndices`, records a render pass that draws a solid rectangle into the corresponding framebuffer, submits it (waiting on the app's present-wait semaphores correctly, signaling a new semaphore that present then waits on), then calls the real `vkQueuePresentKHR`.
- [ ] Swapchain recreate (old swapchain passed in) tears down and rebuilds cleanly; no validation errors.
- [ ] Golden test: `vk_min_present --readback` with the layer shows the rectangle at the expected pixels (tolerance for format).

**Verify:** `meson test -C build layer_golden` → PASS (or 77 skip without a device).

**Steps:**
- [ ] **Step 1: Failing golden test** asserting the readback PPM has the rectangle's color in its region and the clear color elsewhere.
- [ ] **Step 2:** Implement swapchain bookkeeping + the present-time secondary submit. Carefully chain semaphores: take `pPresentInfo->pWaitSemaphores`, make the overlay submit wait on them, signal `overlay_done`, and present waiting on `overlay_done`. Use one command buffer per swapchain image, reset per frame.
- [ ] **Step 3:** Handle `VkSwapchainCreateInfoKHR::oldSwapchain` and `vkDestroySwapchainKHR` (free framebuffers/views/pool/sync; idle the device first).
- [ ] **Step 4: Run golden test, expect PASS.**
- [ ] **Step 5: Commit** `feat(layer): swapchain hooks + present-time draw path`.

---

### Task 15: ImGui Vulkan backend in the layer

**Goal:** Replace the test rectangle with a Dear ImGui render: initialize `imgui_impl_vulkan` against the layer's device/queue/render pass, drive frames manually (no platform/input backend), and draw a placeholder ImGui window per present.

**Files:**
- Create: `src/layer/imgui_renderer.hpp/.cpp`
- Modify: `src/layer/swapchain.cpp` (call into the renderer at present), `src/layer/meson.build` (link `imgui_dep`)
- Test: extend golden test (a known ImGui window/text region appears)

**Acceptance Criteria:**
- [ ] ImGui is initialized once per device with a descriptor pool, the swapchain render pass, image count, and MSAA=1; recreated on swapchain rebuild.
- [ ] Each present: `ImGui_ImplVulkan_NewFrame` → set `io.DisplaySize` to the swapchain extent → `ImGui::NewFrame` → draw a placeholder window → `ImGui::Render` → `ImGui_ImplVulkan_RenderDrawData` into the present command buffer.
- [ ] No input/platform backend is used (`io.DisplaySize` set manually; no `ImGui_ImplXxx_NewFrame` for a platform).
- [ ] Golden test: the ImGui window's frame/background is present in the readback.

**Verify:** `meson test -C build layer_golden` → PASS (or 77).

**Steps:**
- [ ] **Step 1: Update golden test** to expect the ImGui placeholder.
- [ ] **Step 2:** Implement `ImguiRenderer` (`init(device,phys,queue,queueFamily,renderPass,imageCount)`, `begin_frame(extent)`, `end_frame(cmdBuf)`, `shutdown()`). Build a dedicated descriptor pool sized for ImGui. Upload fonts once.
- [ ] **Step 3:** Call it from the present hook inside the render pass instead of the rectangle.
- [ ] **Step 4: Run golden test, expect PASS.**
- [ ] **Step 5: Commit** `feat(layer): Dear ImGui Vulkan backend integration`.

---

### Task 16: Layer state client + avatar textures

**Goal:** Connect the layer to `choir.sock` on a background thread, maintain a lock-free double-buffered `Snapshot`, and turn `AvatarReady` files into Vulkan textures (`ImTextureID`) created on the render thread.

**Files:**
- Create: `src/layer/state_client.hpp/.cpp`, `src/layer/avatar_textures.hpp/.cpp`
- Modify: `src/layer/swapchain.cpp` (drain the avatar-load queue at present; read the latest snapshot)
- Test: `tests/harness/test_layer_state.cpp` (run `fake_host`, load the layer into `vk_min_present`, assert the layer received the 3-participant snapshot — via a debug hook/log)

**Acceptance Criteria:**
- [ ] On device init the layer spawns a client thread: `connect(runtime_socket_path())` → send `Hello{pid, exe}` → loop `FrameReader::feed/next` handling `Snapshot`/`AvatarReady`/`Disabled`/`Ping(→Pong)`; non-blocking, reconnect with backoff if the host is absent.
- [ ] The newest `Snapshot` is published via an atomic swap; the render thread reads it without locks; `Disabled` latches the overlay off for the process.
- [ ] `AvatarReady` enqueues a load; the render thread (with the device) reads the `.rgba`, uploads a `VkImage`+view+sampler+descriptor, registers it with `ImGui_ImplVulkan_AddTexture`, and caches by hash; repeated hashes reuse the texture.
- [ ] If `choir.sock` is absent, the layer draws nothing and the game is unaffected (no crash, no busy-loop).

**Verify:** `meson test -C build layer_state` → PASS (or 77); without `fake_host`, `vk_min_present` + layer still exits 0.

**Steps:**
- [ ] **Step 1: Failing test** — start `fake_host`, run `vk_min_present` with the layer + an env like `CHOIR_DEBUG_DUMP=/tmp/snap.json` that makes the layer write the received snapshot once; assert the file shows 3 participants with index 1 speaking.
- [ ] **Step 2:** Implement `StateClient` (POSIX `AF_UNIX` non-blocking socket + `FrameReader`, background `std::thread`, `std::atomic<std::shared_ptr<const Snapshot>>` published state, a thread-safe avatar-load queue).
- [ ] **Step 3:** Implement `AvatarTextures` (render-thread-only Vulkan resource creation; map hash→`{VkImage,view,sampler,descriptor,ImTextureID}`).
- [ ] **Step 4: Run test, expect PASS.**
- [ ] **Step 5: Commit** `feat(layer): state client + avatar textures`.

---

### Task 17: Overlay UI — voice panel + notification toasts

**Goal:** Draw the real overlay from the current `Snapshot`: a voice participant panel (avatar, name, speaking ring, mute/deaf glyphs) and transient notification toasts, honoring the appearance config and `in_voice` visibility.

**Files:**
- Create: `src/layer/overlay_ui.hpp/.cpp`
- Modify: `src/layer/imgui_renderer.cpp`/`swapchain.cpp` (call `draw_overlay(snapshot, textures, extent)` between NewFrame/Render)
- Test: `tests/harness/test_layer_golden.cpp` (golden image: 3 participants, one with a speaking ring, one muted; compare to a committed reference within tolerance)

**Acceptance Criteria:**
- [ ] When `snapshot.in_voice == false`, nothing is drawn.
- [ ] Each participant row: circular avatar (its texture, or a placeholder if not yet ready), display name, a bright ring + brighten when `speaking`, mute/deaf icons when set; `show_all_members==false` shows only speakers.
- [ ] Toasts render at `toast_anchor`, stacked, and disappear after `toast_duration_ms` (using the present timestamp vs `Notification.created_ms`).
- [ ] Panel respects `anchor`, `scale`, `opacity`. The overlay window is non-interactive (`ImGuiWindowFlags_NoInputs|NoNav|NoDecoration|NoSavedSettings|NoFocusOnAppearing|NoBackground` as appropriate) so it is click-through by construction.
- [ ] Golden test matches the committed reference (driven by `fake_host`'s scripted snapshot) within per-pixel tolerance.

**Verify:** `meson test -C build layer_golden` → PASS (or 77).

**Steps:**
- [ ] **Step 1: Failing golden test** against a committed `tests/harness/golden/voice_panel.ppm` (generate once via the implemented path, eyeball it, commit it, then assert equality within tolerance).
- [ ] **Step 2:** Implement `draw_overlay`: compute the anchored position from `extent`+`anchor`+`scale`; a borderless no-input window; per participant use `ImGui::Image(textureId, ...)` with a foreground draw-list circle for the speaking ring; render mute/deaf as small vector glyphs (draw-list primitives — no extra font assets); toasts as separate stacked windows with fade.
- [ ] **Step 3:** Gate the whole draw on `in_voice`; apply `opacity` via `ImGui::PushStyleVar(ImGuiStyleVar_Alpha,...)`.
- [ ] **Step 4: Run golden test, expect PASS.**
- [ ] **Step 5: Commit** `feat(layer): voice panel + notification toast rendering`.

---

### Task 18: Robustness & failure isolation pass

**Goal:** Guarantee the layer can never crash or hang the host game: wrap every hooked entrypoint, self-disable on internal error, and verify graceful behavior under fault injection.

**Files:**
- Modify: `src/layer/swapchain.cpp`, `src/layer/dispatch.cpp`, `src/layer/state_client.cpp`, `src/layer/imgui_renderer.cpp`
- Test: `tests/harness/test_layer_faults.cpp`

**Acceptance Criteria:**
- [ ] Any exception/`VkResult` failure inside overlay rendering is caught; the layer marks that device "overlay-off" and forwards the real present so the frame still displays.
- [ ] Missing/half-written avatar files, malformed snapshots, and a host that disconnects mid-stream never crash the layer (fault-injection test drives each).
- [ ] No overlay work happens before the first valid `Snapshot` with `in_voice==true` (so denylisted/`Disabled` processes do zero ImGui/Vulkan overlay allocation).
- [ ] Running `vk_min_present` for 300 frames with the layer + `fake_host` toggling `in_voice` on/off shows no validation errors (run with `VK_LOADER_DEBUG=all` and the validation layer; assert clean).

**Verify:** `meson test -C build layer_faults` → PASS (or 77).

**Steps:**
- [ ] **Step 1: Failing fault tests** (feed a truncated avatar file; send a malformed `Snapshot` frame; kill `fake_host` mid-run; assert `vk_min_present` still exits 0 and frames keep presenting).
- [ ] **Step 2:** Add try/catch + `VkResult` checks around the present-time overlay block; a per-device `std::atomic<bool> overlay_failed` latch; lazy overlay init guarded on first `in_voice`.
- [ ] **Step 3: Run, expect PASS.**
- [ ] **Step 4: Commit** `feat(layer): failure isolation — never crash the game`.

---

### Task 19: Install / packaging (per-user, no root)

**Goal:** `meson install` lays down the layer `.so` + configured manifest into the per-user Vulkan implicit-layer dir, the `choir` binary into `~/.local/bin`, plus an optional autostart unit and a README quickstart.

**Files:**
- Modify: `src/layer/meson.build` (install `.so`; `configure_file` the manifest with the *installed* `library_path` + arch → install to `$XDG_DATA_HOME/vulkan/implicit_layer.d` via an install script, since meson installs to `prefix`)
- Create: `packaging/install-user.sh` (wraps `meson install --destdir` into `~/.local` + copies the manifest to `~/.local/share/vulkan/implicit_layer.d/`)
- Create: `packaging/choir.desktop` (XDG autostart for the host)
- Modify: `README.md` (build, install, enable, denylist, "not affiliated with Discord", Streamkit caveat)

**Acceptance Criteria:**
- [ ] After `packaging/install-user.sh`, `~/.local/share/vulkan/implicit_layer.d/choir_overlay.x86_64.json` exists with an absolute `library_path` to the installed `.so`.
- [ ] `vulkaninfo` (if present) lists `VK_LAYER_choir_overlay_x86_64` as an implicit layer.
- [ ] `~/.local/bin/choir` runs; `README` documents enabling the overlay globally + the denylist + `DISABLE_CHOIR_OVERLAY=1`.
- [ ] Uninstall instructions remove the manifest, `.so`, and binary.

**Verify:** `packaging/install-user.sh && test -f ~/.local/share/vulkan/implicit_layer.d/choir_overlay.x86_64.json && ~/.local/bin/choir --version`.

**Steps:**
- [ ] **Step 1:** Make the manifest install path correct (absolute `library_path`). Because the implicit-layer dir is fixed under `$XDG_DATA_HOME/vulkan/implicit_layer.d`, do the manifest copy in `install-user.sh` after `meson install` (templating the arch + installed `.so` path).
- [ ] **Step 2:** Write `install-user.sh`, `choir.desktop`, and the README sections.
- [ ] **Step 3:** Run the install, verify the manifest + binary, `git add` packaging + README.
- [ ] **Step 4: Commit** `feat(packaging): per-user install (layer manifest + host + autostart)`.

---

### Task 20 — 🚩 MANUAL GATE (final acceptance): overlay in a real game over real Discord voice

**Goal (USER-RUN):** Confirm the whole system works end-to-end: with `choir` running + authorized and the user in a Discord voice channel, launch a real Vulkan/DXVK game and see the voice panel render in-game, tracking who's speaking, with notification toasts.

> **USER-ORDERED GATE — NON-SKIPPABLE.** This is the product's acceptance test. It MUST NOT be closed by walking around it or substituting a cheaper check. Close only after the overlay has been observed rendering correctly inside a real game with live Discord voice, with a screenshot/recording captured. Inherently manual (real account + real game + human observation); run by the user.

**Files:** none (verification only; any fixes loop back to the relevant task).

**Acceptance Criteria:**
- [ ] `choir` is running, authorized (Gate 7 passed), and the per-user layer is installed (Task 19).
- [ ] Launch a real Vulkan or DXVK game (e.g. `vkcube` first as a smoke test, then a Proton/native title). With the user in a voice channel, the voice panel appears **inside the game**, shows the correct participants + avatars, highlights the active speaker in real time, and shows mute/deaf state.
- [ ] A Discord notification (DM/ping) shows a toast in-game and auto-expires.
- [ ] Leaving the voice channel hides the overlay; a denylisted app (e.g. add the game to the denylist) shows nothing.
- [ ] The game runs normally with the overlay (no crash, acceptable performance); `DISABLE_CHOIR_OVERLAY=1 %command%` disables it for that launch.

**Verify (manual):** Launch `vkcube` (or `VK_LAYER_PATH`-forced) with `choir` running + in voice → panel visible. Then a real game via Steam (`%command%`). Capture a screenshot as evidence.

**Steps:**
- [ ] **Step 1:** Start `choir`, authorize, join a voice channel.
- [ ] **Step 2:** Smoke test with `vkcube` (overlay should appear).
- [ ] **Step 3:** Launch a real Vulkan/DXVK game; confirm all acceptance criteria; capture a screenshot.
- [ ] **Step 4:** Toggle denylist + `DISABLE_CHOIR_OVERLAY=1` to confirm gating. Record results as the gate's evidence.

```json:metadata
{"userGate": true, "tags": ["user-gate","manual","acceptance"], "gateScope": "all", "verifyCommand": "manual: launch a real Vulkan/DXVK game with choir running + in voice", "acceptanceCriteria": ["voice panel renders inside a real game and tracks the active speaker live","notification toast appears in-game and expires","overlay hides when not in voice and on denylist/DISABLE_CHOIR_OVERLAY","game runs normally with the overlay"]}
```

---

## Dependency graph

```
1 → 2 → {3,8,9,10,12}
3 → 4 → 5 → 6 → 7(gate)
{6,8,9,10} → 11
2 → 12 → 13 → 14 → 15 → 16 → 17 → 18 → 19
{11,19,7} → 20(gate)
```

## Phasing note

Tasks 1–19 are automatable (build + unit/mock/golden tests; golden/headless tests skip with exit 77 where no Vulkan device is present). Tasks 7 and 20 are the two manual gates requiring a real Discord client / real game and are run by the user. Phase 2 (OpenGL shim, 32-bit, gamescope/`mangoapp`, GlobalShortcuts toggle) is out of scope for this plan.

## Self-review (completed during planning)

- **Spec coverage:** every spec section maps to a task — RPC/auth (3–7), state model (8), avatars (9), config/denylist (10), state server (11), Vulkan layer + ImGui (12–17), robustness (18), gating/visibility (10/11/17), install (19), testing strategy (all), risks: Streamkit (Gate 7), draw-order/fullscreen (documented limitations, exercised in 18/20).
- **Placeholder scan:** no TBD/TODO; the one deliberately-deferred value (Streamkit token URL) is isolated to a single `constexpr` resolved by Gate 7.
- **Type consistency:** all tasks use the locked `choir::{Participant,Notification,AppearanceConfig,Snapshot,Anchor}` + `MsgType` + avatar-file format from Task 2; `RpcEvent`/`VoiceState` defined in Task 4 and consumed by Tasks 6/8; `Hello/Snapshot/AvatarReady/Disabled` frames consistent across Tasks 11/16.
