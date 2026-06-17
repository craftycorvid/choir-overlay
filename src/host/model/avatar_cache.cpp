#include "model/avatar_cache.hpp"

#include "ipc/avatar_file.hpp"  // choir::write_avatar_rgba

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

    const std::string path = path_for(avatar_hash);

    // Cache hit (in memory this run, or on disk from a prior run): fire ready
    // immediately, no fetch. The on-disk check also re-populates the in-memory
    // set so subsequent requests skip the filesystem stat.
    {
        std::error_code ec;
        if (known_.count(avatar_hash) != 0 || fs::exists(path, ec)) {
            known_.insert(avatar_hash);
            if (ready) ready(avatar_hash, path, 64, 64);
            return;
        }
    }

    // Miss: fetch + decode + resize (the Qt seam).
    std::optional<DecodedAvatar> img = src_.fetch(cdn_url(user_id, avatar_hash));
    if (!img) {
        std::fprintf(stderr, "choir: avatar fetch failed for hash %s\n",
                     avatar_hash.c_str());
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

    known_.insert(avatar_hash);
    if (ready) ready(avatar_hash, path, img->w, img->h);
}

}  // namespace choir
