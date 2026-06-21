// Compile the vendored Dear ImGui OpenGL3 backend INTO libchoir_gl.so. Like the Vulkan
// unity TU, this #includes the .cpp directly so it sidesteps the meson source sandbox and
// is built with our flags. imgui_impl_opengl3 carries its own GL loader (it dlopens
// libGL), so we add no GL header/lib dependency here.
#include "imgui_impl_opengl3.cpp"
