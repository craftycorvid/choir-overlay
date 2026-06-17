// Tests for the avatar cache (Task 9).
//
// AvatarCache is the Qt-free logic that turns a participant's (user_id,
// avatar_hash) into a cached <dir>/<hash>.rgba file (64x64 RGBA8) and fires
// `ready`. The Qt-specific fetch+decode+resize lives behind the injectable
// AvatarSource interface (production impl in Task 11); here a FakeAvatarSource
// drives the logic.
//
// Coverage:
//   - cdn_url() construction
//   - first request: fetch once, write file, fire ready once
//   - in-memory dedup: second request for same hash fires ready, no fetch
//   - disk-cache hit: a fresh cache over a dir with the file already present
//     fires ready without fetching
//   - fetch failure: no file, no ready, retryable (later success works)
//   - empty hash: no-op

#include "model/avatar_cache.hpp"

#include "ipc/avatar_file.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>  // getpid

using namespace choir;

namespace fs = std::filesystem;

// --- fake source -----------------------------------------------------------

// Records how many times fetch() was called and the last URL it saw. Returns a
// programmed result: a 64x64 solid-color DecodedAvatar, or nullopt to simulate
// network/decode failure.
struct FakeAvatarSource : AvatarSource {
    int calls = 0;
    std::string last_url;
    bool succeed = true;
    uint8_t fill = 0xAB;  // solid color channel value for the programmed image

    std::optional<DecodedAvatar> fetch(const std::string& url) override {
        ++calls;
        last_url = url;
        if (!succeed) return std::nullopt;
        DecodedAvatar a;
        a.w = 64;
        a.h = 64;
        a.rgba.assign(static_cast<size_t>(64) * 64 * 4, fill);
        return a;
    }
};

// --- helpers ---------------------------------------------------------------

// A unique temp cache dir under /tmp for this test run, cleaned up at start.
static std::string make_temp_dir() {
    auto base = fs::temp_directory_path() /
                ("choir_avatar_cache_test_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(base, ec);
    return base.string();
}

// --- tests -----------------------------------------------------------------

static void test_cdn_url() {
    assert(AvatarCache::cdn_url("123", "abc") ==
           "https://cdn.discordapp.com/avatars/123/abc.png?size=64");
}

static void test_first_request_fetches_writes_fires() {
    const std::string dir = make_temp_dir();
    FakeAvatarSource src;
    AvatarCache cache(src, dir);

    int ready_count = 0;
    std::string ready_hash, ready_path;
    uint32_t ready_w = 0, ready_h = 0;
    cache.ready = [&](const std::string& h, const std::string& p, uint32_t w,
                      uint32_t hh) {
        ++ready_count;
        ready_hash = h;
        ready_path = p;
        ready_w = w;
        ready_h = hh;
    };

    cache.request("1", "h");

    // Fetched exactly once, with the CDN URL.
    assert(src.calls == 1);
    assert(src.last_url == "https://cdn.discordapp.com/avatars/1/h.png?size=64");

    // ready fired exactly once with the right payload.
    assert(ready_count == 1);
    assert(ready_hash == "h");
    const std::string expected_path = (fs::path(dir) / "h.rgba").string();
    assert(ready_path == expected_path);
    assert(ready_w == 64);
    assert(ready_h == 64);

    // File on disk is readable via read_avatar_rgba at 64x64.
    assert(fs::exists(expected_path));
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    assert(read_avatar_rgba(expected_path, w, h, rgba));
    assert(w == 64 && h == 64);
    assert(rgba.size() == static_cast<size_t>(64) * 64 * 4);
    assert(rgba[0] == 0xAB);
}

static void test_in_memory_dedup() {
    const std::string dir = make_temp_dir();
    FakeAvatarSource src;
    AvatarCache cache(src, dir);

    int ready_count = 0;
    cache.ready = [&](const std::string&, const std::string&, uint32_t,
                      uint32_t) { ++ready_count; };

    cache.request("1", "h");
    assert(src.calls == 1);
    assert(ready_count == 1);

    // Second request for the same hash: ready fires again, but NO new fetch.
    cache.request("1", "h");
    assert(src.calls == 1);  // unchanged
    assert(ready_count == 2);
}

static void test_disk_cache_hit_fresh_instance() {
    const std::string dir = make_temp_dir();

    // First cache populates the file on disk.
    {
        FakeAvatarSource src;
        AvatarCache cache(src, dir);
        cache.request("1", "h");
        assert(src.calls == 1);
    }

    // A brand new AvatarCache over the same dir (empty in-memory set) must hit
    // the on-disk file and fire ready without fetching.
    FakeAvatarSource src2;
    AvatarCache cache2(src2, dir);
    int ready_count = 0;
    std::string ready_path;
    cache2.ready = [&](const std::string&, const std::string& p, uint32_t,
                       uint32_t) {
        ++ready_count;
        ready_path = p;
    };

    cache2.request("1", "h");
    assert(src2.calls == 0);  // disk hit -> no fetch
    assert(ready_count == 1);
    assert(ready_path == (fs::path(dir) / "h.rgba").string());
}

static void test_fetch_failure_no_file_retryable() {
    const std::string dir = make_temp_dir();
    FakeAvatarSource src;
    src.succeed = false;  // simulate fetch/decode failure
    AvatarCache cache(src, dir);

    int ready_count = 0;
    cache.ready = [&](const std::string&, const std::string&, uint32_t,
                      uint32_t) { ++ready_count; };

    cache.request("1", "h");
    assert(src.calls == 1);
    assert(ready_count == 0);  // no ready on failure

    const std::string path = (fs::path(dir) / "h.rgba").string();
    assert(!fs::exists(path));  // no partial file left behind

    // Retry must work: the failure did not mark the hash known.
    src.succeed = true;
    cache.request("1", "h");
    assert(src.calls == 2);  // fetched again
    assert(ready_count == 1);
    assert(fs::exists(path));
}

static void test_empty_hash_is_noop() {
    const std::string dir = make_temp_dir();
    FakeAvatarSource src;
    AvatarCache cache(src, dir);

    int ready_count = 0;
    cache.ready = [&](const std::string&, const std::string&, uint32_t,
                      uint32_t) { ++ready_count; };

    cache.request("1", "");
    assert(src.calls == 0);
    assert(ready_count == 0);
}

int main() {
    test_cdn_url();
    test_first_request_fetches_writes_fires();
    test_in_memory_dedup();
    test_disk_cache_hit_fresh_instance();
    test_fetch_failure_no_file_retryable();
    test_empty_hash_is_noop();
    return 0;
}
