#pragma once

// Avatar cache (Task 9).
//
// AvatarCache turns a participant's Discord identity (user_id + avatar_hash)
// into a cached <cache_dir>/<hash>.rgba file (64x64 RGBA8, via the shared
// choir::write_avatar_rgba format) and fires `ready` so the state server can
// broadcast an AvatarReady frame and the layer can load the texture.
//
// This component is deliberately Qt-free so it stays unit-testable without Qt
// (the same boundary used for networking/decoding in Tasks 3/5/6). The actual
// HTTP fetch + image decode + resize-to-64x64 is the Qt part and lives behind
// the injectable AvatarSource interface; the production implementation
// (QNetworkAccessManager + QImage) is wired in Task 11. Tests inject a fake.
//
// v1 scope: PARTICIPANT avatars only (user_id + hash -> CDN URL). Notification
// icons (full URLs from Task 4) are out of scope here.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace choir {

// A decoded, resized avatar image. rgba is row-major RGBA8, size == w*h*4.
struct DecodedAvatar {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

// The Qt-specific seam: fetch the image at `url`, decode it, and resize to
// exactly 64x64 RGBA8. Returns nullopt on any failure (network, decode, etc.).
struct AvatarSource {
    virtual ~AvatarSource() = default;
    virtual std::optional<DecodedAvatar> fetch(const std::string& url) = 0;
};

class AvatarCache {
public:
    // cache_dir is typically choir::avatar_cache_dir() at the call site.
    AvatarCache(AvatarSource& src, std::string cache_dir);

    // Ensure the avatar for (user_id, avatar_hash) is cached. On success (or if
    // already cached, in memory or on disk) fires `ready`. Empty avatar_hash is
    // a no-op. On fetch/write failure: logs to stderr, leaves no partial file,
    // does not fire `ready`, and does not mark the hash known (so a later retry
    // can still succeed).
    void request(const std::string& user_id, const std::string& avatar_hash);

    // Fired when an avatar is available on disk at `path` (w x h).
    std::function<void(const std::string& hash, const std::string& path,
                       uint32_t w, uint32_t h)>
        ready;

    // CDN URL for a participant avatar. Static + pure so it is independently
    // testable.
    static std::string cdn_url(const std::string& user_id,
                               const std::string& avatar_hash);

private:
    // Absolute path of the cache file for a given hash.
    std::string path_for(const std::string& avatar_hash) const;

    AvatarSource& src_;
    std::string dir_;
    std::unordered_set<std::string> known_;  // hashes cached this run
};

}  // namespace choir
