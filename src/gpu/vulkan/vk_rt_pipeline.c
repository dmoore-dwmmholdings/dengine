#include "gpu/vulkan/vk_internal.h"
#include <string.h>

de_result de__vk_rt_pipeline_create(de_gpu_device *d, de__gpu_pipeline *p,
                                    const de_gpu_pipeline_desc *desc) {
    de__vk_device *v = de__vk(d);
    de_gpu_rt_desc rt;
    de_gpu_rt_hit_group shorthand;
    de__gpu_rt_desc_init(desc, &rt, &shorthand);
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR props = {0};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 properties = {0};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &props;
    v->fn.vkGetPhysicalDeviceProperties2(v->phys.handle, &properties);
    const u32 groups = 1 + rt.miss_count + rt.hit_group_count,
              capacity = 1 + rt.miss_count + 2 * rt.hit_group_count;
    VkPipelineShaderStageCreateInfo *stages =
        DE_ALLOC(d->alloc, (size_t)capacity * sizeof *stages, 16, DE_MEM_RENDER);
    VkRayTracingShaderGroupCreateInfoKHR *table =
        DE_ALLOC(d->alloc, (size_t)groups * sizeof *table, 16, DE_MEM_RENDER);
    u8 *handles =
        DE_ALLOC(d->alloc, (size_t)groups * props.shaderGroupHandleSize, 16, DE_MEM_RENDER);
    de_result result = DE_ERR_OUT_OF_MEMORY;
    u32 count = 0;
    if (!stages || !table || !handles)
        goto cleanup;
    memset(stages, 0, (size_t)capacity * sizeof *stages);
    memset(table, 0, (size_t)groups * sizeof *table);
    for (u32 i = 0; i < groups; ++i) {
        table[i].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        table[i].generalShader = table[i].closestHitShader = table[i].anyHitShader =
            table[i].intersectionShader = VK_SHADER_UNUSED_KHR;
        table[i].type = i <= rt.miss_count
                            ? VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR
                            : VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        for (u32 a = 0; a < 2; ++a) {
            de_gpu_shader_blob b;
            VkShaderStageFlagBits stage;
            if (!i) {
                if (a)
                    break;
                b = rt.raygen;
                stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
            } else if (i <= rt.miss_count) {
                if (a)
                    break;
                b = rt.miss[i - 1];
                stage = VK_SHADER_STAGE_MISS_BIT_KHR;
            } else {
                const de_gpu_rt_hit_group *g = &rt.hit_groups[i - 1 - rt.miss_count];
                b = a ? g->any_hit : g->closest_hit;
                stage = a ? VK_SHADER_STAGE_ANY_HIT_BIT_KHR : VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
                if (!b.code)
                    break;
            }
            VkShaderModuleCreateInfo m = {0};
            m.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            m.codeSize = (size_t)b.size;
            m.pCode = b.code;
            VkResult vr = v->fn.vkCreateShaderModule(v->device, &m, NULL, &stages[count].module);
            if (vr != VK_SUCCESS) {
                result = de__vk_fail(d, vr, "RT shader module");
                goto cleanup;
            }
            stages[count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[count].stage = stage;
            stages[count].pName = b.entry && b.entry[0] ? b.entry : "main";
            if (i <= rt.miss_count)
                table[i].generalShader = count;
            else if (a)
                table[i].anyHitShader = count;
            else
                table[i].closestHitShader = count;
            ++count;
        }
    }
    VkRayTracingPipelineCreateInfoKHR info = {0};
    info.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    info.stageCount = count;
    info.pStages = stages;
    info.groupCount = groups;
    info.pGroups = table;
    info.maxPipelineRayRecursionDepth = rt.max_recursion;
    info.layout = v->pipeline_layout;
    de__vk_pipeline *out = DE__GPU_PAYLOAD(de__vk_pipeline, p);
    out->bind_point = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
    VkResult vr = v->fn.vkCreateRayTracingPipelinesKHR(v->device, VK_NULL_HANDLE, v->pipeline_cache,
                                                       1, &info, NULL, &out->pipeline);
    if (vr != VK_SUCCESS) {
        result = de__vk_fail(d, vr, "vkCreateRayTracingPipelinesKHR");
        goto cleanup;
    }
    vr = v->fn.vkGetRayTracingShaderGroupHandlesKHR(
        v->device, out->pipeline, 0, groups, (size_t)groups * props.shaderGroupHandleSize, handles);
    if (vr != VK_SUCCESS)
        result = de__vk_fail(d, vr, "vkGetRayTracingShaderGroupHandlesKHR");
    else
        result = de__gpu_rt_sbt_create(
            d, p, handles, props.shaderGroupHandleSize, props.shaderGroupHandleAlignment,
            props.shaderGroupBaseAlignment, rt.miss_count, rt.hit_group_count);
    if (result != DE_OK) {
        v->fn.vkDestroyPipeline(v->device, out->pipeline, NULL);
        out->pipeline = VK_NULL_HANDLE;
    }
cleanup:
    if (stages) {
        for (u32 i = 0; i < count; ++i)
            v->fn.vkDestroyShaderModule(v->device, stages[i].module, NULL);
        DE_FREE(d->alloc, stages, (size_t)capacity * sizeof *stages);
    }
    if (table)
        DE_FREE(d->alloc, table, (size_t)groups * sizeof *table);
    if (handles)
        DE_FREE(d->alloc, handles, (size_t)groups * props.shaderGroupHandleSize);
    return result;
}
void de__vk_cmd_dispatch_rays(de_gpu_cmd *c, u32 x, u32 y, u32 z) {
    de__gpu_pipeline *p = c->bound_pipeline;
    de__vk_device *v = de__vk(c->device);
    de__gpu_buffer *b = de__gpu_get_buffer(c->device, p->rt_sbt);
    VkDeviceAddress base = DE__GPU_PAYLOAD(de__vk_buffer, b)->address;
    const de__gpu_sbt_layout *l = &p->rt_layout;
    VkStridedDeviceAddressRegionKHR raygen = {base, l->raygen_size, l->raygen_size};
    VkStridedDeviceAddressRegionKHR miss = {p->rt_miss_count ? base + l->miss_offset : 0, l->stride,
                                            l->miss_size};
    VkStridedDeviceAddressRegionKHR hit = {p->rt_hit_count ? base + l->hit_offset : 0, l->stride,
                                           l->hit_size},
                                    callable = {0};
    v->fn.vkCmdTraceRaysKHR(((de__vk_cmd *)de__gpu_cmd_payload(c))->cb, &raygen, &miss, &hit,
                            &callable, x, y, z);
}
