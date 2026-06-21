# GL backend objects (shared by libchoir_gl.so and the GL golden test). The vendored
# imgui_impl_opengl3 brings its own GL loader (dlopens libGL), so we link only libdl.
gl_backend_lib = static_library('choir_imgui_gl_backend',
  files('imgui_impl_opengl3_unity.cpp'),
  include_directories: [imgui_backends_inc],
  dependencies: [imgui_dep, dependency('dl')],
  cpp_args: ['-Wno-missing-field-initializers', '-Wno-unused-parameter'],
)

choir_gl_objs = static_library('choir_gl_objs',
  files('gl_api.cpp', 'gl_renderer.cpp', 'gl_avatar_textures.cpp'),
  include_directories: [overlay_inc, ipc_inc, imgui_backends_inc],
  link_with: [gl_backend_lib, libchoir_overlay_shared],
  dependencies: [imgui_dep, json_dep, dependency('threads'), dependency('dl')],
  cpp_args: ['-Wno-missing-field-initializers'],
)

# libchoir_gl.so: the LD_PRELOAD GL interposer. Hidden visibility + a version script that
# exports ONLY the interposer hooks (so our static ImGui never interposes a game's ImGui),
# matching the Vulkan layer's hardening. Links the GL objects + shared overlay core. Resolves
# EGL/GLX at runtime via dlsym, so it links no windowing lib — only libdl.
gl_version_script = meson.current_source_dir() / 'choir_gl.version'
libchoir_gl = shared_library('choir_gl',
  files('interposer.cpp'),
  name_prefix: 'lib',
  include_directories: [overlay_inc, ipc_inc, imgui_backends_inc],
  link_with: [choir_gl_objs, libchoir_overlay_shared, libchoir_ipc],
  dependencies: [imgui_dep, json_dep, dependency('threads'), dependency('dl')],
  cpp_args: ['-Wno-missing-field-initializers'],
  gnu_symbol_visibility: 'hidden',
  link_args: [
    '-Wl,--version-script=@0@'.format(gl_version_script),
    '-Wl,--exclude-libs,ALL',
  ],
  link_depends: gl_version_script,
  install: true,
  install_dir: get_option('libdir') / 'choir',   # beside libchoir_overlay.so
)
