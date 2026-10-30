// The settings menu's drawing (runtime/menu.c builds the frame): one pipeline in the window's render pass, after the
// game's image and Forge's overlay. No vertex buffers: the vertex shader reads the quads (MenuPrim, 6 vec4s) from a
// storage buffer, six vertices each; the fragment shader draws a rounded rectangle as a distance field (anti-aliased
// edge, border ring), a soft shadow, or a glyph's coverage from the atlas (a storage buffer of bytes, re-made when
// the atlas changes). Straight alpha blending into the UNORM window image (colours are the bytes shown).
#include <string.h>

#include "vk.h"
#include "../../menu.h"
#include "../../rt.h"

VkRenderPass vk_window_pass(void);  // (device.c)

static const char vs_src[] =
    "#version 450\n"
    "struct Prim { vec4 rect, radius, color, bcolor, params, uv; };\n"
    "layout(std430, set = 0, binding = 0) readonly buffer Prims { Prim prims[]; };\n"
    "layout(push_constant) uniform PC { vec2 view; uint atlas_w, atlas_h; } pc;\n"
    "layout(location = 0) out vec2 p;\n"
    "layout(location = 1) flat out uint idx;\n"
    "layout(location = 2) out vec2 uv;\n"
    "void main() {\n"
    "  uint i = uint(gl_VertexIndex) / 6u, k = uint(gl_VertexIndex) % 6u;\n"
    "  Prim q = prims[i];\n"
    "  vec2 c = vec2(k == 1u || k == 2u || k == 4u ? 1.0 : 0.0, k == 2u || k == 4u || k == 5u ? 1.0 : 0.0);\n"
    "  float grow = q.params.z == 1.0 ? 0.0 : max(q.params.y, 1.0);  // (room for the soft edge; glyphs: exact)\n"
    "  p = mix(q.rect.xy - grow, q.rect.zw + grow, c);\n"
    "  uv = mix(q.uv.xy, q.uv.zw, c);\n"
    "  idx = i;\n"
    "  gl_Position = vec4(p / pc.view * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char fs_src[] =
    "#version 450\n"
    "struct Prim { vec4 rect, radius, color, bcolor, params, uv; };\n"
    "layout(std430, set = 0, binding = 0) readonly buffer Prims { Prim prims[]; };\n"
    "layout(std430, set = 0, binding = 1) readonly buffer Atlas { uint px[]; };\n"
    "layout(push_constant) uniform PC { vec2 view; uint atlas_w, atlas_h; } pc;\n"
    "layout(location = 0) in vec2 p;\n"
    "layout(location = 1) flat in uint idx;\n"
    "layout(location = 2) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "float rrect(vec2 q, vec2 h, vec4 r) {  // r: top left, top right, bottom right, bottom left\n"
    "  float rad = q.x < 0.0 ? (q.y < 0.0 ? r.x : r.w) : (q.y < 0.0 ? r.y : r.z);\n"
    "  vec2 d = abs(q) - h + rad;\n"
    "  return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - rad;\n"
    "}\n"
    "void main() {\n"
    "  Prim q = prims[idx];\n"
    "  if (q.params.z == 1.0) {  // a glyph\n"
    "    uvec2 t = min(uvec2(uv), uvec2(pc.atlas_w - 1u, pc.atlas_h - 1u));\n"
    "    uint i = t.y * pc.atlas_w + t.x;\n"
    "    float a = float((px[i >> 2] >> ((i & 3u) * 8u)) & 255u) / 255.0;\n"
    "    o = vec4(q.color.rgb, q.color.a * pow(a, q.params.w));\n"
    "    return;\n"
    "  }\n"
    "  vec2 c = (q.rect.xy + q.rect.zw) * 0.5, h = (q.rect.zw - q.rect.xy) * 0.5;\n"
    "  float d = rrect(p - c, h, q.radius);\n"
    "  if (q.params.z == 2.0) {  // a soft shadow\n"
    "    float a = 1.0 - smoothstep(-q.params.y, q.params.y, d);\n"
    "    o = vec4(q.color.rgb, q.color.a * a * a);\n"
    "    return;\n"
    "  }\n"
    "  float cover = clamp(0.5 - d, 0.0, 1.0);\n"
    "  vec4 f = vec4(q.color.rgb * q.color.a, q.color.a);\n"
    "  if (q.params.x > 0.0) {  // the border ring over the fill\n"
    "    vec4 b = vec4(q.bcolor.rgb * q.bcolor.a, q.bcolor.a);\n"
    "    f = mix(b, f, clamp(0.5 - (d + q.params.x), 0.0, 1.0));\n"
    "  }\n"
    "  o = f.a > 0.0 ? vec4(f.rgb / f.a, f.a * cover) : vec4(0.0);\n"
    "}\n";

static VkDescriptorSetLayout set_layout;
static VkPipelineLayout layout;
static VkPipeline pipe;
static VkRenderPass made_for;
static VkBuffer atlas_buf;
static VmaAllocation atlas_mem;
static uint64_t atlas_gen;
static bool failed;

static VkPipeline pipeline(void) {
    VkRenderPass rp = vk_window_pass();
    if (pipe && made_for == rp) return pipe;
    if (!layout) {
        VkDescriptorSetLayoutBinding b[2] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &layout));
    }
    static VkShaderModule vs, fs;
    if (!vs) vs = vk_compile_glsl(vs_src, STAGE_VERTEX, "menu.vert");
    if (!fs) fs = vk_compile_glsl(fs_src, STAGE_FRAGMENT, "menu.frag");
    if (!vs || !fs) {
        LOG("[menu] the menu's shaders did not compile: no menu drawn\n");
        failed = true;
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs_ = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
                                       .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs_,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = layout, .renderPass = rp};
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &pipe));
    made_for = rp;
    return pipe;
}

// The atlas into a buffer of its own (the old one goes once the frames using it are done).
static void atlas_upload(const MenuFrame *f) {
    if (atlas_buf && atlas_gen == f->atlas_gen) return;
    if (atlas_buf) vk_defer_buffer(atlas_buf, atlas_mem);
    uint64_t size = (uint64_t)f->atlas_w * f->atlas_h;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &atlas_buf, &atlas_mem, &info));
    memcpy(info.pMappedData, f->atlas, size);
    vmaFlushAllocation(g_vk.vma, atlas_mem, 0, VK_WHOLE_SIZE);
    atlas_gen = f->atlas_gen;
}

void vk_menu_draw(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (failed) return;
    const MenuFrame *f = menu_frame_take();
    if (!f) return;
    VkPipeline p = f->nprims && f->atlas ? pipeline() : VK_NULL_HANDLE;
    if (p) {
        atlas_upload(f);
        uint64_t align = g_vk.props.limits.minStorageBufferOffsetAlignment;
        VkSpan prims = vk_transient_copy(f->prims, (uint64_t)f->nprims * sizeof(MenuPrim), align > 16 ? align : 16);
        VkDescriptorSet set = vk_alloc_set(set_layout);
        VkDescriptorBufferInfo bufs[2] = {{prims.buf, prims.off, (uint64_t)f->nprims * sizeof(MenuPrim)}, {atlas_buf, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet wr[2];
        for (int i = 0; i < 2; i++)
            wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = (uint32_t)i,
                                           .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bufs[i]};
        vkUpdateDescriptorSets(g_vk.dev, 2, wr, 0, NULL);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
        VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
        vkCmdSetViewport(cb, 0, 1, &vp);
        struct { float view[2]; uint32_t aw, ah; } pc = {{(float)w, (float)h}, f->atlas_w, f->atlas_h};
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        for (uint32_t i = 0; i < f->nbatches; i++) {
            const MenuBatch *b = &f->batches[i];
            int32_t x0 = b->clip[0] < 0 ? 0 : b->clip[0], y0 = b->clip[1] < 0 ? 0 : b->clip[1];
            int32_t x1 = b->clip[2] > (int32_t)w ? (int32_t)w : b->clip[2], y1 = b->clip[3] > (int32_t)h ? (int32_t)h : b->clip[3];
            if (!b->count || x1 <= x0 || y1 <= y0) continue;
            VkRect2D sc = {{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
            vkCmdSetScissor(cb, 0, 1, &sc);
            vkCmdDraw(cb, b->count * 6, 1, b->first * 6, 0);
        }
        VkRect2D full = {{0, 0}, {w, h}};
        vkCmdSetScissor(cb, 0, 1, &full);
    }
    menu_frame_done();
}
