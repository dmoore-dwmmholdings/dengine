/* DENG-42: validation, immutable geometry ownership and asynchronous compaction. */
#include "gpu/gpu_accel_internal.h"
#include <string.h>

static de_result invalid(de_gpu_device *d, const char *why) {
    return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "accel: %s", why);
}

de_result de__gpu_accel_validate(de_gpu_device *d, const de_gpu_accel_desc *x) {
    if (!d || !x)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "accel: NULL device/description");
    if (x->is_tlas) {
        if (!x->max_instances || x->geometry_count || x->geometries)
            return invalid(d, "TLAS requires max_instances and no geometries");
        return DE_OK;
    }
    if (!x->geometry_count || !x->geometries || x->max_instances)
        return invalid(d, "BLAS requires geometries and no max_instances");
    for (u32 i = 0; i < x->geometry_count; ++i) {
        const de_gpu_blas_geometry *g = &x->geometries[i];
        de__gpu_buffer *v = de__gpu_get_buffer(d, g->vertices);
        de__gpu_buffer *b = de__gpu_get_buffer(d, g->indices);
        if (!v || !(v->usage & DE_USAGE_ACCEL_INPUT) || !g->vertex_count ||
            g->vertex_format != DE_FMT_RGB32_FLOAT || g->vertex_stride < 12 ||
            g->vertex_stride % 4 || g->vertex_offset % 4 || g->vertex_offset > v->size ||
            (u64)(g->vertex_count - 1) * g->vertex_stride + 12 > v->size - g->vertex_offset)
            return invalid(d, "invalid float3 vertex range, usage or stale buffer");
        if (g->indices.id) {
            const u32 width = g->index_u16 ? 2u : 4u;
            if (!b || !(b->usage & DE_USAGE_ACCEL_INPUT) || !g->index_count || g->index_count % 3 ||
                g->index_offset % width || g->index_offset > b->size ||
                (u64)g->index_count * width > b->size - g->index_offset)
                return invalid(d, "invalid triangle index range, usage or stale buffer");
        } else if (g->index_count || g->index_offset || g->vertex_count % 3)
            return invalid(d, "nonindexed triangles need vertex_count divisible by three");
    }
    return DE_OK;
}

de_gpu_accel_sizes de_gpu_accel_query_sizes(de_gpu_device *d, const de_gpu_accel_desc *desc) {
    de_gpu_accel_sizes s = {0};
    if (de__gpu_accel_validate(d, desc) != DE_OK)
        return s;
    if (de__gpu_poll_lost(d, "de_gpu_accel_query_sizes")) {
        de__gpu_lost_error("de_gpu_accel_query_sizes");
        return s;
    }
    if (!d->vt->accel_query_sizes) {
        de__gpu_fail(d, DE_ERR_UNSUPPORTED, "accel: backend has no acceleration structures");
        return s;
    }
    if (d->vt->accel_query_sizes(d, desc, &s) != DE_OK)
        memset(&s, 0, sizeof s);
    return s;
}

de_gpu_accel de__gpu_accel_create(de_gpu_device *d, const de_gpu_accel_desc *desc, u64 bytes) {
    de_gpu_accel h = {0};
    de_gpu_accel_sizes sizes = de_gpu_accel_query_sizes(d, desc);
    if (!sizes.accel_bytes || !d->vt->accel_create)
        return h;
    if (bytes)
        sizes.accel_bytes = bytes;
    void *record;
    h.id = de__gpu_pool_alloc(&d->pools[DE__GPU_ACCEL], &record);
    if (!h.id)
        return h;
    de__gpu_accel *a = record;
    a->is_tlas = desc->is_tlas;
    a->sizes = sizes;
    a->desc = *desc;
    a->desc.name = a->name;
    if (desc->name) {
        size_t len = strlen(desc->name);
        if (len >= sizeof a->name)
            len = sizeof a->name - 1;
        memcpy(a->name, desc->name, len);
        a->name[len] = 0;
    }
    if (desc->geometry_count) {
        size_t n = (size_t)desc->geometry_count * sizeof *a->geometries;
        a->geometries = DE_ALLOC(d->alloc, n, 16, DE_MEM_RENDER);
        if (!a->geometries)
            goto fail;
        memcpy(a->geometries, desc->geometries, n);
        a->desc.geometries = a->geometries;
    }
    if (a->is_tlas) {
        a->bindless = de__gpu_bindless_alloc(&d->bindless, DE__GPU_BINDLESS_ACCEL);
        if (!a->bindless)
            goto fail;
    }
    if (d->vt->accel_create(d, a, &a->desc) != DE_OK)
        goto fail;
    if (a->bindless && d->vt->bindless_write_accel)
        d->vt->bindless_write_accel(d, a->bindless, a);
    return h;
fail:
    de__gpu_bindless_free(&d->bindless, DE__GPU_BINDLESS_ACCEL, a->bindless);
    if (a->geometries)
        DE_FREE(d->alloc, a->geometries, (size_t)desc->geometry_count * sizeof *a->geometries);
    de__gpu_pool_release(&d->pools[DE__GPU_ACCEL],
                         de__gpu_pool_retire(&d->pools[DE__GPU_ACCEL], h.id));
    h.id = 0;
    return h;
}

u32 de_gpu_accel_bindless(de_gpu_device *d, de_gpu_accel h) {
    de__gpu_accel *a = d ? de__gpu_get_accel(d, h) : NULL;
    if (!a || !a->is_tlas) {
        if (d)
            invalid(d, "bindless requires a live TLAS");
        return DE_GPU_BINDLESS_INVALID;
    }
    return a->bindless;
}

static de__gpu_accel *command(de_gpu_cmd *c, de_gpu_accel h) {
    if (!c)
        return NULL;
    de__gpu_accel *a = de__gpu_get_accel(c->device, h);
    if (c->state != DE__GPU_CMD_RECORDING || c->in_render || c->queue == DE_QUEUE_COPY || !a) {
        invalid(c->device, "command requires recording graphics/compute list and live AS");
        return NULL;
    }
    return a;
}

void de_gpu_cmd_build_blas(de_gpu_cmd *c, de_gpu_accel h, bool update) {
    de__gpu_accel *a = command(c, h);
    if (!a)
        return;
    if (a->is_tlas || (update && (!a->desc.allow_update || !a->built))) {
        invalid(c->device, "BLAS refit requires built BLAS with allow_update");
        return;
    }
    if (de__gpu_accel_validate(c->device, &a->desc) != DE_OK)
        return;
    if (!c->device->vt->cmd_build_blas)
        return;
    c->device->vt->cmd_build_blas(c, a, update);
    a->emit_cmd = NULL;
    a->emit_value = 0;
}

void de_gpu_cmd_build_tlas(de_gpu_cmd *c, de_gpu_accel h, de_gpu_buffer instances, u32 count,
                           bool update) {
    de__gpu_accel *a = command(c, h);
    if (!a)
        return;
    de__gpu_buffer *b = de__gpu_get_buffer(c->device, instances);
    if (!a->is_tlas || !b || !(b->usage & DE_USAGE_ACCEL_INPUT) || count > a->desc.max_instances ||
        (u64)count * sizeof(de_gpu_tlas_instance) > b->size ||
        (update && (!a->built || !a->desc.allow_update || count != a->built_count))) {
        invalid(c->device, "invalid TLAS instance range or refit count/flags");
        return;
    }
    if (!c->device->vt->cmd_build_tlas)
        return;
    c->device->vt->cmd_build_tlas(c, a, b, count, update);
    if (a->built)
        a->built_count = count;
}

void de_gpu_cmd_accel_emit_compacted_size(de_gpu_cmd *c, de_gpu_accel h) {
    de__gpu_accel *a = command(c, h);
    if (!a)
        return;
    if (a->is_tlas || !a->built || !a->desc.allow_compaction || a->emit_cmd ||
        !c->device->vt->cmd_accel_emit) {
        invalid(c->device, "compaction size requires built compactable BLAS and no pending query");
        return;
    }
    c->device->vt->cmd_accel_emit(c, a);
    a->emit_cmd = c;
    a->emit_serial = c->accel_serial;
    a->emit_value = 0;
    a->emit_queue = c->queue;
}

void de__gpu_accel_submitted(de_gpu_cmd *c, u64 value) {
    de__gpu_pool *p = &c->device->pools[DE__GPU_ACCEL];
    u32 n = de__gpu_pool_get_stats(p).used;
    for (u32 i = 0; i < n; ++i) {
        if (!de__gpu_pool_is_live(p, i))
            continue;
        de__gpu_accel *a = de__gpu_pool_at(p, i);
        if (a->emit_cmd == c && a->emit_serial == c->accel_serial && !a->emit_value)
            a->emit_value = value;
    }
}

bool de_gpu_accel_compacted_size(de_gpu_device *d, de_gpu_accel h, u64 *bytes) {
    if (bytes)
        *bytes = 0;
    de__gpu_accel *a = d ? de__gpu_get_accel(d, h) : NULL;
    if (!a || !bytes) {
        if (d)
            invalid(d, "size requires live BLAS and output pointer");
        return false;
    }
    if (!a->emit_value || d->vt->queue_completed(d, a->emit_queue) < a->emit_value)
        return false;
    return d->vt->accel_compacted_size && d->vt->accel_compacted_size(d, a, bytes);
}

de_gpu_accel de_gpu_cmd_accel_compact(de_gpu_cmd *c, de_gpu_accel h) {
    de_gpu_accel result = {0};
    de__gpu_accel *a = command(c, h);
    u64 bytes;
    if (!a || !de_gpu_accel_compacted_size(c->device, h, &bytes))
        return result;
    if (!bytes || bytes > a->sizes.accel_bytes || !c->device->vt->cmd_accel_compact)
        return result;
    result = de__gpu_accel_create(c->device, &a->desc, bytes);
    if (result.id) {
        de__gpu_accel *dst = de__gpu_get_accel(c->device, result);
        c->device->vt->cmd_accel_compact(c, a, dst);
        if (!dst->built) {
            de_gpu_accel_destroy(c->device, result);
            result.id = 0;
        }
    }
    return result;
}

void de__gpu_accel_translate(const de_gpu_tlas_instance *in, de__gpu_native_instance *out,
                             const de__gpu_accel_address *table, u32 n) {
    memset(out, 0, sizeof *out);
    memcpy(out->transform, in->transform, sizeof out->transform);
    out->custom_mask = in->custom_index | ((u32)in->mask << 24);
    out->sbt_flags = in->sbt_offset | ((u32)in->flags << 24);
    u32 index = de__gpu_handle_index(in->blas.id);
    if (index < n && table[index].handle == in->blas.id && table[index].address)
        out->address = table[index].address;
    else
        out->custom_mask &= 0xffffffu;
}

de__gpu_buffer *de__gpu_accel_buffer(de_gpu_device *d, u64 bytes, u32 usage, de_gpu_memory memory) {
    size_t n = DE__GPU_ALIGN16(sizeof(de__gpu_buffer)) + d->vt->payload_size[DE__GPU_BUFFER];
    de__gpu_buffer *b = DE_ALLOC(d->alloc, n, 16, DE_MEM_RENDER);
    if (!b)
        return NULL;
    memset(b, 0, n);
    b->size = bytes;
    b->usage = usage;
    b->memory = memory;
    memcpy(b->name, "AS internal", sizeof "AS internal");
    de_gpu_buffer_desc desc = {0};
    desc.size = bytes;
    desc.usage = usage;
    desc.memory = memory;
    desc.name = b->name;
    if (usage & DE_USAGE_STORAGE) {
        b->bindless = de__gpu_bindless_alloc(&d->bindless, DE__GPU_BINDLESS_BUFFER);
        if (!b->bindless)
            goto fail;
    }
    if (d->vt->buffer_create(d, b, &desc) != DE_OK)
        goto fail;
    if (b->bindless && d->vt->bindless_write_buffer)
        d->vt->bindless_write_buffer(d, b->bindless, b);
    return b;
fail:
    de__gpu_bindless_free(&d->bindless, DE__GPU_BINDLESS_BUFFER, b->bindless);
    DE_FREE(d->alloc, b, n);
    return NULL;
}

void de__gpu_accel_buffer_free(de_gpu_device *d, de__gpu_buffer *b) {
    if (!b)
        return;
    d->vt->buffer_destroy(d, b);
    de__gpu_bindless_free(&d->bindless, DE__GPU_BINDLESS_BUFFER, b->bindless);
    DE_FREE(d->alloc, b, DE__GPU_ALIGN16(sizeof *b) + d->vt->payload_size[DE__GPU_BUFFER]);
}

static bool grow(de_gpu_device *d, de__gpu_buffer **b, u64 bytes, u32 usage, de_gpu_memory mem) {
    if (*b && (*b)->size >= bytes)
        return true;
    de__gpu_buffer *next = de__gpu_accel_buffer(d, bytes, usage, mem);
    if (!next)
        return false;
    de__gpu_accel_buffer_free(d, *b);
    *b = next;
    return true;
}

de__gpu_accel_scratch *de__gpu_accel_scratch_get(de_gpu_cmd *c, u64 bytes, u32 instances) {
    de_gpu_device *d = c->device;
    de__gpu_accel_scratch *s = c->accel_cursor;
    if (!s) {
        s = DE_ALLOC(d->alloc, sizeof *s, 16, DE_MEM_RENDER);
        if (!s)
            return NULL;
        memset(s, 0, sizeof *s);
        /* Append, retaining the recording's existing blocks. */
        de__gpu_accel_scratch **tail = &c->accel_scratch;
        while (*tail)
            tail = &(*tail)->next;
        *tail = s;
    }
    c->accel_cursor = s->next;
    if (!grow(d, &s->scratch, bytes ? bytes : 256, DE_USAGE_STORAGE, DE_MEM_GPU_ONLY))
        return NULL;
    if (instances) {
        u32 n = de__gpu_pool_get_stats(&d->pools[DE__GPU_ACCEL]).used;
        if (!grow(d, &s->instances, (u64)instances * 64, DE_USAGE_STORAGE | DE_USAGE_ACCEL_INPUT,
                  DE_MEM_GPU_ONLY) ||
            !grow(d, &s->addresses, (u64)(n ? n : 1) * 16, DE_USAGE_STORAGE, DE_MEM_UPLOAD))
            return NULL;
        memset(s->addresses->mapped, 0, (size_t)s->addresses->size);
        de__gpu_accel_address *table = s->addresses->mapped;
        for (u32 i = 0; i < n; ++i) {
            if (!de__gpu_pool_is_live(&d->pools[DE__GPU_ACCEL], i))
                continue;
            de__gpu_accel *a = de__gpu_pool_at(&d->pools[DE__GPU_ACCEL], i);
            if (a->is_tlas || !a->built)
                continue;
            table[i].handle = de__gpu_pool_id_at(&d->pools[DE__GPU_ACCEL], i);
            table[i].address = d->vt->accel_address(d, a);
        }
    }
    return s;
}

void de__gpu_accel_cmd_fini(de_gpu_cmd *c) {
    if (c->accel_pipeline) {
        c->device->vt->pipeline_destroy(c->device, c->accel_pipeline);
        DE_FREE(c->device->alloc, c->accel_pipeline,
                DE__GPU_ALIGN16(sizeof *c->accel_pipeline) +
                    c->device->vt->payload_size[DE__GPU_PIPELINE]);
    }
    de__gpu_accel_scratch *s = c->accel_scratch;
    while (s) {
        de__gpu_accel_scratch *next = s->next;
        de__gpu_accel_buffer_free(c->device, s->scratch);
        de__gpu_accel_buffer_free(c->device, s->instances);
        de__gpu_accel_buffer_free(c->device, s->addresses);
        DE_FREE(c->device->alloc, s, sizeof *s);
        s = next;
    }
}

bool de__gpu_accel_translate_gpu(de_gpu_cmd *c, de__gpu_buffer *input, de__gpu_accel_scratch *s,
                                 u32 count) {
    if (!c->device->vt->cmd_accel_translate)
        return false;
    bool ok = c->device->vt->cmd_accel_translate(
        c, input, s->instances, s->addresses, count,
        de__gpu_pool_get_stats(&c->device->pools[DE__GPU_ACCEL]).used);
    /* Translation consumes compute state; subsequent user dispatches bind their pipeline again. */
    c->has_pipeline = false;
    return ok;
}
