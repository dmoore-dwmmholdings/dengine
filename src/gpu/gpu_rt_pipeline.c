#include "gpu/gpu_backend.h"
#include <string.h>
static u64 align_up(u64 v, u32 a) {
    return (v + a - 1u) & ~((u64)a - 1u);
}
de_result de__gpu_sbt_layout_make(u32 size, u32 alignment, u32 base, u32 misses, u32 hits,
                                  de__gpu_sbt_layout *out) {
    if (!out || !size || !alignment || !base || (alignment & (alignment - 1)) ||
        (base & (base - 1)) || base < alignment || size > 4096 || base > 65536 || misses > 65536 ||
        hits > 65536)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "SBT: invalid size/alignment/count");
    u64 stride = align_up(size, alignment);
    *out = (de__gpu_sbt_layout){.raygen_size = align_up(stride, base), .stride = (u32)stride};
    out->miss_offset = out->raygen_size;
    out->miss_size = stride * misses;
    out->hit_offset = align_up(out->miss_offset + out->miss_size, base);
    out->hit_size = stride * hits;
    out->bytes = align_up(out->hit_offset + out->hit_size, base);
    return DE_OK;
}
void de__gpu_rt_desc_init(const de_gpu_pipeline_desc *p, de_gpu_rt_desc *r,
                          de_gpu_rt_hit_group *g) {
    if (p->rt)
        *r = *p->rt;
    else {
        *g = (de_gpu_rt_hit_group){p->closest_hit, p->any_hit};
        *r = (de_gpu_rt_desc){.raygen = p->raygen,
                              .miss = p->miss.code ? &p->miss : NULL,
                              .miss_count = p->miss.code ? 1u : 0u,
                              .hit_groups = g,
                              .hit_group_count = (p->closest_hit.code || p->any_hit.code) ? 1u : 0u,
                              .max_recursion = p->rt_max_recursion,
                              .payload_bytes = p->rt_payload_bytes};
    }
    if (!r->max_recursion)
        r->max_recursion = 1;
    if (!r->attribute_bytes)
        r->attribute_bytes = 8;
}
static bool blob(de_gpu_shader_blob b) {
    return b.code && b.size;
}
de_result de__gpu_rt_validate(de_gpu_device *d, const de_gpu_pipeline_desc *p) {
    de_gpu_rt_desc r;
    de_gpu_rt_hit_group g;
    de__gpu_rt_desc_init(p, &r, &g);
    if (!blob(r.raygen) || r.miss_count > 65536 || r.hit_group_count > 65536 ||
        (r.miss_count && !r.miss) || (r.hit_group_count && !r.hit_groups))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "RT: raygen or stage arrays invalid");
    for (u32 i = 0; i < r.miss_count; ++i)
        if (!blob(r.miss[i]))
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "RT: missing miss shader %u", i);
    for (u32 i = 0; i < r.hit_group_count; ++i)
        if (!blob(r.hit_groups[i].closest_hit))
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "RT: missing closest-hit shader %u", i);
    for (u32 i = 0; i < r.hit_group_count; ++i) {
        de_gpu_shader_blob any = r.hit_groups[i].any_hit;
        if ((any.code != NULL) != (any.size != 0))
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "RT: malformed any-hit blob");
    }
    if (!d->caps.rt_pipelines)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED, "RT: caps.rt_pipelines is false");
    if (r.max_recursion > d->caps.max_rt_recursion)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED, "RT: recursion %u exceeds device maximum %u",
                            r.max_recursion, d->caps.max_rt_recursion);
    if (r.payload_bytes > d->caps.max_rt_payload_bytes ||
        r.attribute_bytes > d->caps.max_rt_attribute_bytes)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "RT: payload/attributes exceed device limits");
    return DE_OK;
}
de_result de__gpu_rt_sbt_create(de_gpu_device *d, de__gpu_pipeline *p, const void *handles,
                                u32 size, u32 alignment, u32 base, u32 misses, u32 hits) {
    DE_TRY(de__gpu_sbt_layout_make(size, alignment, base, misses, hits, &p->rt_layout));
    p->rt_sbt = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = p->rt_layout.bytes,
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_SHADER_BINDING_TABLE,
                                 .memory = DE_MEM_UPLOAD,
                                 .name = "pipeline SBT"});
    if (!p->rt_sbt.id)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RT: SBT allocation");
    u8 *dst = de_gpu_buffer_map(d, p->rt_sbt);
    memset(dst, 0, (size_t)p->rt_layout.bytes);
    const u8 *src = handles;
    memcpy(dst, src, size);
    for (u32 i = 0; i < misses; ++i)
        memcpy(dst + p->rt_layout.miss_offset + (u64)i * p->rt_layout.stride,
               src + (u64)(1 + i) * size, size);
    for (u32 i = 0; i < hits; ++i)
        memcpy(dst + p->rt_layout.hit_offset + (u64)i * p->rt_layout.stride,
               src + (u64)(1 + misses + i) * size, size);
    p->rt_miss_count = misses;
    p->rt_hit_count = hits;
    return DE_OK;
}
