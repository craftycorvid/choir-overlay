// Thin wrapper that builds Dear ImGui's Vulkan backend INTO the Choir layer with
// VK_NO_PROTOTYPES, so the backend's Vulkan calls go through function pointers we
// supply via ImGui_ImplVulkan_LoadFunctions (resolving through the layer's own
// dispatch chain) instead of the global loader trampolines, which reject the
// unwrapped handles a layer sees. See src/layer/imgui_renderer.cpp.
//
// We #include the backend .cpp from the imgui subproject's backends/ include dir (on
// the include path via meson). A compile-time #include sidesteps the meson source
// sandbox that forbids listing a nested-subproject file directly as a source, while
// still building exactly the vendored backend (no duplicate symbol vs libimgui.so,
// which is built core-only with vulkan=disabled).
#define VK_NO_PROTOTYPES
#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES

#include "imgui_impl_vulkan.cpp"

#include "imgui_vk_backend.hpp"
namespace choir {
VkPipeline create_hdr_pipeline(VkRenderPass render_pass, const uint32_t* frag_spv,
                               size_t frag_spv_size_bytes, int mode, float nits) {
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    if (!bd || render_pass == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    ImGui_ImplVulkan_InitInfo* v = &bd->VulkanInitInfo;
    const VkAllocationCallbacks* alloc = v->Allocator;
    ImGui_ImplVulkan_CreateShaderModules(v->Device, alloc);  // ensure bd->ShaderModuleVert exists
    if (!bd->ShaderModuleVert || !bd->PipelineLayout) return VK_NULL_HANDLE;

    VkShaderModule frag = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = frag_spv_size_bytes;
    smci.pCode = frag_spv;
    if (vkCreateShaderModule(v->Device, &smci, alloc, &frag) != VK_SUCCESS) return VK_NULL_HANDLE;

    struct Spec { int32_t mode; float nits; } spec{mode, nits};
    VkSpecializationMapEntry entries[2] = {
        {0, offsetof(Spec, mode), sizeof(int32_t)},
        {1, offsetof(Spec, nits), sizeof(float)},
    };
    VkSpecializationInfo si{2, entries, sizeof(Spec), &spec};

    VkPipelineShaderStageCreateInfo stage[2] = {};
    stage[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage[0].module = bd->ShaderModuleVert; stage[0].pName = "main";
    stage[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stage[1].module = frag; stage[1].pName = "main"; stage[1].pSpecializationInfo = &si;

    VkVertexInputBindingDescription bind{0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT,  offsetof(ImDrawVert, pos)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT,  offsetof(ImDrawVert, uv)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col)},
    };
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 3; vin.pVertexAttributeDescriptions = attr;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = (v->PipelineInfoMain.MSAASamples != 0) ? v->PipelineInfoMain.MSAASamples : VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.flags = bd->PipelineCreateFlags;
    ci.stageCount = 2; ci.pStages = stage;
    ci.pVertexInputState = &vin; ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp; ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms; ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb; ci.pDynamicState = &dy;
    ci.layout = bd->PipelineLayout; ci.renderPass = render_pass; ci.subpass = 0;

    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(v->Device, v->PipelineCache, 1, &ci, alloc, &pipe);
    vkDestroyShaderModule(v->Device, frag, alloc);
    return (r == VK_SUCCESS) ? pipe : VK_NULL_HANDLE;
}
}  // namespace choir
