#include "skin_internal.h"
#include "render/shaders/engine_shaders.h"
#include <string.h>
#include <math.h>

static void release_geometry(de_renderer *r, de__skin_entry *e) {
    for (u32 b = 0; b < 2; ++b) {
        for (u32 l = 0; l < 8; ++l) {
            de_gpu_accel_destroy(r->dev, e->blas[b][l]);
            e->blas[b][l] = (de_gpu_accel){0};
            e->blas_built[b][l] = false;
        }
        de_gpu_buffer_destroy(r->dev, e->output[b]);
        e->output[b] = (de_gpu_buffer){0};
    }
    for (u32 b = 0; b < 4; ++b) {
        de_gpu_buffer_destroy(r->dev, e->description[b]);
        e->description[b] = (de_gpu_buffer){0};
    }
    de_gpu_buffer_destroy(r->dev, e->ranges);
    e->ranges = (de_gpu_buffer){0};
    e->source = NULL;
    e->initialized = false;
}
de_result de__skin_init(de_renderer *r) {
    if (r->skin)
        return DE_OK;
    de__skin *s = DE_NEW(r->alloc, de__skin, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "skinning owner");
    memset(s, 0, sizeof *s);
    s->renderer = r;
    s->frame = UINT64_MAX;
    s->interval = de_cvar_register(
        &(de_cvar_desc){.name = "r.rt.skin_rebuild_interval",
                        .help = "Skinned BLAS full rebuild interval; refit between rebuilds",
                        .type = DE_CVAR_INT,
                        .default_number = 16});
    r->skin = s;
    return DE_OK;
}
static de_result ensure_pipeline(de_renderer *r) {
    de__skin *s = r->skin;
    if (s->pipeline.id)
        return DE_OK;
    const char *entries[] = {"cs_skin"};
    de__engine_shader_code code;
    char why[256];
    de_result result =
        de__engine_shader_load(r->dev, r->alloc, "skin", 0, entries, 1, &code, why, sizeof why);
    if (result == DE_OK) {
        s->pipeline =
            de_gpu_pipeline_create(r->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                                   .cs = code.entries[0].blob,
                                                                   .threadgroup_size = {64, 1, 1},
                                                                   .push_constant_bytes = 32,
                                                                   .name = "Skin"});
        de__engine_shader_code_free(&code);
        if (!s->pipeline.id)
            result = DE_FAIL(DE_ERR_UNSUPPORTED, "skinning compute pipeline");
    }
    return result;
}

void de_render_set_skin_palette(de_renderer *r, de_entity entity, const de_affine *joints,
                                u32 count) {
    if (!r || !entity.bits || (count && !joints) || count > 65535)
        return;
    if (de__skin_init(r) != DE_OK)
        return;
    de__skin *s = r->skin;
    de__skin_input *entry = NULL;
    for (u32 i = 0; i < s->input_count; ++i)
        if (s->inputs[i].entity.bits == entity.bits) {
            entry = &s->inputs[i];
            break;
        }
    if (!entry) {
        for (u32 i = 0; i < s->input_count; ++i)
            if (!s->inputs[i].entity.bits) {
                entry = &s->inputs[i];
                break;
            }
    }
    if (!entry) {
        if (s->input_count == s->input_capacity) {
            u32 capacity = s->input_capacity ? s->input_capacity * 2 : 16;
            de__skin_input *next =
                DE_ALLOC(r->alloc, (size_t)capacity * sizeof *next, 16, DE_MEM_RENDER);
            if (!next)
                return;
            memset(next, 0, (size_t)capacity * sizeof *next);
            if (s->inputs) {
                memcpy(next, s->inputs, (size_t)s->input_count * sizeof *next);
                DE_FREE(r->alloc, s->inputs, (size_t)s->input_capacity * sizeof *next);
            }
            s->inputs = next;
            s->input_capacity = capacity;
        }
        entry = &s->inputs[s->input_count++];
    }
    entry->entity = entity;
    if (count > entry->capacity) {
        de_affine *next = DE_ALLOC(r->alloc, (size_t)count * sizeof *next, 16, DE_MEM_RENDER);
        if (!next)
            return;
        if (entry->joints)
            DE_FREE(r->alloc, entry->joints, (size_t)entry->capacity * sizeof *next);
        entry->joints = next;
        entry->capacity = count;
    }
    if (count)
        memcpy(entry->joints, joints, (size_t)count * sizeof *joints);
    entry->count = count;
}

static de_result reserve_snapshot(const de_allocator *alloc, de__render_world_frame *f, u32 count,
                                  u32 joints) {
    if (count > f->skin_capacity) {
        u32 capacity = count > 16 ? count : 16;
        de__skin_palette_snapshot *next =
            DE_ALLOC(alloc, (size_t)capacity * sizeof *next, 16, DE_MEM_RENDER);
        if (!next)
            return DE_ERR_OUT_OF_MEMORY;
        if (f->skin_palettes)
            DE_FREE(alloc, f->skin_palettes, (size_t)f->skin_capacity * sizeof *next);
        f->skin_palettes = next;
        f->skin_capacity = capacity;
    }
    if (joints > f->skin_joint_capacity) {
        de_affine *next = DE_ALLOC(alloc, (size_t)joints * sizeof *next, 16, DE_MEM_RENDER);
        if (!next)
            return DE_ERR_OUT_OF_MEMORY;
        if (f->skin_joints)
            DE_FREE(alloc, f->skin_joints, (size_t)f->skin_joint_capacity * sizeof *next);
        f->skin_joints = next;
        f->skin_joint_capacity = joints;
    }
    return DE_OK;
}
void de__skin_frame_free(const de_allocator *alloc, de__render_world_frame *f) {
    if (f->skin_palettes)
        DE_FREE(alloc, f->skin_palettes, (size_t)f->skin_capacity * sizeof *f->skin_palettes);
    if (f->skin_joints)
        DE_FREE(alloc, f->skin_joints, (size_t)f->skin_joint_capacity * sizeof *f->skin_joints);
    f->skin_palettes = NULL;
    f->skin_joints = NULL;
    f->skin_count = f->skin_joint_count = f->skin_capacity = f->skin_joint_capacity = 0;
}
de_result de__skin_frame_copy(const de_allocator *alloc, de__render_world_frame *dst,
                              const de__render_world_frame *src) {
    if (dst == src)
        return DE_OK;
    dst->skin_count = dst->skin_joint_count = 0;
    DE_TRY(reserve_snapshot(alloc, dst, src->skin_count, src->skin_joint_count));
    if (src->skin_count)
        memcpy(dst->skin_palettes, src->skin_palettes,
               (size_t)src->skin_count * sizeof *src->skin_palettes);
    if (src->skin_joint_count)
        memcpy(dst->skin_joints, src->skin_joints,
               (size_t)src->skin_joint_count * sizeof *src->skin_joints);
    dst->skin_count = src->skin_count;
    dst->skin_joint_count = src->skin_joint_count;
    return DE_OK;
}
static u32 frame_slot(const de__render_world_frame *f, u64 entity) {
    for (u32 slot = 0; slot < f->instances.count; ++slot)
        if (f->instances.entity[slot] == entity)
            return slot;
    return UINT32_MAX;
}
de_result de__skin_extract(de_renderer *r, de__render_world_frame *f) {
    f->skin_count = f->skin_joint_count = 0;
    if (!r->skin)
        return DE_OK;
    de__skin *s = r->skin;
    de__render_world *rw = de__rw_of(r);
    u32 count = 0;
    u64 joints = 0;
    for (u32 i = 0; i < s->input_count; ++i) {
        de__skin_input *input = &s->inputs[i];
        if (!input->entity.bits)
            continue;
        if (rw && rw->world && !de_entity_alive(rw->world, input->entity)) {
            if (input->joints)
                DE_FREE(r->alloc, input->joints, (size_t)input->capacity * sizeof *input->joints);
            memset(input, 0, sizeof *input);
            continue;
        }
        if (frame_slot(f, input->entity.bits) != UINT32_MAX) {
            ++count;
            joints += input->count;
        }
    }
    if (joints > UINT32_MAX)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "skin snapshot joint capacity");
    DE_TRY(reserve_snapshot(r->alloc, f, count, (u32)joints));
    for (u32 i = 0; i < s->input_count; ++i) {
        const de__skin_input *input = &s->inputs[i];
        if (!input->entity.bits)
            continue;
        u32 slot = frame_slot(f, input->entity.bits);
        if (slot == UINT32_MAX)
            continue;
        f->skin_palettes[f->skin_count++] = (de__skin_palette_snapshot){
            input->entity.bits, slot, f->skin_joint_count, input->count};
        if (input->count)
            memcpy(f->skin_joints + f->skin_joint_count, input->joints,
                   (size_t)input->count * sizeof *input->joints);
        f->skin_joint_count += input->count;
    }
    return DE_OK;
}
static void release_entry(de_renderer *r, de__skin_entry *e) {
    release_geometry(r, e);
    for (u32 j = 0; j < 4; ++j)
        de_gpu_buffer_destroy(r->dev, e->palette[j]);
    memset(e, 0, sizeof *e);
}
static de__skin_entry *render_entry(de_renderer *r, u64 entity) {
    de__skin *s = r->skin;
    de__skin_entry *available = NULL;
    for (u32 i = 0; i < s->count; ++i) {
        if (s->entries[i].entity.bits == entity)
            return &s->entries[i];
        if (!s->entries[i].entity.bits && !available)
            available = &s->entries[i];
    }
    if (!available) {
        if (s->count == s->capacity) {
            u32 capacity = s->capacity ? s->capacity * 2 : 16;
            de__skin_entry *next =
                DE_ALLOC(r->alloc, (size_t)capacity * sizeof *next, 16, DE_MEM_RENDER);
            if (!next)
                return NULL;
            memset(next, 0, (size_t)capacity * sizeof *next);
            if (s->entries) {
                memcpy(next, s->entries, (size_t)s->count * sizeof *next);
                DE_FREE(r->alloc, s->entries, (size_t)s->capacity * sizeof *next);
            }
            s->entries = next;
            s->capacity = capacity;
        }
        available = &s->entries[s->count++];
    }
    available->entity.bits = entity;
    return available;
}

static de_gpu_buffer make_buffer(de_renderer *r, u64 size, u32 usage, de_gpu_memory memory,
                                 const char *name) {
    return de_gpu_buffer_create(
        r->dev, &(de_gpu_buffer_desc){
                    .size = size ? size : 16, .usage = usage, .memory = memory, .name = name});
}
static de_result geometry(de_renderer *r, de__skin_entry *e, const de_render_mesh *source) {
    if (e->source == source && e->output[0].id && e->output[1].id && e->ranges.id &&
        e->description[3].id)
        return DE_OK;
    release_geometry(r, e);
    e->source = source;
    for (u32 i = 0; i < 2; ++i) {
        e->output[i] = make_buffer(r, (u64)source->vertex_count * 60,
                                   DE_USAGE_STORAGE | DE_USAGE_COPY_SRC | DE_USAGE_ACCEL_INPUT,
                                   DE_MEM_GPU_ONLY, "skin current/previous vertices");
        if (!e->output[i].id)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "skin output");
    }
    for (u32 i = 0; i < 4; ++i) {
        e->description[i] =
            de_gpu_buffer_create(r->dev, &(de_gpu_buffer_desc){.size = sizeof(de__gs_mesh),
                                                               .stride = sizeof(de__gs_mesh),
                                                               .usage = DE_USAGE_STORAGE,
                                                               .memory = DE_MEM_UPLOAD,
                                                               .name = "skin source description"});
        if (!e->description[i].id)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "skin description");
    }
    e->ranges = make_buffer(r, (u64)source->lod_count * source->submesh_count * 8, DE_USAGE_STORAGE,
                            DE_MEM_UPLOAD, "skin RT ranges");
    u32 *ranges = de_gpu_buffer_map(r->dev, e->ranges);
    if (!ranges)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "skin ranges");
    for (u32 l = 0; l < source->lod_count; ++l)
        for (u32 sub = 0; sub < source->submesh_count; ++sub) {
            const de_cooked_mesh_range range =
                source->lod_submeshes
                    ? source->lod_submeshes[(size_t)l * source->submesh_count + sub]
                    : (de_cooked_mesh_range){.index_first = source->lods[l].index_first,
                                             .index_count = source->lods[l].index_count};
            ranges[((size_t)l * source->submesh_count + sub) * 2] = range.index_first;
            ranges[((size_t)l * source->submesh_count + sub) * 2 + 1] = range.index_count;
        }
    return DE_OK;
}

de__skin_entry *de__skin_instance(const de_renderer *r, u32 slot) {
    if (r && r->skin)
        for (u32 i = 0; i < r->skin->count; ++i)
            if (r->skin->entries[i].active && r->skin->entries[i].slot == slot)
                return &r->skin->entries[i];
    return NULL;
}

de_result de__skin_prepare(de_renderer *r, const de__render_world_frame *f,
                           const de__gpu_scene_assets *assets) {
    if (!r || !r->skin || !f)
        return DE_OK;
    de__skin *s = r->skin;
    if (s->frame == r->frame_index)
        return DE_OK;
    s->frame = r->frame_index;
    /* Only render-owned entries are touched here. Retire absent generations before reuse;
     * the RHI keeps their submitted GPU work alive through its per-queue fence retirement. */
    for (u32 i = 0; i < s->count; ++i) {
        de__skin_entry *e = &s->entries[i];
        e->active = false;
        bool present = false;
        for (u32 p = 0; p < f->skin_count; ++p)
            if (f->skin_palettes[p].entity == e->entity.bits) {
                present = true;
                break;
            }
        if (e->entity.bits && !present)
            release_entry(r, e);
    }
    if (!assets || !assets->mesh)
        return DE_OK;
    if (f->skin_count)
        DE_TRY(ensure_pipeline(r));
    de_gpu_cmd *cmd = NULL;
    const u32 ring = (u32)(r->frame_index % 4);
    for (u32 i = 0; i < f->skin_count; ++i) {
        const de__skin_palette_snapshot *pose = &f->skin_palettes[i];
        de__skin_entry *e = render_entry(r, pose->entity);
        if (!e)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "skin render entry");
        e->active = false;
        e->slot = pose->slot;
        e->joint_count = pose->count;
        const de_affine *joints = pose->count ? f->skin_joints + pose->offset : NULL;
        const u32 slot = e->slot;
        const de_render_mesh *source = assets->mesh(assets->user, f->instances.mesh[slot]);
        if (!source || !source->vertex_count || !source->stream_format[DE_COOKED_STREAM_JOINTS0] ||
            !source->stream_format[DE_COOKED_STREAM_WEIGHTS0])
            continue;
        DE_TRY(geometry(r, e, source));
        if (!e->palette[ring].id || e->palette_capacity[ring] < e->joint_count) {
            de_gpu_buffer next = make_buffer(r, (u64)e->joint_count * sizeof(de_affine),
                                             DE_USAGE_STORAGE, DE_MEM_UPLOAD, "skin palette ring");
            if (!next.id)
                return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "skin palette");
            de_gpu_buffer_destroy(r->dev, e->palette[ring]);
            e->palette[ring] = next;
            e->palette_capacity[ring] = e->joint_count;
        }
        if (e->joint_count)
            memcpy(de_gpu_buffer_map(r->dev, e->palette[ring]), joints,
                   (size_t)e->joint_count * sizeof(de_affine));
        de__gs_mesh packed;
        de__gpu_scene_pack_mesh_record(source, &packed);
        memcpy(de_gpu_buffer_map(r->dev, e->description[ring]), &packed, sizeof packed);
        if (!cmd) {
            cmd = de_gpu_cmd_begin(r->dev, DE_QUEUE_GRAPHICS, "Skin");
            if (!cmd)
                return DE_FAIL(DE_ERR_INTERNAL, "Skin command list");
            de_gpu_cmd_set_pipeline(cmd, s->pipeline);
        }
        if (e->initialized)
            e->current ^= 1u;
        de_gpu_barrier begin = {.buffer = e->output[e->current],
                                .before = e->initialized
                                              ? DE_ACCESS_SHADER_READ | DE_ACCESS_ACCEL_READ
                                              : DE_ACCESS_NONE,
                                .after = DE_ACCESS_SHADER_WRITE};
        de_gpu_cmd_barriers(cmd, &begin, 1);
        if (!e->initialized) {
            begin.buffer = e->output[e->current ^ 1u];
            de_gpu_cmd_barriers(cmd, &begin, 1);
        }
        u32 pc[8] = {de_gpu_buffer_bindless(r->dev, e->description[ring]),
                     de_gpu_buffer_bindless(r->dev, e->palette[ring]),
                     de_gpu_buffer_bindless(r->dev, e->output[e->current]),
                     de_gpu_buffer_bindless(r->dev, e->output[e->current ^ 1u]),
                     e->joint_count,
                     source->vertex_count,
                     !e->initialized,
                     0};
        de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
        de_gpu_cmd_dispatch(cmd, (source->vertex_count + 63) / 64, 1, 1);
        de_gpu_barrier end[2] = {{.buffer = e->output[e->current],
                                  .before = DE_ACCESS_SHADER_WRITE,
                                  .after = DE_ACCESS_SHADER_READ | DE_ACCESS_ACCEL_READ},
                                 {.buffer = e->output[e->current ^ 1u],
                                  .before = DE_ACCESS_SHADER_WRITE,
                                  .after = DE_ACCESS_SHADER_READ | DE_ACCESS_ACCEL_READ}};
        de_gpu_cmd_barriers(cmd, end, e->initialized ? 1 : 2);
        e->initialized = true;
        e->active = true;
        e->packed = packed;
        e->packed.vertices = pc[2];
        const u32 n = source->vertex_count;
        const u32 offsets[6] = {0, 12 * n, 24 * n, 40 * n, 48 * n, 56 * n},
                  formats[6] = {2, 2, 3, 1, 1, 9};
        for (u32 k = 0; k < 6; ++k) {
            e->packed.stream_offset[k / 4].v[k % 4] = offsets[k];
            e->packed.stream_format[k / 4].v[k % 4] = formats[k];
        }
        de_aabb bounds = {{source->bounds_min[0], source->bounds_min[1], source->bounds_min[2]},
                          {source->bounds_max[0], source->bounds_max[1], source->bounds_max[2]}};
        de_aabb skinned = bounds;
        for (u32 j = 0; j < e->joint_count; ++j) {
            de_aabb b = de_aabb_transform(bounds, &joints[j]);
            skinned.min = de_v3_min(skinned.min, b.min);
            skinned.max = de_v3_max(skinned.max, b.max);
        }
        if (!e->joint_count)
            skinned = bounds;
        f->instances.bounds[slot] = de_aabb_transform(skinned, &f->instances.transform[slot]);
    }
    if (cmd)
        DE_TRY(de_gpu_queue_submit(r->dev, DE_QUEUE_GRAPHICS,
                                   &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}));
    return DE_OK;
}

de_result de__skin_rt_prepare(de_renderer *r) {
    if (!r->skin)
        return DE_OK;
    de_gpu_cmd *cmd = NULL;
    i64 setting = de_cvar_int(r->skin->interval);
    u32 interval = setting > 0 ? (u32)setting : 16u;
    for (u32 i = 0; i < r->skin->count; ++i) {
        de__skin_entry *e = &r->skin->entries[i];
        if (!e->active)
            continue;
        const u32 b = e->current;
        for (u32 l = 0; l < e->source->lod_count && l < 8; ++l) {
            const de_cooked_mesh_lod *lod = &e->source->lods[l];
            if (!lod->index_count)
                continue;
            if (!e->blas[b][l].id) {
                de_gpu_blas_geometry geometry = {.vertices = e->output[b],
                                                 .vertex_stride = 12,
                                                 .vertex_count = e->source->vertex_count,
                                                 .vertex_format = DE_FMT_RGB32_FLOAT,
                                                 .indices =
                                                     e->source->buffers[DE_RENDER_MESH_INDICES],
                                                 .index_offset = (u64)lod->index_first * 4,
                                                 .index_count = lod->index_count,
                                                 .opaque = false};
                e->blas[b][l] =
                    de_gpu_accel_create(r->dev, &(de_gpu_accel_desc){.geometries = &geometry,
                                                                     .geometry_count = 1,
                                                                     .allow_update = true,
                                                                     .prefer_fast_trace = true,
                                                                     .name = "skin private BLAS"});
                if (!e->blas[b][l].id)
                    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "skin BLAS");
            }
            if (!cmd) {
                cmd = de_gpu_cmd_begin(r->dev, DE_QUEUE_GRAPHICS, "Skin.RefitBLAS");
                if (!cmd)
                    return DE_ERR_INTERNAL;
            }
            bool update = e->blas_built[b][l] &&
                          ((r->frame_index + de_entity_index(e->entity)) % interval) >= 2u;
            de_gpu_cmd_build_blas(cmd, e->blas[b][l], update);
            de_gpu_barrier barrier = {.accel = e->blas[b][l],
                                      .before = DE_ACCESS_ACCEL_WRITE,
                                      .after = DE_ACCESS_ACCEL_READ};
            de_gpu_cmd_barriers(cmd, &barrier, 1);
            e->blas_built[b][l] = true;
            if (update)
                ++e->refits;
            else
                ++e->rebuilds;
        }
    }
    if (cmd)
        DE_TRY(de_gpu_queue_submit(r->dev, DE_QUEUE_GRAPHICS,
                                   &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}));
    return DE_OK;
}
bool de__skin_rt_metadata(de_renderer *r, const de__gs_mesh *mesh, de__rt_mesh_gpu *out) {
    if (!r->skin)
        return false;
    for (u32 i = 0; i < r->skin->count; ++i) {
        de__skin_entry *e = &r->skin->entries[i];
        if (!e->active || e->packed.vertices != mesh->vertices)
            continue;
        out->ranges = de_gpu_buffer_bindless(r->dev, e->ranges);
        out->submeshes = e->source->submesh_count;
        for (u32 l = 0; l < e->source->lod_count && l < 8; ++l) {
            out->blas[l] = e->blas[e->current][l].id;
            out->index_first[l] = e->source->lods[l].index_first;
            if (out->blas[l])
                out->resident_mask |= 1u << l;
        }
        return true;
    }
    return false;
}
void de__skin_release_rt(de_renderer *r) {
    if (!r || !r->skin)
        return;
    for (u32 i = 0; i < r->skin->count; ++i)
        for (u32 b = 0; b < 2; ++b)
            for (u32 l = 0; l < 8; ++l) {
                de_gpu_accel_destroy(r->dev, r->skin->entries[i].blas[b][l]);
                r->skin->entries[i].blas[b][l] = (de_gpu_accel){0};
                r->skin->entries[i].blas_built[b][l] = false;
            }
}
void de__skin_destroy(de_renderer *r) {
    if (!r || !r->skin)
        return;
    de__skin *s = r->skin;
    for (u32 i = 0; i < s->count; ++i)
        release_entry(r, &s->entries[i]);
    for (u32 i = 0; i < s->input_count; ++i)
        if (s->inputs[i].joints)
            DE_FREE(r->alloc, s->inputs[i].joints,
                    (size_t)s->inputs[i].capacity * sizeof *s->inputs[i].joints);
    if (s->inputs)
        DE_FREE(r->alloc, s->inputs, (size_t)s->input_capacity * sizeof *s->inputs);
    de_gpu_pipeline_destroy(r->dev, s->pipeline);
    if (s->entries)
        DE_FREE(r->alloc, s->entries, (size_t)s->capacity * sizeof *s->entries);
    DE_FREE(r->alloc, s, sizeof *s);
    r->skin = NULL;
}
