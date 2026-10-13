/* vk_draw.c — Vulkan pipeline binding, push constants, draws and dispatches (DENG-323;
 * SPEC §12.1).
 *
 * Bindless set: every pipeline shares de__vk_device.pipeline_layout, so the one descriptor set
 * (set 0) stays bound across pipeline changes. set_pipeline binds it the first time a pipeline of
 * a bind point (graphics, compute) is bound in a command buffer, not per draw. cmd_begin
 * (vk_cmd.c) clears de__vk_cmd.set_bound.
 *
 * Push constants: one range of the layout, VK_SHADER_STAGE_ALL, from offset 0.
 *
 * Indirect: DrawIndexedIndirect records are 20 bytes (VkDrawIndexedIndirectCommand) and mesh
 * records 12 bytes (VkDrawMeshTasksIndirectCommandEXT), as de_gpu.h documents for every backend.
 * The count variants need drawIndirectCount (Vulkan 1.2 feature); a device without it reports
 * DE_ERR_UNSUPPORTED and records nothing. Without a count buffer, several draws need
 * multiDrawIndirect, else they are recorded one by one.
 *
 * Scissor: de_gpu coordinates are framebuffer pixels from the top-left, which is also Vulkan's
 * scissor space (the viewport flip does not apply to it); negative offsets are cut.
 */
#include "gpu/vulkan/vk_internal.h"

#include <string.h>

#define INDEXED_STRIDE 20u /* sizeof(VkDrawIndexedIndirectCommand) */
#define MESH_STRIDE    12u /* sizeof(VkDrawMeshTasksIndirectCommandEXT) */

static VkCommandBuffer cb_of(de_gpu_cmd *c) {
    return de__vk_cmd_of(c)->cb;
}

static VkBuffer buf_of(de__gpu_buffer *b) {
    return DE__GPU_PAYLOAD(de__vk_buffer, b)->buffer;
}

void de__vk_cmd_set_pipeline(de_gpu_cmd *c, de__gpu_pipeline *p) {
    de__vk_device *vk = de__vk(c->device);
    de__vk_cmd *vc = de__vk_cmd_of(c);
    const de__vk_pipeline *vp = DE__GPU_PAYLOAD(de__vk_pipeline, p);
    const u32 slot = vp->bind_point == VK_PIPELINE_BIND_POINT_COMPUTE ? 1u : 0u;
    vk->fn.vkCmdBindPipeline(vc->cb, vp->bind_point, vp->pipeline);
    if (!vc->set_bound[slot]) {
        vk->fn.vkCmdBindDescriptorSets(vc->cb, vp->bind_point, vk->pipeline_layout, 0, 1, &vk->set,
                                       0, NULL);
        vc->set_bound[slot] = true;
    }
}

void de__vk_cmd_push_constants(de_gpu_cmd *c, const void *data, u32 bytes) {
    de__vk_device *vk = de__vk(c->device);
    vk->fn.vkCmdPushConstants(cb_of(c), vk->pipeline_layout, VK_SHADER_STAGE_ALL, 0, bytes, data);
}

void de__vk_cmd_draw(de_gpu_cmd *c, u32 vertex_count, u32 instance_count, u32 first_vertex,
                     u32 first_instance) {
    de__vk(c->device)->fn.vkCmdDraw(cb_of(c), vertex_count, instance_count, first_vertex,
                                    first_instance);
}

void de__vk_cmd_draw_indexed(de_gpu_cmd *c, u32 index_count, u32 instance_count, u32 first_index,
                             i32 vertex_offset, u32 first_instance) {
    de__vk(c->device)->fn.vkCmdDrawIndexed(cb_of(c), index_count, instance_count, first_index,
                                           vertex_offset, first_instance);
}

void de__vk_cmd_set_index_buffer(de_gpu_cmd *c, de__gpu_buffer *b, u64 offset, bool index_u16) {
    de__vk(c->device)->fn.vkCmdBindIndexBuffer(
        cb_of(c), buf_of(b), offset, index_u16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

VkRect2D de__vk_scissor_rect(i32 x, i32 y, u32 w, u32 h) {
    /* Work in 64 bits: the far edges, then cut at 0 and at INT32_MAX (Vulkan's limit). */
    i64 x0 = x, y0 = y, x1 = (i64)x + w, y1 = (i64)y + h;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > INT32_MAX)
        x1 = INT32_MAX;
    if (y1 > INT32_MAX)
        y1 = INT32_MAX;
    VkRect2D r;
    r.offset.x = (i32)(x0 < INT32_MAX ? x0 : INT32_MAX);
    r.offset.y = (i32)(y0 < INT32_MAX ? y0 : INT32_MAX);
    r.extent.width = x1 > x0 ? (u32)(x1 - x0) : 0u;
    r.extent.height = y1 > y0 ? (u32)(y1 - y0) : 0u;
    return r;
}

void de__vk_cmd_scissor(de_gpu_cmd *c, i32 x, i32 y, u32 w, u32 h) {
    const VkRect2D r = de__vk_scissor_rect(x, y, w, h);
    de__vk(c->device)->fn.vkCmdSetScissor(cb_of(c), 0, 1, &r);
}

void de__vk_cmd_dispatch(de_gpu_cmd *c, u32 x, u32 y, u32 z) {
    de__vk(c->device)->fn.vkCmdDispatch(cb_of(c), x, y, z);
}

void de__vk_cmd_dispatch_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset) {
    de__vk(c->device)->fn.vkCmdDispatchIndirect(cb_of(c), buf_of(args), offset);
}

/* drawIndirectCount and multiDrawIndirect are enabled whenever the device has them
 * (vk_device.c wanted_features). */
static bool has_indirect_count(const de__vk_device *vk) {
    return vk->phys.f12.drawIndirectCount != VK_FALSE;
}
static bool has_multi_draw(const de__vk_device *vk) {
    return vk->phys.f.features.multiDrawIndirect != VK_FALSE;
}

void de__vk_cmd_draw_indexed_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset,
                                      de__gpu_buffer *count, u64 count_offset, u32 max_draws) {
    de__vk_device *vk = de__vk(c->device);
    if (count) {
        if (!has_indirect_count(vk)) {
            de__gpu_fail(c->device, DE_ERR_UNSUPPORTED,
                         "de_gpu_cmd_draw_indexed_indirect(\"%s\"): this Vulkan device has no "
                         "drawIndirectCount; nothing was recorded",
                         c->name);
            return;
        }
        vk->fn.vkCmdDrawIndexedIndirectCount(cb_of(c), buf_of(args), offset, buf_of(count),
                                             count_offset, max_draws, INDEXED_STRIDE);
        return;
    }
    if (max_draws <= 1 || has_multi_draw(vk)) {
        vk->fn.vkCmdDrawIndexedIndirect(cb_of(c), buf_of(args), offset, max_draws, INDEXED_STRIDE);
        return;
    }
    for (u32 i = 0; i < max_draws; ++i)
        vk->fn.vkCmdDrawIndexedIndirect(cb_of(c), buf_of(args), offset + (u64)i * INDEXED_STRIDE, 1,
                                        INDEXED_STRIDE);
}

void de__vk_cmd_dispatch_mesh(de_gpu_cmd *c, u32 x, u32 y, u32 z) {
    de__vk_device *vk = de__vk(c->device);
    if (vk->fn.vkCmdDrawMeshTasksEXT)
        vk->fn.vkCmdDrawMeshTasksEXT(cb_of(c), x, y, z);
}

void de__vk_cmd_dispatch_mesh_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset,
                                       de__gpu_buffer *count, u64 count_offset, u32 max) {
    de__vk_device *vk = de__vk(c->device);
    if (count) {
        if (!has_indirect_count(vk) || !vk->fn.vkCmdDrawMeshTasksIndirectCountEXT) {
            de__gpu_fail(c->device, DE_ERR_UNSUPPORTED,
                         "de_gpu_cmd_dispatch_mesh_indirect(\"%s\"): this Vulkan device has no "
                         "drawIndirectCount; nothing was recorded",
                         c->name);
            return;
        }
        vk->fn.vkCmdDrawMeshTasksIndirectCountEXT(cb_of(c), buf_of(args), offset, buf_of(count),
                                                  count_offset, max, MESH_STRIDE);
        return;
    }
    if (!vk->fn.vkCmdDrawMeshTasksIndirectEXT)
        return;
    if (max <= 1 || has_multi_draw(vk)) {
        vk->fn.vkCmdDrawMeshTasksIndirectEXT(cb_of(c), buf_of(args), offset, max, MESH_STRIDE);
        return;
    }
    for (u32 i = 0; i < max; ++i)
        vk->fn.vkCmdDrawMeshTasksIndirectEXT(cb_of(c), buf_of(args), offset + (u64)i * MESH_STRIDE,
                                             1, MESH_STRIDE);
}
