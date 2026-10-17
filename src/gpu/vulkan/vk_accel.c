#include "gpu/vulkan/vk_internal.h"
#include "gpu/gpu_accel_internal.h"
#include <string.h>
#include "gpu/vulkan/vk_accel_shader.h"

static VkBuildAccelerationStructureFlagsKHR flags(const de_gpu_accel_desc *x) {
    return (x->allow_update ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0) |
           (x->allow_compaction ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR : 0) |
           (x->prefer_fast_trace ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                                 : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR);
}
static VkDeviceAddress address(de_gpu_device *d, de_gpu_buffer h) {
    de__gpu_buffer *b = de__gpu_get_buffer(d, h);
    return b ? DE__GPU_PAYLOAD(de__vk_buffer, b)->address : 0;
}
static VkAccelerationStructureGeometryKHR geometry(de_gpu_device *d,
                                                   const de_gpu_blas_geometry *g) {
    VkAccelerationStructureGeometryKHR r = {0};
    r.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    r.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    r.flags = g->opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
    VkAccelerationStructureGeometryTrianglesDataKHR *t = &r.geometry.triangles;
    t->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    t->vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    t->vertexData.deviceAddress = address(d, g->vertices) + g->vertex_offset;
    t->vertexStride = g->vertex_stride;
    t->maxVertex = g->vertex_count - 1;
    t->indexType = !g->indices.id ? VK_INDEX_TYPE_NONE_KHR
                   : g->index_u16 ? VK_INDEX_TYPE_UINT16
                                  : VK_INDEX_TYPE_UINT32;
    t->indexData.deviceAddress = g->indices.id ? address(d, g->indices) + g->index_offset : 0;
    return r;
}
static VkAccelerationStructureBuildGeometryInfoKHR info(const de_gpu_accel_desc *x) {
    VkAccelerationStructureBuildGeometryInfoKHR r = {0};
    r.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    r.type = x->is_tlas ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
                        : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    r.flags = flags(x);
    r.geometryCount = x->is_tlas ? 1 : x->geometry_count;
    return r;
}
static VkPhysicalDeviceAccelerationStructurePropertiesKHR properties(de__vk_device *vk) {
    VkPhysicalDeviceAccelerationStructurePropertiesKHR a = {0};
    a.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 p = {0};
    p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p.pNext = &a;
    vk->fn.vkGetPhysicalDeviceProperties2(vk->phys.handle, &p);
    return a;
}

de_result de__vk_accel_sizes(de_gpu_device *d, const de_gpu_accel_desc *x, de_gpu_accel_sizes *s) {
    de__vk_device *vk = de__vk(d);
    if (!vk->on.accel || !vk->on.ray_query || !vk->fn.vkGetAccelerationStructureBuildSizesKHR)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "vulkan: KHR AS and ray query unavailable");
    VkPhysicalDeviceAccelerationStructurePropertiesKHR p = properties(vk);
    if (x->geometry_count > p.maxGeometryCount || x->max_instances > p.maxInstanceCount)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "vulkan: AS exceeds native geometry/instance limit");
    if (x->is_tlas &&
        (u64)x->max_instances * 64 > vk->phys.props.properties.limits.maxStorageBufferRange)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "vulkan: TLAS translation exceeds maxStorageBufferRange");
    const u32 n = x->is_tlas ? 1 : x->geometry_count;
    VkAccelerationStructureGeometryKHR *g =
        DE_ALLOC(d->alloc, (size_t)n * sizeof *g, 16, DE_MEM_RENDER);
    u32 *counts = DE_ALLOC(d->alloc, (size_t)n * sizeof *counts, 16, DE_MEM_RENDER);
    if (!g || !counts) {
        if (g)
            DE_FREE(d->alloc, g, (size_t)n * sizeof *g);
        if (counts)
            DE_FREE(d->alloc, counts, (size_t)n * sizeof *counts);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vulkan: AS size query");
    }
    u64 total = 0;
    for (u32 i = 0; i < n; ++i) {
        if (x->is_tlas) {
            memset(g, 0, sizeof *g);
            g->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            g->geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            g->geometry.instances.sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
            counts[i] = x->max_instances;
        } else {
            g[i] = geometry(d, &x->geometries[i]);
            counts[i] = (x->geometries[i].indices.id ? x->geometries[i].index_count
                                                     : x->geometries[i].vertex_count) /
                        3;
        }
        total += counts[i];
    }
    VkAccelerationStructureBuildGeometryInfoKHR b = info(x);
    b.pGeometries = g;
    VkAccelerationStructureBuildSizesInfoKHR sizes = {0};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    if (x->is_tlas || total <= p.maxPrimitiveCount)
        vk->fn.vkGetAccelerationStructureBuildSizesKHR(
            vk->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &b, counts, &sizes);
    DE_FREE(d->alloc, g, (size_t)n * sizeof *g);
    DE_FREE(d->alloc, counts, (size_t)n * sizeof *counts);
    if (!sizes.accelerationStructureSize)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "vulkan: invalid AS size or primitive limit");
    s->accel_bytes = sizes.accelerationStructureSize;
    s->build_scratch_bytes = sizes.buildScratchSize;
    s->update_scratch_bytes = sizes.updateScratchSize;
    return DE_OK;
}

void de__vk_accel_destroy(de_gpu_device *d, de__gpu_accel *a) {
    de__vk_device *vk = de__vk(d);
    de__vk_accel *v = DE__GPU_PAYLOAD(de__vk_accel, a);
    if (v->query)
        vk->fn.vkDestroyQueryPool(vk->device, v->query, NULL);
    if (v->handle)
        vk->fn.vkDestroyAccelerationStructureKHR(vk->device, v->handle, NULL);
    de__gpu_accel_buffer_free(d, v->storage);
    u32 n = a->is_tlas ? 1 : a->desc.geometry_count;
    if (v->geometry)
        DE_FREE(d->alloc, v->geometry, (size_t)n * sizeof *v->geometry);
    if (v->ranges)
        DE_FREE(d->alloc, v->ranges, (size_t)n * sizeof *v->ranges);
    if (v->counts)
        DE_FREE(d->alloc, v->counts, (size_t)n * sizeof *v->counts);
    memset(v, 0, sizeof *v);
}

de_result de__vk_accel_create(de_gpu_device *d, de__gpu_accel *a, const de_gpu_accel_desc *x) {
    de__vk_device *vk = de__vk(d);
    de__vk_accel *v = DE__GPU_PAYLOAD(de__vk_accel, a);
    v->scratch_alignment = properties(vk).minAccelerationStructureScratchOffsetAlignment;
    u32 n = x->is_tlas ? 1 : x->geometry_count;
    v->geometry = DE_ALLOC(d->alloc, (size_t)n * sizeof *v->geometry, 16, DE_MEM_RENDER);
    v->ranges = DE_ALLOC(d->alloc, (size_t)n * sizeof *v->ranges, 16, DE_MEM_RENDER);
    v->counts = DE_ALLOC(d->alloc, (size_t)n * sizeof *v->counts, 16, DE_MEM_RENDER);
    v->storage =
        de__gpu_accel_buffer(d, a->sizes.accel_bytes, DE_USAGE_ACCEL_STORAGE, DE_MEM_GPU_ONLY);
    if (!v->geometry || !v->ranges || !v->counts || !v->storage)
        goto fail;
    VkAccelerationStructureCreateInfoKHR ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    ci.buffer = DE__GPU_PAYLOAD(de__vk_buffer, v->storage)->buffer;
    ci.size = a->sizes.accel_bytes;
    ci.type = x->is_tlas ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
                         : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (vk->fn.vkCreateAccelerationStructureKHR(vk->device, &ci, NULL, &v->handle) != VK_SUCCESS)
        goto fail;
    if (x->allow_compaction) {
        VkQueryPoolCreateInfo qi = {0};
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        qi.queryCount = 1;
        if (vk->fn.vkCreateQueryPool(vk->device, &qi, NULL, &v->query) != VK_SUCCESS)
            goto fail;
    }
    return DE_OK;
fail:
    de__vk_accel_destroy(d, a);
    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vulkan: acceleration structure allocation");
}
u64 de__vk_accel_address(de_gpu_device *d, de__gpu_accel *a) {
    de__vk_device *vk = de__vk(d);
    VkAccelerationStructureDeviceAddressInfoKHR i = {0};
    i.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    i.accelerationStructure = DE__GPU_PAYLOAD(de__vk_accel, a)->handle;
    return vk->fn.vkGetAccelerationStructureDeviceAddressKHR(vk->device, &i);
}
/* Internal dependencies order builds, translations, copies and ray queries even when the caller
 * only supplies the documented geometry/input-buffer transitions. */
static void dependency(de_gpu_cmd *c) {
    VkMemoryBarrier2 m = {0};
    m.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    m.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    m.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo i = {0};
    i.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    i.memoryBarrierCount = 1;
    i.pMemoryBarriers = &m;
    de__vk(c->device)->fn.vkCmdPipelineBarrier2(de__vk_cmd_of(c)->cb, &i);
}
static void build(de_gpu_cmd *c, de__gpu_accel *a, bool update, de__gpu_accel_scratch *s) {
    de__vk_accel *v = DE__GPU_PAYLOAD(de__vk_accel, a);
    VkAccelerationStructureBuildGeometryInfoKHR b = info(&a->desc);
    b.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                    : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    b.srcAccelerationStructure = update ? v->handle : VK_NULL_HANDLE;
    b.dstAccelerationStructure = v->handle;
    b.pGeometries = v->geometry;
    u64 alignment = v->scratch_alignment;
    b.scratchData.deviceAddress =
        (DE__GPU_PAYLOAD(de__vk_buffer, s->scratch)->address + alignment - 1) & ~(alignment - 1);
    const VkAccelerationStructureBuildRangeInfoKHR *ranges = v->ranges;
    dependency(c);
    de__vk(c->device)->fn.vkCmdBuildAccelerationStructuresKHR(de__vk_cmd_of(c)->cb, 1, &b, &ranges);
    dependency(c);
}
void de__vk_accel_blas(de_gpu_cmd *c, de__gpu_accel *a, bool update) {
    a->built = false;
    de__vk_accel *v = DE__GPU_PAYLOAD(de__vk_accel, a);
    de__gpu_accel_scratch *s = de__gpu_accel_scratch_get(
        c,
        (update ? a->sizes.update_scratch_bytes : a->sizes.build_scratch_bytes) +
            v->scratch_alignment,
        0);
    if (!s)
        return;
    for (u32 i = 0; i < a->desc.geometry_count; ++i) {
        v->geometry[i] = geometry(c->device, &a->geometries[i]);
        memset(&v->ranges[i], 0, sizeof v->ranges[i]);
        v->ranges[i].primitiveCount =
            (a->geometries[i].indices.id ? a->geometries[i].index_count
                                         : a->geometries[i].vertex_count) /
            3;
    }
    build(c, a, update, s);
    a->built = true;
}
void de__vk_accel_tlas(de_gpu_cmd *c, de__gpu_accel *a, de__gpu_buffer *instances, u32 count,
                       bool update) {
    a->built = false;
    de__vk_accel *v = DE__GPU_PAYLOAD(de__vk_accel, a);
    de__gpu_accel_scratch *s = de__gpu_accel_scratch_get(
        c,
        (update ? a->sizes.update_scratch_bytes : a->sizes.build_scratch_bytes) +
            v->scratch_alignment,
        count ? count : 1);
    if (!s || !de__gpu_accel_translate_gpu(c, instances, s, count))
        return;
    memset(v->geometry, 0, sizeof *v->geometry);
    v->geometry->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    v->geometry->geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    v->geometry->geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    v->geometry->geometry.instances.data.deviceAddress =
        DE__GPU_PAYLOAD(de__vk_buffer, s->instances)->address;
    memset(v->ranges, 0, sizeof *v->ranges);
    v->ranges->primitiveCount = count;
    build(c, a, update, s);
    a->built = true;
}
void de__vk_accel_emit(de_gpu_cmd *c, de__gpu_accel *a) {
    de__vk_device *vk = de__vk(c->device);
    de__vk_accel *v = DE__GPU_PAYLOAD(de__vk_accel, a);
    dependency(c);
    vk->fn.vkCmdResetQueryPool(de__vk_cmd_of(c)->cb, v->query, 0, 1);
    vk->fn.vkCmdWriteAccelerationStructuresPropertiesKHR(
        de__vk_cmd_of(c)->cb, 1, &v->handle,
        VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, v->query, 0);
}
bool de__vk_accel_read(de_gpu_device *d, de__gpu_accel *a, u64 *bytes) {
    de__vk_device *vk = de__vk(d);
    return vk->fn.vkGetQueryPoolResults(vk->device, DE__GPU_PAYLOAD(de__vk_accel, a)->query, 0, 1,
                                        sizeof *bytes, bytes, sizeof *bytes,
                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
}
void de__vk_accel_compact(de_gpu_cmd *c, de__gpu_accel *src, de__gpu_accel *dst) {
    VkCopyAccelerationStructureInfoKHR i = {0};
    i.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
    i.src = DE__GPU_PAYLOAD(de__vk_accel, src)->handle;
    i.dst = DE__GPU_PAYLOAD(de__vk_accel, dst)->handle;
    i.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
    dependency(c);
    de__vk(c->device)->fn.vkCmdCopyAccelerationStructureKHR(de__vk_cmd_of(c)->cb, &i);
    dependency(c);
    dst->built = true;
}
void de__vk_bindless_write_accel(de_gpu_device *d, u32 index, de__gpu_accel *a) {
    de__vk_device *vk = de__vk(d);
    VkWriteDescriptorSetAccelerationStructureKHR i = {0};
    i.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    i.accelerationStructureCount = 1;
    i.pAccelerationStructures = &DE__GPU_PAYLOAD(de__vk_accel, a)->handle;
    VkWriteDescriptorSet w = {0};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.pNext = &i;
    w.dstSet = vk->set;
    w.dstBinding = DE__VK_BINDING_ACCELS;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vk->fn.vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);
}

bool de__vk_accel_translate(de_gpu_cmd *c, de__gpu_buffer *input, de__gpu_buffer *output,
                            de__gpu_buffer *addresses, u32 count, u32 table_count) {
    de_gpu_device *d = c->device;
    if (!c->accel_pipeline) {
        size_t n =
            DE__GPU_ALIGN16(sizeof(de__gpu_pipeline)) + d->vt->payload_size[DE__GPU_PIPELINE];
        de__gpu_pipeline *p = DE_ALLOC(d->alloc, n, 16, DE_MEM_RENDER);
        if (!p)
            return false;
        memset(p, 0, n);
        p->kind = DE_PIPE_COMPUTE;
        p->push_constant_bytes = 20;
        de_gpu_pipeline_desc desc = {0};
        desc.kind = DE_PIPE_COMPUTE;
        desc.push_constant_bytes = 20;
        desc.cs = (de_gpu_shader_blob){de__accel_instances_spirv, sizeof de__accel_instances_spirv,
                                       "main"};
        desc.threadgroup_size[0] = 64;
        desc.threadgroup_size[1] = desc.threadgroup_size[2] = 1;
        if (de__vk_pipeline_create(d, p, &desc) != DE_OK) {
            DE_FREE(d->alloc, p, n);
            return false;
        }
        c->accel_pipeline = p;
    }
    const u32 pc[5] = {input->bindless, output->bindless, addresses->bindless, count, table_count};
    dependency(c);
    de__vk_cmd_set_pipeline(c, c->accel_pipeline);
    de__vk_cmd_push_constants(c, pc, sizeof pc);
    if (count)
        de__vk_cmd_dispatch(c, (count + 63) / 64 > 65535 ? 65535 : (count + 63) / 64, 1, 1);
    dependency(c);
    return true;
}
