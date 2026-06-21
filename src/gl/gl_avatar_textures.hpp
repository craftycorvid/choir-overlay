// GL avatar-texture cache: turns the host's AvatarReady RGBA8 files into GL textures
// usable as ImTextureID, keyed by avatar hash. RENDER-THREAD ONLY (touches GL). The GL
// analogue of src/layer/avatar_textures.* — far simpler (no staging buffer/fence/descr).
#pragma once
#include "iavatar_textures.hpp"
#include "gl_api.hpp"            // GLuint
#include <string>
#include <unordered_map>
namespace choir {
class GlAvatarTextures : public IAvatarTextures {
public:
    GlAvatarTextures() = default;
    ~GlAvatarTextures();
    GlAvatarTextures(const GlAvatarTextures&) = delete;
    GlAvatarTextures& operator=(const GlAvatarTextures&) = delete;
    ImTextureID get_or_load(const AvatarReq& req) override;
    ImTextureID lookup(const std::string& hash) const override;
    size_t size() const { return textures_.size(); }
    void shutdown();   // glDeleteTextures all; safe to call repeatedly
private:
    std::unordered_map<std::string, GLuint> textures_;   // hash -> GL texture name
};
}  // namespace choir
