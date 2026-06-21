#include "gl_avatar_textures.hpp"
#include "gl_api.hpp"
#include "ipc/avatar_file.hpp"   // read_avatar_rgba
#include <cstdint>
#include <vector>

namespace choir {
namespace {
// GL constants we need (avoid pulling <GL/gl.h>).
constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_RGBA = 0x1908, GL_RGBA8 = 0x8058;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401, GL_LINEAR = 0x2601;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803;
constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5, GL_TEXTURE_BINDING_2D = 0x8069;

using PFNGenTextures   = void (*)(GLsizei, GLuint*);
using PFNBindTexture   = void (*)(GLenum, GLuint);
using PFNTexParameteri = void (*)(GLenum, GLenum, GLint);
using PFNTexImage2D    = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
using PFNDeleteTextures= void (*)(GLsizei, const GLuint*);
using PFNPixelStorei   = void (*)(GLenum, GLint);
using PFNGetIntegerv   = void (*)(GLenum, GLint*);

struct GL {
    PFNGenTextures   GenTextures   = reinterpret_cast<PFNGenTextures>(glapi::get_proc("glGenTextures"));
    PFNBindTexture   BindTexture   = reinterpret_cast<PFNBindTexture>(glapi::get_proc("glBindTexture"));
    PFNTexParameteri TexParameteri = reinterpret_cast<PFNTexParameteri>(glapi::get_proc("glTexParameteri"));
    PFNTexImage2D    TexImage2D    = reinterpret_cast<PFNTexImage2D>(glapi::get_proc("glTexImage2D"));
    PFNDeleteTextures DeleteTextures = reinterpret_cast<PFNDeleteTextures>(glapi::get_proc("glDeleteTextures"));
    PFNPixelStorei   PixelStorei   = reinterpret_cast<PFNPixelStorei>(glapi::get_proc("glPixelStorei"));
    PFNGetIntegerv   GetIntegerv   = reinterpret_cast<PFNGetIntegerv>(glapi::get_proc("glGetIntegerv"));
    bool ok() const { return GenTextures && BindTexture && TexParameteri && TexImage2D
                          && DeleteTextures && PixelStorei && GetIntegerv; }
};
// Resolves all six GL entrypoints once, on first call — which must be from the render
// thread with the GL loader live (else they stay null and get_or_load returns Invalid).
const GL& gl() { static GL g; return g; }
}  // namespace

GlAvatarTextures::~GlAvatarTextures() { shutdown(); }

ImTextureID GlAvatarTextures::lookup(const std::string& hash) const {
    auto it = textures_.find(hash);
    return it == textures_.end() ? ImTextureID_Invalid
                                 : static_cast<ImTextureID>(static_cast<uintptr_t>(it->second));
}

ImTextureID GlAvatarTextures::get_or_load(const AvatarReq& req) {
    if (auto it = textures_.find(req.hash); it != textures_.end())
        return static_cast<ImTextureID>(static_cast<uintptr_t>(it->second));
    if (!gl().ok()) return ImTextureID_Invalid;
    uint32_t w = 0, h = 0; std::vector<uint8_t> rgba;
    if (!read_avatar_rgba(req.path, w, h, rgba) || w == 0 || h == 0
        || rgba.size() < size_t(w) * h * 4)
        return ImTextureID_Invalid;
    GLuint tex = 0;
    gl().GenTextures(1, &tex);
    if (tex == 0) return ImTextureID_Invalid;
    // Save the game's texture binding + unpack alignment: we run inside the swap hook, so
    // leaking either into the game's next frame can cause subtle rendering bugs.
    GLint prev_tex = 0, prev_align = 4;
    gl().GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    gl().GetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    gl().BindTexture(GL_TEXTURE_2D, tex);
    gl().PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl().TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(w), GLsizei(h), 0,
                    GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    gl().BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));   // restore game's binding
    gl().PixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
    textures_.emplace(req.hash, tex);
    return static_cast<ImTextureID>(static_cast<uintptr_t>(tex));
}

void GlAvatarTextures::shutdown() {
    if (textures_.empty()) return;
    if (gl().ok())
        for (auto& [hash, tex] : textures_) gl().DeleteTextures(1, &tex);
    textures_.clear();
}
}  // namespace choir
