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
