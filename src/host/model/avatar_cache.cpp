#include "model/avatar_cache.hpp"

#include "ipc/avatar_file.hpp"  // choir::write_avatar_rgba
#include "ipc/emoji.hpp"        // choir::emoji::split_runs / url_for

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace choir {

AvatarCache::AvatarCache(AvatarSource& src, std::string cache_dir)
    : src_(src), dir_(std::move(cache_dir)) {}

std::string AvatarCache::cdn_url(const std::string& user_id,
                                 const std::string& avatar_hash) {
    return "https://cdn.discordapp.com/avatars/" + user_id + "/" + avatar_hash +
           ".png?size=64";
}

std::string AvatarCache::path_for(const std::string& avatar_hash) const {
    return (fs::path(dir_) / (avatar_hash + ".rgba")).string();
}

void AvatarCache::request(const std::string& user_id,
                          const std::string& avatar_hash) {
    // No avatar set on the participant: nothing to do.
    if (avatar_hash.empty()) return;
    request_url(avatar_hash, cdn_url(user_id, avatar_hash));
}

void AvatarCache::request_url(const std::string& key, const std::string& url) {
    if (key.empty() || url.empty()) return;

    const std::string path = path_for(key);

    // Cache hit (in memory this run, or on disk from a prior run): fire ready
    // immediately, no fetch. The on-disk check also re-populates the in-memory
    // set so subsequent requests skip the filesystem stat.
    {
        std::error_code ec;
        if (known_.count(key) != 0 || fs::exists(path, ec)) {
            known_.insert(key);
            if (ready) ready(key, path, 64, 64);
            return;
        }
    }

    // Miss: fetch + decode + resize (the Qt seam).
    std::optional<DecodedAvatar> img = src_.fetch(url);
    if (!img) {
        std::fprintf(stderr, "choir: avatar fetch failed for hash %s\n",
                     key.c_str());
        return;  // not marked known -> retryable
    }

    // Ensure the cache directory exists before writing.
    {
        std::error_code ec;
        fs::create_directories(dir_, ec);
        if (ec) {
            std::fprintf(stderr,
                         "choir: failed to create avatar cache dir %s: %s\n",
                         dir_.c_str(), ec.message().c_str());
            return;
        }
    }

    if (!write_avatar_rgba(path, img->w, img->h, img->rgba.data())) {
        std::fprintf(stderr, "choir: failed to write avatar cache file %s\n",
                     path.c_str());
        // Leave no partial file behind; do not mark known (retryable).
        std::error_code ec;
        fs::remove(path, ec);
        return;
    }

    known_.insert(key);
    if (ready) ready(key, path, img->w, img->h);
}

void request_notification_emoji(AvatarCache& cache, const std::string& title,
                                const std::string& body) {
    // ponytail: cap guards the synchronous fetch loop (QtAvatarSource blocks up
    // to its timeout per miss) from stalling the Qt event loop on emoji spam;
    // go async if it ever matters.
    constexpr size_t kMaxEmojiPerNotification = 16;
    std::unordered_set<std::string> seen;
    for (const std::string* s : {&title, &body}) {
        for (const emoji::Run& r : emoji::split_runs(*s)) {
            if (r.key.empty() || !seen.insert(r.key).second) continue;
            if (seen.size() > kMaxEmojiPerNotification) return;
            cache.request_url(r.key, emoji::url_for(r.key));
        }
    }
}

}  // namespace choir
