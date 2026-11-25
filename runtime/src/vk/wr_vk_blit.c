/*
 * winrecomp runtime - present-pass blit: an offscreen target onto the
 * swapchain image, into a given rectangle (pillar/letterbox), with gamma.
 */
#include "wr_vk.h"
#include "wr.h"

#include <math.h>

static const uint32_t k_vs[] =
#include "shaders/blit_vert.spv.h"
;
static const uint32_t k_fs[] =
#include "shaders/blit_frag.spv.h"
;

static VkPipelineLayout g_layout;
static VkPipeline g_pipe;

static int blit_init(void)
{
    if (g_pipe)
        return 1;
    VkPushConstantRange pc = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    li.setLayoutCount = 1;
    li.pSetLayouts = &g_vk.tex_layout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_layout) != VK_SUCCESS)
        return 0;
    VkShaderModule vs = wr_vk_shader(k_vs, sizeof k_vs), fs = wr_vk_shader(k_fs, sizeof k_fs);
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = { 0 };
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &ds;
    ci.layout = g_layout;
    ci.renderPass = g_vk.present_clear;
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_pipe);
    vkDestroyShaderModule(g_vk.dev, vs, NULL);
    vkDestroyShaderModule(g_vk.dev, fs, NULL);
    if (r != VK_SUCCESS) {
        wr_log("vulkan: blit pipeline failed (%d)", (int)r);
        return 0;
    }
    return 1;
}

void wr_vk_blit(VkCommandBuffer cmd, wr_vk_target *t, int x, int y, int w, int h, float gamma)
{
    if (!t || !t->set || w <= 0 || h <= 0 || !blit_init())
        return;
    VkViewport v = { (float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f };
    VkRect2D sc = { { x > 0 ? x : 0, y > 0 ? y : 0 }, { (uint32_t)w, (uint32_t)h } };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe);
    vkCmdSetViewport(cmd, 0, 1, &v);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &t->set, 0, NULL);
    float g = gamma > 0.05f ? 1.0f / gamma : 1.0f;
    float pc[4] = { g, g, g, 1.0f };
    vkCmdPushConstants(cmd, g_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void wr_vk_fit_rect(uint32_t sw, uint32_t sh, uint32_t aw, uint32_t ah, int *x, int *y, int *w, int *h)
{
    /* largest aw:ah rectangle centred in sw x sh */
    double s = fmin((double)sw / aw, (double)sh / ah);
    *w = (int)(aw * s + 0.5);
    *h = (int)(ah * s + 0.5);
    *x = ((int)sw - *w) / 2;
    *y = ((int)sh - *h) / 2;
}
