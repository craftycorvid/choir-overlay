#include "swapchain_color.hpp"
#include <cassert>
#include <cmath>
using namespace choir;
static bool approx(float a, float b, float t = 1e-3f) { return std::fabs(a - b) <= t; }
int main() {
    assert(transfer_function_for(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_PASS_THROUGH_EXT) == TransferFunction::ScRgb);
    assert(transfer_function_for(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) == TransferFunction::ScRgb);
    assert(transfer_function_for(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT) == TransferFunction::Pq);
    assert(transfer_function_for(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_HDR10_HLG_EXT) == TransferFunction::Hlg);
    assert(transfer_function_for(VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) == TransferFunction::Srgb);
    assert(transfer_function_for(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) == TransferFunction::None);
    assert(transfer_function_for(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_PASS_THROUGH_EXT) == TransferFunction::None);

    { float r=0,g=0,b=0; apply_transfer(TransferFunction::None,200,r,g,b); assert(r==0&&g==0&&b==0); }
    { float r=1,g=0.5f,b=0; apply_transfer(TransferFunction::Srgb,200,r,g,b); assert(approx(r,1)&&approx(g,0.214f,5e-3f)); }
    { float r=1,g=1,b=1; apply_transfer(TransferFunction::ScRgb,200,r,g,b); assert(approx(r,2.5f)&&approx(g,2.5f)); }
    { float r=0,g=0,b=0; apply_transfer(TransferFunction::Pq,200,r,g,b); assert(approx(r,0,5e-3f)); float w=1,x=1,y=1; apply_transfer(TransferFunction::Pq,200,w,x,y); assert(w>0.3f&&w<0.8f); }
    { float r=1,g=1,b=1; apply_transfer(TransferFunction::Hlg,200,r,g,b); assert(r>0.5f&&r<=1.0f); }
    return 0;
}
