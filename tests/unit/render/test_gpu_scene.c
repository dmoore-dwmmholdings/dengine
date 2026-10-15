/*
 * test_gpu_scene.c: the GPU scene (DENG-177; src/render/scene/).
 *
 * The scene is driven by synthetic render-world buffers (a "fake render world" below that keeps
 * the DENG-175 contract: stable slots, LIFO reuse never in the same frame, ascending dirty and
 * removed lists, origin_changed), and by fake mesh, material and texture records, so the tests
 * need no ECS and no asset system and can run 100k instances cheaply.
 *
 * Every frame is checked against the fake render world, field by field:
 *   - null backend: the frame's upload plan is applied to CPU copies of the tables
 *     (de__gpu_scene_debug_apply), so the dirty tracking, the runs, the scatter lists and the
 *     ring layout are checked on every host and under the sanitizers;
 *   - Metal, Vulkan (CI: lavapipe with validation and synchronization validation) and D3D12 (CI:
 *     WARP): the tables are read back from the GPU after the frame (growth copies, copy_buffer
 *     runs and the scatter.slang kernels, cooked at test time when the build has the shader
 *     importer; else copy-only).
 * Tests: layout_match, zero_upload_steady, incremental_bytes, random_ops_readback_<backend>,
 * growth_preserves, slot_reuse, origin_rebase_full_upload, view_constants, renderer_owns_scene,
 * no_heap_after_warmup.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* layout_match reads scene_types.slang with fopen and sscanf */
#endif
#include "de_alloc_guard.h"
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_render.h>

#include <dengine/de_ecs.h>
#include <dengine/de_task.h>

#include "render/components_internal.h"
#include "render/graph/rg_internal.h"
#include "render/loaders/render_assets.h"
#include "render/renderer_internal.h"
#include "render/scene/gpu_scene.h"
#include "render/shaders/engine_shaders.h"
#include "render/world/render_world.h"

#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_internal.h"
#endif
/* The adapter, the runtime and WARP of a D3D12 device, checked on CI's WARP (DENG-420, DENG-423).
 * The Agility exports come from de_d3d12_agility_deploy (its shared export TU). */
#include "render_d3d12_test.h"
#if defined(DE_GS_TEST_COOK) && DE_GS_TEST_COOK
#include <dengine/de_asset.h>

#include "asset/manifest_internal.h"
#include "cook/importers/shader/shader_importer.h"
#include "cook/importers/shader/slang_compile.h"
#include "cook/lib/de_cooker.h"
#include "gpu/shader/shader_internal.h"
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS (10ull * 1000000000ull)

/* ================================================================================================
 * Fake asset records
 * ============================================================================================== */

#define FAKE_MESHES    12u
#define FAKE_MATERIALS 10u

static de_render_mesh g_meshes[FAKE_MESHES];
static bool g_mesh_ready[FAKE_MESHES + 1];
static de_render_material g_mats[FAKE_MATERIALS];
static bool g_mat_ready[FAKE_MATERIALS + 1];
static bool g_tex_ready;

static void fake_assets_init(void) {
    memset(g_meshes, 0, sizeof g_meshes);
    memset(g_mats, 0, sizeof g_mats);
    for (u32 i = 0; i < FAKE_MESHES; ++i) {
        const u32 id = i + 1u;
        de_render_mesh *m = &g_meshes[i];
        for (u32 b = 0; b < DE_RENDER_MESH_BUFFER_COUNT; ++b)
            m->bindless[b] = id * 10u + b;
        m->vertex_count = id * 100u;
        m->index_count = id * 300u;
        m->meshlet_count = (id % 2u) ? id * 3u : 0u;
        m->submesh_count = 1u + id % 3u;
        m->lod_count = 1u + id % DE_COOKED_MESH_MAX_LODS;
        for (u32 l = 0; l < m->lod_count; ++l)
            m->lods[l] = (de_cooked_mesh_lod){.index_first = l * 30u,
                                              .index_count = 30u,
                                              .meshlet_first = l,
                                              .meshlet_count = 1u,
                                              .triangle_count = 10u - l,
                                              .vertex_count = id * 100u - l,
                                              .error = 0.01f * (f32)l};
        for (u32 c = 0; c < 3; ++c) {
            m->bounds_min[c] = -(f32)id - (f32)c;
            m->bounds_max[c] = (f32)id + (f32)c;
            m->position_offset[c] = -(f32)c;
            m->position_scale[c] = 0.5f + (f32)c;
            m->sphere[c] = 0.25f * (f32)c;
        }
        m->sphere[3] = (f32)id * 1.5f;
        for (u32 k = 0; k < 2; ++k) {
            m->uv_offset[k][0] = 0.1f * (f32)k;
            m->uv_offset[k][1] = 0.2f;
            m->uv_scale[k][0] = 1.0f;
            m->uv_scale[k][1] = 2.0f + (f32)k;
        }
        m->lod_error = 0.001f * (f32)id;
        m->max_position_error = 0.0001f * (f32)id;
        for (u32 s = 0; s < DE_COOKED_STREAM_COUNT; ++s) {
            m->stream_offset[s] = s < 4u ? s * 256u * id : UINT32_MAX;
            m->stream_format[s] = s < 4u ? s + 1u : 0u;
        }
        g_mesh_ready[id] = true;
    }
    for (u32 i = 0; i < FAKE_MATERIALS; ++i) {
        const u32 id = i + 1u;
        de_render_material *m = &g_mats[i];
        de_standard_material *s = &m->standard;
        s->base_color = (de_vec4){0.1f * (f32)id, 0.2f, 0.3f, 1.0f};
        s->metallic = 0.5f;
        s->roughness = 0.05f * (f32)id;
        s->normal_scale = 1.0f;
        s->emissive = (de_vec3){1.0f, 0.5f, 0.25f};
        s->emissive_strength = (f32)id;
        s->ior = 1.5f;
        s->specular = 1.0f;
        s->specular_color = (de_vec3){1.0f, 1.0f, 1.0f};
        s->alpha_cutoff = 0.5f;
        s->double_sided = (id % 2u) != 0;
        s->unlit = id == 3u;
        m->alpha_mode = id % 3u;
        m->flags = id << 8;
        for (u32 t = 0; t < DE_COOKED_TEX_COUNT; ++t) {
            if ((t + id) % 3u == 0)
                m->textures[t].id = id * 32u + t + 1u;
            m->texcoord[t] = (u8)((t + id) % 4u == 0);
        }
        g_mat_ready[id] = true;
    }
    g_tex_ready = true;
}

static const de_render_mesh *fake_mesh(void *user, de_mesh m) {
    (void)user;
    return m.id >= 1u && m.id <= FAKE_MESHES && g_mesh_ready[m.id] ? &g_meshes[m.id - 1u] : NULL;
}
static const de_render_material *fake_material(void *user, de_material m) {
    (void)user;
    return m.id >= 1u && m.id <= FAKE_MATERIALS && g_mat_ready[m.id] ? &g_mats[m.id - 1u] : NULL;
}
static u32 fake_texture(void *user, de_texture t) {
    (void)user;
    return g_tex_ready ? t.id * 3u + 1u : 0u;
}

/* ================================================================================================
 * Fake render world (the DENG-175 contract)
 * ============================================================================================== */

typedef struct frw {
    de__render_world_frame f;
    u32 icap, lcap;
    u32 *ifree, ifree_n, *ipend, ipend_n;
    u32 *lfree, lfree_n, *lpend, lpend_n;
    u8 *idirty_mark, *ldirty_mark;
    u64 next_entity;
    u64 rng;
} frw;

static u32 rnd(frw *w) {
    w->rng ^= w->rng << 13;
    w->rng ^= w->rng >> 7;
    w->rng ^= w->rng << 17;
    return (u32)(w->rng >> 11);
}
static f32 rndf(frw *w) {
    return (f32)(rnd(w) % 20001u) / 1000.0f - 10.0f;
}

static void *zalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    ASSERT_TRUE(p != NULL);
    return p;
}

static void frw_init(frw *w, u32 icap, u32 lcap, u64 seed) {
    memset(w, 0, sizeof *w);
    w->icap = icap;
    w->lcap = lcap;
    w->rng = seed ? seed : 0x9E3779B97F4A7C15ull;
    w->next_entity = 1;
    de__rw_instances *in = &w->f.instances;
    in->entity = (u64 *)zalloc(icap * sizeof(u64));
    in->mesh = (de_mesh *)zalloc(icap * sizeof(de_mesh));
    in->materials = (de_material *)zalloc((size_t)icap * DE__RW_MATERIALS * sizeof(de_material));
    in->transform = (de_affine *)zalloc(icap * sizeof(de_affine));
    in->prev_transform = (de_affine *)zalloc(icap * sizeof(de_affine));
    in->bounds = (de_aabb *)zalloc(icap * sizeof(de_aabb));
    in->layer_mask = (u32 *)zalloc(icap * sizeof(u32));
    in->flags = (u8 *)zalloc(icap);
    de__rw_lights *li = &w->f.lights;
    li->entity = (u64 *)zalloc(lcap * sizeof(u64));
    li->transform = (de_affine *)zalloc(lcap * sizeof(de_affine));
    li->light = (DeLight *)zalloc(lcap * sizeof(DeLight));
    w->f.dirty_instances = (u32 *)zalloc(icap * sizeof(u32));
    w->f.removed_instances = (u32 *)zalloc(icap * sizeof(u32));
    w->f.dirty_lights = (u32 *)zalloc(lcap * sizeof(u32));
    w->f.removed_lights = (u32 *)zalloc(lcap * sizeof(u32));
    w->ifree = (u32 *)zalloc(icap * sizeof(u32));
    w->ipend = (u32 *)zalloc(icap * sizeof(u32));
    w->lfree = (u32 *)zalloc(lcap * sizeof(u32));
    w->lpend = (u32 *)zalloc(lcap * sizeof(u32));
    w->idirty_mark = (u8 *)zalloc(icap);
    w->ldirty_mark = (u8 *)zalloc(lcap);
    w->f.origin = (de_dvec3){1024.0, -2048.0, 0.0};
}

static void frw_free(frw *w) {
    de__rw_instances *in = &w->f.instances;
    free(in->entity);
    free(in->mesh);
    free(in->materials);
    free(in->transform);
    free(in->prev_transform);
    free(in->bounds);
    free(in->layer_mask);
    free(in->flags);
    free(w->f.lights.entity);
    free(w->f.lights.transform);
    free(w->f.lights.light);
    free(w->f.dirty_instances);
    free(w->f.removed_instances);
    free(w->f.dirty_lights);
    free(w->f.removed_lights);
    free(w->ifree);
    free(w->ipend);
    free(w->lfree);
    free(w->lpend);
    free(w->idirty_mark);
    free(w->ldirty_mark);
    memset(w, 0, sizeof *w);
}

/* Starts a frame: the slots freed last frame become reusable. */
static void frw_begin(frw *w) {
    w->f.frame++;
    w->f.origin_changed = w->f.frame == 1;
    for (u32 i = 0; i < w->ipend_n; ++i)
        w->ifree[w->ifree_n++] = w->ipend[i];
    for (u32 i = 0; i < w->lpend_n; ++i)
        w->lfree[w->lfree_n++] = w->lpend[i];
    w->ipend_n = w->lpend_n = 0;
    for (u32 i = 0; i < w->f.dirty_instance_count; ++i)
        w->idirty_mark[w->f.dirty_instances[i]] = 0;
    for (u32 i = 0; i < w->f.dirty_light_count; ++i)
        w->ldirty_mark[w->f.dirty_lights[i]] = 0;
    w->f.dirty_instance_count = w->f.removed_instance_count = 0;
    w->f.dirty_light_count = w->f.removed_light_count = 0;
}

static int cmp_u32(const void *a, const void *b) {
    const u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Ends a frame: the lists ascending, as DENG-175 gives them. */
static void frw_end(frw *w) {
    qsort(w->f.dirty_instances, w->f.dirty_instance_count, sizeof(u32), cmp_u32);
    qsort(w->f.removed_instances, w->f.removed_instance_count, sizeof(u32), cmp_u32);
    qsort(w->f.dirty_lights, w->f.dirty_light_count, sizeof(u32), cmp_u32);
    qsort(w->f.removed_lights, w->f.removed_light_count, sizeof(u32), cmp_u32);
    w->f.instances.live = 0;
    for (u32 s = 0; s < w->f.instances.count; ++s)
        w->f.instances.live += w->f.instances.entity[s] != 0;
    w->f.lights.live = 0;
    for (u32 s = 0; s < w->f.lights.count; ++s)
        w->f.lights.live += w->f.lights.entity[s] != 0;
}

static void idirty(frw *w, u32 s) {
    if (!w->idirty_mark[s]) {
        w->idirty_mark[s] = 1;
        w->f.dirty_instances[w->f.dirty_instance_count++] = s;
    }
}
static void ldirty(frw *w, u32 s) {
    if (!w->ldirty_mark[s]) {
        w->ldirty_mark[s] = 1;
        w->f.dirty_lights[w->f.dirty_light_count++] = s;
    }
}

static de_affine rnd_affine(frw *w) {
    de_affine a;
    memset(&a, 0, sizeof a);
    for (u32 i = 0; i < 12; ++i)
        a.m[i] = rndf(w);
    return a;
}

static void inst_fields(frw *w, u32 s) {
    de__rw_instances *in = &w->f.instances;
    const u32 r = rnd(w);
    in->mesh[s].id = r % 7u == 0 ? 0u : 1u + r % FAKE_MESHES; /* some without a mesh */
    for (u32 k = 0; k < DE__RW_MATERIALS; ++k) {
        const u32 q = rnd(w);
        in->materials[(size_t)s * DE__RW_MATERIALS + k].id =
            k < 2u && q % 4u != 0 ? 1u + q % FAKE_MATERIALS : 0u;
    }
    in->layer_mask[s] = rnd(w);
    in->flags[s] = (u8)(rnd(w) % 8u);
    if (in->flags[s] & DE__RW_INST_HAS_BOUNDS) {
        const f32 x = rndf(w), y = rndf(w), z = rndf(w);
        in->bounds[s] = (de_aabb){{x, y, z}, {x + 1.0f, y + 2.0f, z + 3.0f}};
    } else {
        in->bounds[s] = de_aabb_empty();
    }
}

static u32 frw_add_inst(frw *w) {
    de__rw_instances *in = &w->f.instances;
    u32 s;
    if (w->ifree_n != 0)
        s = w->ifree[--w->ifree_n];
    else if (in->count < w->icap)
        s = in->count++;
    else
        return DE__RW_NONE;
    in->entity[s] = (w->next_entity++ << 32) | (u64)(s + 1u);
    inst_fields(w, s);
    in->transform[s] = rnd_affine(w);
    in->prev_transform[s] = in->transform[s];
    idirty(w, s);
    return s;
}

static void frw_move_inst(frw *w, u32 s) {
    de__rw_instances *in = &w->f.instances;
    in->prev_transform[s] = in->transform[s];
    in->transform[s] = rnd_affine(w);
    idirty(w, s);
}

static void frw_modify_inst(frw *w, u32 s) {
    inst_fields(w, s);
    idirty(w, s);
}

static void frw_remove_inst(frw *w, u32 s) {
    de__rw_instances *in = &w->f.instances;
    in->entity[s] = 0;
    in->mesh[s].id = 0;
    memset(&in->materials[(size_t)s * DE__RW_MATERIALS], 0, sizeof(de_material) * DE__RW_MATERIALS);
    w->f.removed_instances[w->f.removed_instance_count++] = s;
    w->ipend[w->ipend_n++] = s;
}

static u32 frw_add_light(frw *w) {
    de__rw_lights *li = &w->f.lights;
    u32 s;
    if (w->lfree_n != 0)
        s = w->lfree[--w->lfree_n];
    else if (li->count < w->lcap)
        s = li->count++;
    else
        return DE__RW_NONE;
    li->entity[s] = (w->next_entity++ << 32) | (u64)(s + 7u);
    li->transform[s] = rnd_affine(w);
    DeLight *l = &li->light[s];
    l->kind = (de_light_kind)(rnd(w) % 5u);
    l->color = (de_vec3){rndf(w), rndf(w), rndf(w)};
    l->intensity = rndf(w);
    l->range = rndf(w);
    l->spot_inner = rndf(w);
    l->spot_outer = rndf(w);
    l->area_size = (de_vec2){rndf(w), rndf(w)};
    l->source_radius = rndf(w);
    l->cast_shadows = rnd(w) % 2u != 0;
    ldirty(w, s);
    return s;
}

static void frw_remove_light(frw *w, u32 s) {
    w->f.lights.entity[s] = 0;
    w->f.removed_lights[w->f.removed_light_count++] = s;
    w->lpend[w->lpend_n++] = s;
}

/* A random live slot, or DE__RW_NONE. */
static u32 live_inst(frw *w) {
    const u32 n = w->f.instances.count;
    if (n == 0)
        return DE__RW_NONE;
    for (u32 tries = 0; tries < 8; ++tries) {
        const u32 s = rnd(w) % n;
        if (w->f.instances.entity[s] != 0 && !w->idirty_mark[s])
            return s;
    }
    return DE__RW_NONE;
}

/* ================================================================================================
 * Expected entries (independent of the scene's packing code)
 * ============================================================================================== */

static bool f4eq(const de__gs_float4 *a, f32 x, f32 y, f32 z, f32 w) {
    return a->v[0] == x && a->v[1] == y && a->v[2] == z && a->v[3] == w;
}

static bool rows_eq(const de__gs_float4 *rows, const de_affine *a) {
    for (u32 r = 0; r < 3; ++r)
        if (!f4eq(&rows[r], a->m[r], a->m[3 + r], a->m[6 + r], a->m[9 + r]))
            return false;
    return true;
}

static u32 g_bad_reports;

static void report(const char *what, u32 slot, const char *field) {
    if (g_bad_reports++ < 12)
        fprintf(stderr, "  %s slot %u: %s differs\n", what, slot, field);
}

/* Checks instance entry `e` of slot `s`. Returns false (and reports) on a mismatch. */
static bool check_instance(const de__gpu_scene *gs, const frw *w, u32 s, const de__gs_instance *e) {
    const char *what = "instance";
    const de__rw_instances *in = &w->f.instances;
    if (in->entity[s] == 0) {
        bool ok = e->flags == 0 && e->mesh == DE__GS_NONE;
        if (!ok)
            report("free instance", s, "flags/mesh");
        return ok;
    }
    bool ok = true;
#define CHK(cond, name)                                                                            \
    if (!(cond)) {                                                                                 \
        report(what, s, name);                                                                     \
        ok = false;                                                                                \
    }
    CHK(rows_eq(e->transform, &in->transform[s]), "transform");
    CHK(rows_eq(e->prev_transform, &in->prev_transform[s]), "prev_transform");
    u32 flags = DE__GS_INSTANCE_LIVE;
    if (in->flags[s] & DE__RW_INST_CAST_SHADOWS)
        flags |= DE__GS_INSTANCE_CAST_SHADOWS;
    if (in->flags[s] & DE__RW_INST_VISIBLE_IN_RT)
        flags |= DE__GS_INSTANCE_VISIBLE_IN_RT;
    if (in->flags[s] & DE__RW_INST_HAS_BOUNDS) {
        flags |= DE__GS_INSTANCE_HAS_BOUNDS;
        const de_aabb *b = &in->bounds[s];
        CHK(f4eq(&e->aabb_min, b->min.x, b->min.y, b->min.z, 0.0f), "aabb_min");
        CHK(f4eq(&e->aabb_max, b->max.x, b->max.y, b->max.z, 0.0f), "aabb_max");
        CHK(e->sphere.v[3] > 0.0f, "sphere");
    }
    CHK(e->flags == flags, "flags");
    CHK(e->layer_mask == in->layer_mask[s], "layer_mask");
    CHK(e->entity_lo == (u32)in->entity[s] && e->entity_hi == (u32)(in->entity[s] >> 32), "entity");
    const u32 want_mesh =
        DE_HANDLE_VALID(in->mesh[s]) ? de__gpu_scene_mesh_slot(gs, in->mesh[s]) : DE__GS_NONE;
    CHK(e->mesh == want_mesh && (!DE_HANDLE_VALID(in->mesh[s]) || want_mesh != DE__GS_NONE),
        "mesh");
    for (u32 k = 0; k < DE__GS_INSTANCE_MATERIALS; ++k) {
        const de_material m = in->materials[(size_t)s * DE__RW_MATERIALS + k];
        const u32 want = DE_HANDLE_VALID(m) ? de__gpu_scene_material_slot(gs, m) : DE__GS_NONE;
        CHK(e->materials[k / 4u].v[k % 4u] == want, "materials");
    }
    return ok;
}

static bool check_light(const frw *w, u32 s, const de__gs_light *e) {
    const char *what = "light";
    const de__rw_lights *li = &w->f.lights;
    if (li->entity[s] == 0) {
        const bool ok = e->flags == 0;
        if (!ok)
            report("free light", s, "flags");
        return ok;
    }
    const DeLight *l = &li->light[s];
    bool ok = true;
    CHK(rows_eq(e->transform, &li->transform[s]), "transform");
    CHK(f4eq(&e->color, l->color.x, l->color.y, l->color.z, l->intensity), "color");
    CHK(f4eq(&e->params, l->range, l->spot_inner, l->spot_outer, l->source_radius), "params");
    CHK(f4eq(&e->area, l->area_size.x, l->area_size.y, 0.0f, 0.0f), "area");
    CHK(e->kind == (u32)l->kind, "kind");
    CHK(e->flags == (DE__GS_LIGHT_LIVE | (l->cast_shadows ? DE__GS_LIGHT_CAST_SHADOWS : 0u)),
        "flags");
    CHK(e->entity_lo == (u32)li->entity[s] && e->entity_hi == (u32)(li->entity[s] >> 32), "entity");
    return ok;
}

static bool check_mesh(u32 id, u32 slot, const de__gs_mesh *e) {
    const char *what = "mesh";
    const de_render_mesh *m = fake_mesh(NULL, (de_mesh){id});
    bool ok = true;
    const u32 s = slot;
    if (m == NULL) {
        CHK(e->flags == 0, "not-ready flags");
        return ok;
    }
    CHK(e->flags == (DE__GS_MESH_READY | (m->meshlet_count ? DE__GS_MESH_MESHLETS : 0u)), "flags");
    CHK(e->vertices == m->bindless[DE_RENDER_MESH_VERTICES] &&
            e->indices == m->bindless[DE_RENDER_MESH_INDICES] &&
            e->meshlets == m->bindless[DE_RENDER_MESH_MESHLETS] &&
            e->meshlet_vertices == m->bindless[DE_RENDER_MESH_MESHLET_VERTICES] &&
            e->meshlet_triangles == m->bindless[DE_RENDER_MESH_MESHLET_TRIANGLES],
        "bindless");
    CHK(e->vertex_count == m->vertex_count && e->index_count == m->index_count &&
            e->meshlet_count == m->meshlet_count && e->submesh_count == m->submesh_count &&
            e->lod_count == m->lod_count,
        "counts");
    CHK(f4eq(&e->position_offset, m->position_offset[0], m->position_offset[1],
             m->position_offset[2], m->lod_error),
        "position_offset");
    CHK(f4eq(&e->sphere, m->sphere[0], m->sphere[1], m->sphere[2], m->sphere[3]), "sphere");
    for (u32 l = 0; l < m->lod_count; ++l)
        CHK(e->lods[l].index_first == m->lods[l].index_first &&
                e->lods[l].vertex_count == m->lods[l].vertex_count &&
                e->lods[l].error == m->lods[l].error,
            "lods");
    for (u32 st = 0; st < DE_COOKED_STREAM_COUNT; ++st)
        CHK(e->stream_offset[st / 4u].v[st % 4u] == m->stream_offset[st] &&
                e->stream_format[st / 4u].v[st % 4u] == m->stream_format[st],
            "streams");
    return ok;
}

static bool check_material(u32 id, u32 slot, const de__gs_material *e) {
    const char *what = "material";
    const de_render_material *m = fake_material(NULL, (de_material){id});
    bool ok = true;
    const u32 s = slot;
    if (m == NULL) {
        CHK(e->flags == 0, "not-ready flags");
        return ok;
    }
    const de_standard_material *st = &m->standard;
    CHK(f4eq(&e->base_color, st->base_color.x, st->base_color.y, st->base_color.z,
             st->base_color.w),
        "base_color");
    CHK(f4eq(&e->pbr, st->metallic, st->roughness, st->normal_scale, st->alpha_cutoff), "pbr");
    CHK(e->alpha_mode == m->alpha_mode, "alpha mode"); /* DENG-302: the block's header */
    CHK(e->features == de__material_features(st, m->alpha_mode, e->model), "features");
    CHK(e->flags ==
            (DE__GS_MATERIAL_READY | (st->double_sided ? DE__GS_MATERIAL_DOUBLE_SIDED : 0u) |
             (st->unlit ? DE__GS_MATERIAL_UNLIT : 0u)),
        "flags");
    for (u32 t = 0; t < DE_COOKED_TEX_COUNT; ++t) {
        const u32 want = DE_HANDLE_VALID(m->textures[t]) ? fake_texture(NULL, m->textures[t]) : 0u;
        CHK(de__material_params_texture(e, t) == want, "textures");
        CHK(de__material_params_texcoord(e, t) == (m->texcoord[t] != 0 ? 1u : 0u), "texcoord");
    }
    return ok;
}
#undef CHK

/* ================================================================================================
 * Fixture: a device, a graph, a scene and the fake render world
 * ============================================================================================== */

typedef struct gsx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_arena *arena;
    de_gpu_device *dev;
    de_rg *g;
    de__gpu_scene *gs;
    frw w;
    de_gpu_backend backend;
    bool real_gpu;
    /* null: CPU copies of the tables; GPU: one readback buffer for all of them. */
    u8 *mirror[DE__GS_TABLE_COUNT];
    u32 mirror_cap[DE__GS_TABLE_COUNT];
    de_gpu_buffer rb;
    u64 rb_size;
    u64 rb_off[DE__GS_TABLE_COUNT];
    de_gpu_fence fence;
    u64 frames;
    de__gpu_view_params view;
    bool set_view;
    bool check;  /* verify every table after each frame */
    u32 checked; /* frames verified */
} gsx;

static void gsx_setup(gsx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "gpu_scene_test"}, &f->arena), DE_OK);
    de_error_clear();
    fake_assets_init();
    g_bad_reports = 0;
    f->check = true;
    f->set_view = true;
    f->view.view = de_m4_look_at((de_vec3){0, -10, 2}, (de_vec3){0, 0, 0}, (de_vec3){0, 0, 1});
    f->view.proj = de_m4_perspective_rz(1.0f, 16.0f / 9.0f, 0.1f);
    f->view.camera_position = (de_vec3){0, -10, 2};
    f->view.width = 1280;
    f->view.height = 720;
}

static u32 validation_errors(de_gpu_device *d) {
    switch (de_gpu_get_caps(d)->backend) {
#if DE_HAS_GPU_VULKAN
    case DE_GPU_VULKAN: {
        u32 warnings = 0;
        return de__gpu_vk_validation_messages(d, &warnings);
    }
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
    case DE_GPU_D3D12:
        return de__d3d12_debug_error_count(d);
#endif
    default:
        return 0;
    }
}

static void gsx_teardown(gsx *f) {
    if (f->g) {
        de__rg_reset(f->g);
        de__rg_destroy(f->g);
    }
    de__gpu_scene_destroy(f->gs);
    if (f->dev) {
        de_gpu_wait_idle(f->dev);
        if (f->real_gpu)
            EXPECT_EQ(validation_errors(f->dev), 0u);
        if (DE_HANDLE_VALID(f->rb))
            de_gpu_buffer_destroy(f->dev, f->rb);
        if (DE_HANDLE_VALID(f->fence))
            de_gpu_fence_destroy(f->dev, f->fence);
        de_gpu_wait_idle(f->dev);
        de_gpu_device_destroy(f->dev);
    }
    for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t)
        free(f->mirror[t]);
    frw_free(&f->w);
    if (f->arena)
        de_arena_destroy(f->arena);
    if (f->alloc.alloc)
        EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
}

/* ---------- Scatter kernels, cooked at test time ---------- */

typedef struct gs_kernels {
    bool ok;
    de_gpu_shader_blob inst, light;
    u32 threadgroup[3];
    u32 push_bytes;
} gs_kernels;

#if defined(DE_GS_TEST_COOK) && DE_GS_TEST_COOK
static const char *target_name(de__shader_target t) {
    switch (t) {
    case DE__SHADER_TARGET_SPIRV:
        return "spirv";
    case DE__SHADER_TARGET_DXIL:
        return "dxil";
    case DE__SHADER_TARGET_METALLIB:
        return "metallib";
    default:
        return "none";
    }
}

typedef struct cooked_kernels {
    bool tried;
    gs_kernels k;
    char why[300];
    de_cook_result res;
    de__shader_view view;
} cooked_kernels;
static cooked_kernels g_cooked[DE__SHADER_TARGET_COUNT_];

static bool find_entry(const de__shader_view *v, u32 target, const char *name,
                       de_gpu_shader_blob *out, de__shader_record *rec_out) {
    for (u32 e = 0; e < v->header.entry_count; ++e) {
        const char *n = NULL;
        (void)de__shader_view_entry(v, e, &n);
        if (strcmp(n, name) != 0)
            continue;
        const i32 ri = de__shader_view_find(v, target, 0, e);
        if (ri < 0)
            return false;
        const de__shader_record rec = de__shader_view_record(v, (u32)ri);
        out->code = v->base + rec.code_offset;
        out->size = rec.code_size;
        out->entry = n;
        *rec_out = rec;
        return true;
    }
    return false;
}

/* Cooks shaders/scene/scatter.slang for `backend` (once per target and process). */
static bool cook_kernels(de_gpu_backend backend, gs_kernels *out, char *why, size_t cap) {
    const de__shader_target target = de__shader_target_for_backend(backend);
    if ((u32)target == 0 || (u32)target >= DE__SHADER_TARGET_COUNT_) {
        snprintf(why, cap, "no shader target for %s", de_gpu_backend_name(backend));
        return false;
    }
    cooked_kernels *ck = &g_cooked[target];
    if (!ck->tried) {
        ck->tried = true;
        char msg[200];
        if (!de__slang_available(msg, sizeof msg)) {
            snprintf(ck->why, sizeof ck->why, "no Slang compiler: %.200s", msg);
        } else {
            static bool mounted;
            if (!mounted && de_fs_mount("gsshaders", DE_GS_TEST_SHADER_DIR, 0, 0) == DE_OK)
                mounted = true;
            de_cooker *c = NULL;
            de_cooker_desc cd = {.verify_determinism = DE_COOK_VERIFY_OFF};
            static const char *const exts[] = {".slang"};
            de_asset_type_desc t;
            memset(&t, 0, sizeof t);
            t.name = "shader";
            t.extensions = exts;
            t.extension_count = 1;
            t.importer_version = de_shader_importer_version();
            t.import = de_shader_import;
            const char *targets[1] = {target_name(target)};
            de__manifest_kv kv;
            memset(&kv, 0, sizeof kv);
            kv.key = "targets";
            kv.value.type = DE__MANIFEST_STRING_ARRAY;
            kv.value.u.arr.items = targets;
            kv.value.u.arr.count = 1;
            de__manifest_asset a;
            memset(&a, 0, sizeof a);
            a.id = "scene/scatter";
            a.type = "shader";
            a.source = "scene/scatter.slang";
            a.file = "gsshaders:/assets.toml";
            a.settings = &kv;
            a.setting_count = 1;
            de_result r = !mounted ? DE_ERR_NOT_FOUND : DE_OK;
            if (r == DE_OK &&
                (de_cooker_create(&cd, &c) != DE_OK || de_cooker_register_type(c, &t) != DE_OK))
                r = DE_ERR_INTERNAL;
            if (r == DE_OK) {
                de_cook_input in = {.asset = &a, .asset_root = "gsshaders:/", .source = NULL};
                r = de_cooker_cook(c, &in, &ck->res);
            }
            if (r == DE_OK)
                r = de__shader_view_init(ck->res.data, ck->res.size, &ck->view);
            de_cooker_destroy(c);
            de__shader_record ri, rl;
            if (r == DE_OK &&
                find_entry(&ck->view, (u32)target, "cs_scatter_instances", &ck->k.inst, &ri) &&
                find_entry(&ck->view, (u32)target, "cs_scatter_lights", &ck->k.light, &rl)) {
                ck->k.ok = true;
                memcpy(ck->k.threadgroup, ri.threadgroup, sizeof ck->k.threadgroup);
                ck->k.push_bytes = ri.push_constant_bytes > rl.push_constant_bytes
                                       ? ri.push_constant_bytes
                                       : rl.push_constant_bytes;
            } else {
                snprintf(ck->why, sizeof ck->why, "cooking scatter.slang for %s failed: %.200s",
                         target_name(target), de_error_last() ? de_error_last()->message : "?");
            }
        }
    }
    *out = ck->k;
    snprintf(why, cap, "%s", ck->why);
    return ck->k.ok;
}
#else
static bool cook_kernels(de_gpu_backend backend, gs_kernels *out, char *why, size_t cap) {
    (void)backend;
    memset(out, 0, sizeof *out);
    snprintf(why, cap, "this build has no shader importer (DE_BUILD_TOOLS off or no Slang)");
    return false;
}
#endif

/* ---------- Opening ---------- */

static bool backend_built(de_gpu_backend b) {
    switch (b) {
    case DE_GPU_NULL:
        return true;
#if DE_HAS_GPU_METAL
    case DE_GPU_METAL:
        return true;
#endif
#if DE_HAS_GPU_VULKAN
    case DE_GPU_VULKAN:
        return true;
#endif
#if DE_HAS_GPU_D3D12
    case DE_GPU_D3D12:
        return true;
#endif
    default:
        return false;
    }
}

typedef struct open_opts {
    de_gpu_backend backend;
    u32 instances, lights; /* fake render-world capacity */
    u32 initial_capacity[DE__GS_TABLE_COUNT];
    bool scatter; /* null: dummy scatter pipelines; GPU: the cooked kernels when available */
    /* DENG-421: the engine's own kernels (builtin_scatter: the engine shader "scene_scatter",
     * embedded at build time in every build type); they must be there on a Base-tier device */
    bool builtin;
    u64 staging_bytes; /* own ring */
    u64 budget_bytes;
    u64 seed;
} open_opts;

/* Opens a device of `o->backend` (SKIPs when it is not built or not there), a graph, a scene. */
static void gsx_open(gsx *f, const open_opts *o) {
    static char why[700];
    f->backend = o->backend;
    if (!backend_built(o->backend)) {
        snprintf(why, sizeof why, "the %s backend is not in this build",
                 de_gpu_backend_name(o->backend));
        SKIP(why);
    }
    f->real_gpu = o->backend != DE_GPU_NULL;
#if defined(__APPLE__)
    if (o->backend == DE_GPU_METAL)
        setenv("MTL_DEBUG_LAYER", "1", 0);
#endif
    de_gpu_device_desc desc = {.backend = o->backend,
                               .debug_layer = f->real_gpu,
                               .gpu_validation = o->backend == DE_GPU_VULKAN,
                               .pipeline_cache_dir = ""};
    de_result r = de_gpu_device_create(&desc, &f->dev);
    if (r == DE_ERR_UNSUPPORTED && o->backend == DE_GPU_VULKAN) {
        desc.adapter = 1;
        r = de_gpu_device_create(&desc, &f->dev);
    }
    if (r != DE_OK) {
        f->dev = NULL;
        snprintf(why, sizeof why, "no usable %s device: %.400s", de_gpu_backend_name(o->backend),
                 de_error_last() ? de_error_last()->message : "?");
        SKIP(why);
    }
    const de_gpu_caps *caps = de_gpu_get_caps(f->dev);
    if (f->real_gpu) {
        printf("device: \"%s\" (%s)\n", caps->device_name, de_gpu_backend_name(o->backend));
        render_d3d12_report(f->dev);
        if (o->backend == DE_GPU_METAL && !caps->base_tier_ok) {
            /* CI's macOS runner: a paravirtual GPU below the Base tier hangs on compute (DENG-328).
             * WARP and lavapipe run whatever their tier. */
            snprintf(why, sizeof why, "below the Base tier: %.400s", caps->base_tier_reason);
            SKIP(why);
        }
    }
    ASSERT_EQ(
        de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "gpu_scene"},
                      &f->g),
        DE_OK);
    f->fence = de_gpu_fence_create(f->dev, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(f->fence));

    de__gpu_scene_desc d = {.device = f->dev,
                            .allocator = &f->alloc,
                            .resolver = {.mesh = fake_mesh,
                                         .material = fake_material,
                                         .texture_bindless = fake_texture},
                            .staging_bytes = o->staging_bytes,
                            .budget_bytes = o->budget_bytes};
    memcpy(d.initial_capacity, o->initial_capacity, sizeof d.initial_capacity);
    static const u32 dummy_code[4] = {0x07230203u, 0, 0, 0};
    why[0] = '\0';
    if (o->builtin) {
        d.builtin_scatter = true;
        if (caps->base_tier_ok) {
            /* No code for this backend's target in this build (DXIL needs DXC on the build host):
             * nothing to test here. */
            static const char *const k_entries[2] = {"cs_scatter_instances", "cs_scatter_lights"};
            de__engine_shader_code es;
            if (de__engine_shader_load(f->dev, NULL, "scene_scatter", 0, k_entries, 2, &es, why,
                                       sizeof why) != DE_OK)
                SKIP(why);
            printf("scene_scatter: %s\n", de__engine_shader_origin_name(es.origin));
            de__engine_shader_code_free(&es);
        }
    } else if (o->scatter && o->backend == DE_GPU_NULL) {
        d.scatter_instances = (de_gpu_shader_blob){dummy_code, sizeof dummy_code, "cs"};
        d.scatter_lights = (de_gpu_shader_blob){dummy_code, sizeof dummy_code, "cs"};
        d.scatter_threadgroup[0] = 64;
        d.scatter_threadgroup[1] = d.scatter_threadgroup[2] = 1;
    } else if (o->scatter && !caps->base_tier_ok) {
        /* No pipelines below the Base tier (a hardware device or WARP without shader model 6.6):
         * copy-only. The vendored WARP with the Agility runtime is Base tier (DENG-420). */
        printf("scatter kernels unavailable (below the Base tier: %s): copy-only\n",
               caps->base_tier_reason);
    } else if (o->scatter) {
        gs_kernels k;
        if (cook_kernels(o->backend, &k, why, sizeof why)) {
            d.scatter_instances = k.inst;
            d.scatter_lights = k.light;
            memcpy(d.scatter_threadgroup, k.threadgroup, sizeof d.scatter_threadgroup);
            d.scatter_push_constant_bytes = k.push_bytes;
        } else {
            printf("scatter kernels unavailable (%s): copy-only\n", why);
        }
    }
    ASSERT_EQ(de__gpu_scene_create(&d, &f->gs), DE_OK);
    if (o->builtin && caps->base_tier_ok)
        ASSERT_TRUE(de__gpu_scene_has_scatter(f->gs)); /* DENG-421: in every build type */
    if (f->real_gpu)
        printf("scatter: %s\n", de__gpu_scene_has_scatter(f->gs) ? "compute" : "copy-only");
#if defined(_WIN32) && DE_HAS_GPU_D3D12 && defined(DE_D3D12_TEST_WARP_DLL) &&                      \
    defined(DE_GS_TEST_COOK) && DE_GS_TEST_COOK
    /* With the vendored WARP next to this executable and the shader importer (CI's Windows
     * jobs), the scatter kernels must run: a fallback to the OS runtime or the OS WARP fails here
     * instead of quietly going copy-only (DENG-420). Cooking DXIL needs DXC (dxcompiler), which
     * the Windows runners have. */
    if (o->backend == DE_GPU_D3D12 && o->scatter && caps->vendor_id == 0x1414u &&
        caps->device_id == 0x8cu) {
        EXPECT_TRUE(caps->base_tier_ok);
        if (!de__gpu_scene_has_scatter(f->gs))
            printf("WARP without the scatter kernels: %s\n", why[0] ? why : "?");
        EXPECT_TRUE(de__gpu_scene_has_scatter(f->gs));
    }
#endif
    frw_init(&f->w, o->instances, o->lights, o->seed);
}

/* ---------- One frame ---------- */

typedef struct rb_ctx {
    gsx *f;
    de_rg_resource tables[DE__GS_TABLE_COUNT];
    de_rg_resource rb;
} rb_ctx;

static void readback_pass(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    (void)pass;
    const rb_ctx *c = (const rb_ctx *)user;
    for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t) {
        const de__gpu_scene_table_info ti = de__gpu_scene_table(c->f->gs, (de__gs_table_id)t);
        if (DE_HANDLE_VALID(c->tables[t]) && ti.count != 0)
            de_gpu_cmd_copy_buffer(cmd, c->f->rb, c->f->rb_off[t], ti.buffer, 0,
                                   (u64)ti.count * ti.stride);
    }
}

/* Grows the CPU copies (null) or the readback buffer (GPU) to the tables' capacities. */
static void track_capacity(gsx *f) {
    u64 total = 0;
    for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t) {
        const de__gpu_scene_table_info ti = de__gpu_scene_table(f->gs, (de__gs_table_id)t);
        if (!f->real_gpu && ti.capacity > f->mirror_cap[t]) {
            u8 *n = (u8 *)zalloc((size_t)ti.capacity * ti.stride);
            if (f->mirror[t] != NULL)
                memcpy(n, f->mirror[t], (size_t)f->mirror_cap[t] * ti.stride);
            free(f->mirror[t]);
            f->mirror[t] = n;
            f->mirror_cap[t] = ti.capacity;
        }
        f->rb_off[t] = total;
        total += ((u64)ti.capacity * ti.stride + 255u) & ~(u64)255u;
    }
    if (f->real_gpu && total > f->rb_size) {
        if (DE_HANDLE_VALID(f->rb)) {
            de_gpu_wait_idle(f->dev);
            de_gpu_buffer_destroy(f->dev, f->rb);
        }
        f->rb = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = total,
                                                                   .usage = DE_USAGE_COPY_DST,
                                                                   .memory = DE_MEM_READBACK,
                                                                   .name = "gpu scene readback"});
        ASSERT_TRUE(DE_HANDLE_VALID(f->rb));
        f->rb_size = total;
    }
}

/* Checks every table (the CPU copies on null, the readback on a GPU). */
static bool check_tables(gsx *f) {
    const u8 *data[DE__GS_TABLE_COUNT];
    const u8 *rb = f->real_gpu ? (const u8 *)de_gpu_buffer_map(f->dev, f->rb) : NULL;
    for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t)
        data[t] = f->real_gpu ? rb + f->rb_off[t] : f->mirror[t];
    bool ok = true;
    const frw *w = &f->w;
    const de__gpu_scene_table_info it = de__gpu_scene_table(f->gs, DE__GS_INSTANCES);
    if (it.count < w->f.instances.count) {
        fprintf(stderr, "  instance table count %u < %u\n", it.count, w->f.instances.count);
        return false;
    }
    for (u32 s = 0; s < w->f.instances.count; ++s) {
        de__gs_instance e;
        memcpy(&e, data[DE__GS_INSTANCES] + (size_t)s * sizeof e, sizeof e);
        ok &= check_instance(f->gs, w, s, &e);
        if (w->f.instances.entity[s] == 0)
            continue;
        if (e.mesh != DE__GS_NONE) {
            de__gs_mesh m;
            memcpy(&m, data[DE__GS_MESHES] + (size_t)e.mesh * sizeof m, sizeof m);
            ok &= check_mesh(w->f.instances.mesh[s].id, e.mesh, &m);
        }
        for (u32 k = 0; k < 2; ++k) {
            const u32 ms = e.materials[0].v[k];
            if (ms == DE__GS_NONE)
                continue;
            de__gs_material m;
            memcpy(&m, data[DE__GS_MATERIALS] + (size_t)ms * sizeof m, sizeof m);
            ok &= check_material(w->f.instances.materials[(size_t)s * DE__RW_MATERIALS + k].id, ms,
                                 &m);
        }
    }
    for (u32 s = 0; s < w->f.lights.count; ++s) {
        de__gs_light e;
        memcpy(&e, data[DE__GS_LIGHTS] + (size_t)s * sizeof e, sizeof e);
        ok &= check_light(w, s, &e);
    }
    if (f->set_view) {
        de__gs_view v, want;
        memcpy(&v, data[DE__GS_VIEWS], sizeof v);
        de__gpu_scene_debug_pack(f->gs, DE__GS_VIEWS, 0, &want);
        const bool vok = memcmp(&v, &want, sizeof v) == 0 && v.instances == it.bindless &&
                         v.instance_count == it.count &&
                         v.lights == de__gpu_scene_table(f->gs, DE__GS_LIGHTS).bindless &&
                         v.meshes == de__gpu_scene_table(f->gs, DE__GS_MESHES).bindless;
        if (!vok)
            report("view", 0, "scene constants");
        ok &= vok;
    }
    return ok;
}

/* One frame of the scene with the fake render world's current buffer. */
static bool gsx_frame(gsx *f) {
    const u64 n = ++f->frames;
    if (de__gpu_scene_update(f->gs, &f->w.f) != DE_OK)
        return false;
    if (f->set_view && de__gpu_scene_set_view(f->gs, 0, &f->view) != DE_OK)
        return false;
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    if (de__rg_begin(f->g, f->arena) != DE_OK)
        return false;
    if (de__gpu_scene_add_passes(f->gs, f->g) != DE_OK)
        return false;
    track_capacity(f);
    rb_ctx rc = {.f = f};
    if (f->real_gpu && f->check) {
        de_rg_pass *p = de_rg_add_pass(f->g, "readback", DE_QUEUE_GRAPHICS, readback_pass, &rc);
        rc.rb = de_rg_import_buffer(f->g, f->rb, DE_ACCESS_NONE);
        de_rg_write(p, rc.rb, DE_ACCESS_COPY_DST);
        for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t) {
            if (de__gpu_scene_table(f->gs, (de__gs_table_id)t).count == 0)
                continue;
            rc.tables[t] = de__gpu_scene_import(f->gs, f->g, (de__gs_table_id)t);
            de_rg_read(p, rc.tables[t], DE_ACCESS_COPY_SRC);
            de_rg_export(f->g, rc.tables[t], DE_ACCESS_SHADER_READ);
        }
    }
    de_result r = de__rg_finalize(f->g);
    if (r == DE_OK)
        r = de__rg_compile(f->g);
    if (r == DE_OK)
        r = de__rg_execute(f->g, &(de__rg_execute_desc){
                                     .frame_index = n, .frame_fence = f->fence, .frame_value = n});
    if (r != DE_OK) {
        fprintf(stderr, "frame %llu: %s\n", (unsigned long long)n,
                de_error_last() ? de_error_last()->message : "?");
        de__gpu_scene_frame_end(f->gs, f->g, false);
        return false;
    }
    if (!f->real_gpu) /* the CPU copies follow every frame, checked or not */
        for (u32 t = 0; t < DE__GS_TABLE_COUNT; ++t)
            de__gpu_scene_debug_apply(f->gs, (de__gs_table_id)t, f->mirror[t], f->mirror_cap[t]);
    de__gpu_scene_frame_end(f->gs, f->g, true);
    de__rg_reset(f->g);
    if (!f->check)
        return true;
    if (f->real_gpu && de_gpu_fence_wait(f->dev, f->fence, n, WAIT_NS) != DE_OK)
        return false;
    f->checked++;
    return check_tables(f);
}

/* One frame of the fake render world with `ops` random operations, then the scene. */
static bool random_frame(gsx *f, u32 ops, bool lights) {
    frw *w = &f->w;
    frw_begin(w);
    for (u32 i = 0; i < ops; ++i) {
        const u32 op = rnd(w) % 100u;
        if (op < 25u) {
            (void)frw_add_inst(w);
        } else if (op < 40u) {
            const u32 s = live_inst(w);
            if (s != DE__RW_NONE)
                frw_remove_inst(w, s);
        } else if (op < 75u) {
            const u32 s = live_inst(w);
            if (s != DE__RW_NONE)
                frw_move_inst(w, s);
        } else if (op < 88u) {
            const u32 s = live_inst(w);
            if (s != DE__RW_NONE)
                frw_modify_inst(w, s);
        } else if (lights && op < 94u) {
            (void)frw_add_light(w);
        } else if (lights && w->f.lights.count != 0) {
            const u32 s = rnd(w) % w->f.lights.count;
            if (w->f.lights.entity[s] != 0 && !w->ldirty_mark[s])
                frw_remove_light(w, s);
        }
    }
    if (rnd(w) % 97u == 0) { /* a rebase */
        w->f.origin.x += 1024.0;
        w->f.origin_changed = true;
    }
    if (rnd(w) % 13u == 0) /* the camera moves */
        f->view.view.m[12] += 0.25f;
    if (rnd(w) % 41u == 0) { /* a mesh reloads (new bindless indices) or a texture arrives */
        const u32 id = 1u + rnd(w) % FAKE_MESHES;
        g_meshes[id - 1u].bindless[DE_RENDER_MESH_VERTICES] += 1000u;
    }
    frw_end(w);
    return gsx_frame(f);
}

/* A scene of `n` instances (meshes and materials from the fake records) and `l` lights. */
static void populate(gsx *f, u32 n, u32 l) {
    frw_begin(&f->w);
    for (u32 i = 0; i < n; ++i)
        ASSERT_NE(frw_add_inst(&f->w), DE__RW_NONE);
    for (u32 i = 0; i < l; ++i)
        ASSERT_NE(frw_add_light(&f->w), DE__RW_NONE);
    frw_end(&f->w);
}

/* A frame where nothing changes. */
static void static_frame(gsx *f) {
    frw_begin(&f->w);
    frw_end(&f->w);
    ASSERT_TRUE(gsx_frame(f));
}

/* ================================================================================================
 * Tests
 * ============================================================================================== */

/* ---------- layout_match: scene_types.slang against the C mirrors ---------- */

typedef struct slang_field {
    char type[48], name[64];
    u32 count;
} slang_field;

typedef struct slang_struct {
    char name[48];
    slang_field fields[48];
    u32 field_count;
    u32 size, align;
} slang_struct;

static char *read_text(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
        return NULL;
    fseek(fp, 0, SEEK_END);
    const long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1u);
    if (buf != NULL && fread(buf, 1, (size_t)n, fp) == (size_t)n)
        buf[n] = '\0';
    fclose(fp);
    return buf;
}

/* Strips // comments. */
static void strip_comments(char *s) {
    for (char *p = s; *p; ++p)
        if (p[0] == '/' && p[1] == '/')
            while (*p && *p != '\n')
                *p++ = ' ';
}

/* The size and alignment of a field type (16-byte vectors align to 16; structs to their own). */
static bool type_layout(const slang_struct *known, u32 nknown, const char *type, u32 *size,
                        u32 *align) {
    if (strcmp(type, "uint") == 0 || strcmp(type, "float") == 0) {
        *size = 4;
        *align = 4;
        return true;
    }
    if (strcmp(type, "uint4") == 0 || strcmp(type, "float4") == 0) {
        *size = 16;
        *align = 16;
        return true;
    }
    for (u32 i = 0; i < nknown; ++i)
        if (strcmp(known[i].name, type) == 0) {
            *size = known[i].size;
            *align = known[i].align;
            return true;
        }
    return false;
}

TEST(gpu_scene, layout_match) {
    char path[1024];
    snprintf(path, sizeof path, "%s/scene/scene_types.slang", DE_GS_TEST_SHADER_DIR);
    char *text = read_text(path);
    ASSERT_TRUE(text != NULL);
    strip_comments(text);
    static slang_struct structs[16];
    u32 nstructs = 0;
    /* Structs: "public struct Name { public type name[N]; ... };" */
    for (char *p = strstr(text, "struct "); p != NULL; p = strstr(p, "struct ")) {
        slang_struct *st = &structs[nstructs];
        memset(st, 0, sizeof *st);
        ASSERT_EQ(sscanf(p + 7, "%47s", st->name), 1);
        char *body = strchr(p, '{');
        char *end = body ? strchr(body, '}') : NULL;
        ASSERT_TRUE(body != NULL && end != NULL);
        *end = '\0';
        u32 off = 0, align = 4;
        for (char *q = body + 1; *q;) {
            char *semi = strchr(q, ';');
            if (semi == NULL)
                break;
            *semi = '\0';
            char a[48] = "", b[48] = "", c[64] = "";
            const int k = sscanf(q, "%47s %47s %63s", a, b, c);
            slang_field *fd = &st->fields[st->field_count];
            const char *type = strcmp(a, "public") == 0 ? b : a;
            const char *decl = strcmp(a, "public") == 0 ? c : b;
            ASSERT_TRUE(k >= 2);
            snprintf(fd->type, sizeof fd->type, "%s", type);
            fd->count = 1;
            const char *br = strchr(decl, '[');
            snprintf(fd->name, sizeof fd->name, "%.*s", (int)(br ? br - decl : (long)strlen(decl)),
                     decl);
            if (br)
                fd->count = (u32)strtoul(br + 1, NULL, 10);
            u32 fs = 0, fa = 0;
            if (!type_layout(structs, nstructs, fd->type, &fs, &fa)) {
                fprintf(stderr, "%s.%s: unknown type %s\n", st->name, fd->name, fd->type);
                ASSERT_TRUE(false);
            }
            off = (off + fa - 1u) & ~(fa - 1u);
            off += fs * fd->count;
            if (fa > align)
                align = fa;
            st->field_count++;
            q = semi + 1;
        }
        st->size = (off + 15u) & ~15u;
        st->align = align;
        EXPECT_EQ(off % 16u, 0u); /* every struct a multiple of 16 bytes */
        ++nstructs;
        p = end + 1;
    }
    u32 nlayouts = 0;
    const de__gs_layout *layouts = de__gs_layouts(&nlayouts);
    EXPECT_EQ(nstructs, nlayouts);
    for (u32 i = 0; i < nlayouts; ++i) {
        const de__gs_layout *L = &layouts[i];
        const slang_struct *st = NULL;
        for (u32 j = 0; j < nstructs; ++j)
            if (strcmp(structs[j].name, L->slang_name) == 0)
                st = &structs[j];
        if (st == NULL) {
            fprintf(stderr, "%s is not in scene_types.slang\n", L->slang_name);
            EXPECT_TRUE(false);
            continue;
        }
        EXPECT_EQ(st->size, L->size);
        EXPECT_EQ(st->field_count, L->field_count);
        u32 off = 0;
        for (u32 k = 0; k < st->field_count && k < L->field_count; ++k) {
            const slang_field *sf = &st->fields[k];
            const de__gs_layout_field *cf = &L->fields[k];
            u32 fs = 0, fa = 0;
            (void)type_layout(structs, nstructs, sf->type, &fs, &fa);
            off = (off + fa - 1u) & ~(fa - 1u);
            const u32 size = fs * sf->count;
            if (strcmp(sf->name, cf->name) != 0 || strcmp(sf->type, cf->slang_type) != 0 ||
                off != cf->offset || size != cf->size)
                fprintf(stderr, "%s field %u: slang %s %s @%u (%u bytes), C %s %s @%u (%u bytes)\n",
                        L->slang_name, k, sf->type, sf->name, off, size, cf->slang_type, cf->name,
                        cf->offset, cf->size);
            EXPECT_STREQ(sf->name, cf->name);
            EXPECT_STREQ(sf->type, cf->slang_type);
            EXPECT_EQ(off, cf->offset);
            EXPECT_EQ(size, cf->size);
            if (fa == 16u)
                EXPECT_EQ(off % 16u, 0u);
            off += size;
        }
    }
    free(text);

    /* Constants: "public static const uint NAME = value;" */
    text = read_text(path);
    ASSERT_TRUE(text != NULL);
    strip_comments(text);
    u32 nconst = 0;
    const de__gs_layout_const *consts = de__gs_layout_consts(&nconst);
    u32 found = 0;
    for (char *p = strstr(text, "static const uint "); p != NULL;
         p = strstr(p + 1, "static const uint ")) {
        char name[64] = "";
        char value[32] = "";
        ASSERT_EQ(sscanf(p + 18, "%63s = %31[0-9xXa-fA-F]", name, value), 2);
        const u32 v = (u32)strtoul(value, NULL, 0);
        bool known = false;
        for (u32 i = 0; i < nconst; ++i)
            if (strcmp(consts[i].name, name) == 0) {
                known = true;
                EXPECT_EQ(v, consts[i].value);
            }
        if (!known)
            fprintf(stderr, "constant %s has no C mirror\n", name);
        EXPECT_TRUE(known);
        ++found;
    }
    EXPECT_EQ(found, nconst);
    free(text);
}

/* ---------- zero_upload_steady: 100k static instances upload nothing (S12 exit check) ----------
 */

TEST_F(gpu_scene, zero_upload_steady, gsx) {
    gsx_open(f, &(open_opts){.backend = DE_GPU_NULL, .instances = 100000, .lights = 64});
    f->check = false; /* 100k entries: checked once below */
    populate(f, 100000, 64);
    ASSERT_TRUE(gsx_frame(f));
    const de__gpu_scene_stats *st = de__gpu_scene_stats_get(f->gs);
    EXPECT_EQ(st->frame_entries[DE__GS_INSTANCES], 100000u);
    EXPECT_EQ(st->frame_entries[DE__GS_LIGHTS], 64u);
    EXPECT_EQ(st->frame_entries[DE__GS_VIEWS], 1u);
    EXPECT_GE(st->frame_bytes, 100000ull * sizeof(de__gs_instance));
    EXPECT_EQ(st->deferred_frames, 0u);
    EXPECT_EQ(st->count[DE__GS_INSTANCES], 100000u);
    printf("first frame: %llu bytes, %u copies, %u meshes, %u materials\n",
           (unsigned long long)st->frame_bytes, st->frame_copies, st->live_meshes,
           st->live_materials);
    for (u32 i = 0; i < 30; ++i) {
        static_frame(f);
        EXPECT_EQ(st->frame_bytes, 0u);
        EXPECT_EQ(st->frame_passes, 0u);
        EXPECT_EQ(st->frame_copies, 0u);
    }
    de_render_stats rs;
    memset(&rs, 0, sizeof rs);
    de__gpu_scene_fill_stats(f->gs, &rs);
    EXPECT_EQ(rs.gpu_scene_upload_bytes, 0u);
    EXPECT_EQ(rs.gpu_scene_instances, 100000u);
    EXPECT_EQ(rs.gpu_scene_lights, 64u);
    EXPECT_GE(rs.gpu_scene_gpu_bytes, 100000ull * sizeof(de__gs_instance));
    EXPECT_TRUE(rs.gpu_scene_meshes > 0 && rs.gpu_scene_meshes <= FAKE_MESHES);
}

/* ---------- incremental_bytes: moving 10 instances uploads those entries only ---------- */

static void incremental(gsx *f, bool scatter) {
    gsx_open(f, &(open_opts){.backend = DE_GPU_NULL,
                             .instances = 4096,
                             .lights = 16,
                             .scatter = scatter,
                             .seed = 7});
    populate(f, 4000, 16);
    ASSERT_TRUE(gsx_frame(f));
    static_frame(f);
    const de__gpu_scene_stats *st = de__gpu_scene_stats_get(f->gs);
    ASSERT_EQ(st->frame_bytes, 0u);

    /* 10 instances far apart: 10 entries, 10 runs. */
    frw_begin(&f->w);
    for (u32 i = 0; i < 10; ++i)
        frw_move_inst(&f->w, 17u + i * 389u);
    frw_end(&f->w);
    ASSERT_TRUE(gsx_frame(f));
    EXPECT_EQ(st->frame_entries[DE__GS_INSTANCES], 10u);
    EXPECT_EQ(st->frame_entries[DE__GS_LIGHTS], 0u);
    EXPECT_EQ(st->frame_entries[DE__GS_VIEWS], 0u);
    if (scatter) {
        EXPECT_EQ(st->frame_bytes, 10u * (sizeof(de__gs_instance) + 4u));
        EXPECT_EQ(st->frame_scattered, 10u);
        EXPECT_EQ(st->frame_dispatches, 1u);
        EXPECT_EQ(st->frame_copies, 2u); /* the slots and the payloads into the scratch buffers */
        EXPECT_EQ(st->frame_passes, 2u); /* upload + scatter */
    } else {
        EXPECT_EQ(st->frame_bytes, 10u * sizeof(de__gs_instance));
        EXPECT_EQ(st->frame_copies, 10u);
        EXPECT_EQ(st->frame_passes, 1u);
    }
    static_frame(f);
    EXPECT_EQ(st->frame_bytes, 0u);

    /* 10 neighbours: one run, one copy (also with the scatter kernels: the run is long). */
    frw_begin(&f->w);
    for (u32 i = 0; i < 10; ++i)
        frw_move_inst(&f->w, 1000u + i);
    frw_end(&f->w);
    ASSERT_TRUE(gsx_frame(f));
    EXPECT_EQ(st->frame_bytes, 10u * sizeof(de__gs_instance));
    EXPECT_EQ(st->frame_copies, 1u);
    EXPECT_EQ(st->frame_scattered, 0u);

    /* One light, one camera move. */
    frw_begin(&f->w);
    f->w.f.lights.light[3].intensity = 42.0f;
    ldirty(&f->w, 3);
    frw_end(&f->w);
    f->view.view.m[13] += 1.0f;
    ASSERT_TRUE(gsx_frame(f));
    EXPECT_EQ(st->frame_entries[DE__GS_INSTANCES], 0u);
    EXPECT_EQ(st->frame_entries[DE__GS_LIGHTS], 1u);
    EXPECT_EQ(st->frame_entries[DE__GS_VIEWS], 1u);
    EXPECT_EQ(st->frame_bytes, sizeof(de__gs_light) + (scatter ? 4u : 0u) + sizeof(de__gs_view));
    /* The view's previous matrix catches up once, then nothing. */
    static_frame(f);
    EXPECT_EQ(st->frame_entries[DE__GS_VIEWS], 1u);
    static_frame(f);
    EXPECT_EQ(st->frame_bytes, 0u);
    EXPECT_EQ(f->checked, f->frames);
}

TEST_F(gpu_scene, incremental_bytes, gsx) {
    incremental(f, false);
}

TEST_F(gpu_scene, incremental_bytes_scatter, gsx) {
    incremental(f, true);
}

/* ---------- random_ops_readback: 1000 frames of random changes, every table checked ---------- */

static void random_ops_ex(gsx *f, de_gpu_backend b, bool scatter, bool builtin, u32 frames) {
    gsx_open(f, &(open_opts){.backend = b,
                             .instances = 1500,
                             .lights = 96,
                             .initial_capacity = {64, 4, 4, 16, 1},
                             .scatter = scatter,
                             .builtin = builtin,
                             .seed = 0xDE177ull});
    populate(f, 40, 4);
    ASSERT_TRUE(gsx_frame(f));
    u32 bad = 0;
    for (u32 i = 0; i < frames && bad == 0; ++i)
        if (!random_frame(f, 1u + i % 23u, true)) {
            fprintf(stderr, "frame %u differs\n", i);
            ++bad;
        }
    EXPECT_EQ(bad, 0u);
    const de__gpu_scene_stats *st = de__gpu_scene_stats_get(f->gs);
    printf("%s%s: %u frames checked, %u instances, %u lights, %u meshes, %u materials, %u "
           "growths, %llu bytes\n",
           de_gpu_backend_name(b), scatter ? " + scatter" : "", f->checked,
           st->count[DE__GS_INSTANCES], st->count[DE__GS_LIGHTS], st->live_meshes,
           st->live_materials, st->growths, (unsigned long long)st->total_bytes);
    EXPECT_GT(st->growths, 0u);
}

static void random_ops(gsx *f, de_gpu_backend b, bool scatter, u32 frames) {
    random_ops_ex(f, b, scatter, false, frames);
}

/* DENG-421: the engine's built-in kernels (embedded at build time, or cooked through the Dev
 * cache), so Release and Shipping builds run the compute scatter too. */
TEST_F(gpu_scene, random_ops_readback_metal_builtin, gsx) {
    random_ops_ex(f, DE_GPU_METAL, true, true, 300);
}

TEST_F(gpu_scene, random_ops_readback_vulkan_builtin, gsx) {
    random_ops_ex(f, DE_GPU_VULKAN, true, true, 300);
}

TEST_F(gpu_scene, random_ops_readback_d3d12_builtin, gsx) {
    random_ops_ex(f, DE_GPU_D3D12, true, true, 300);
}

TEST_F(gpu_scene, random_ops_readback_null, gsx) {
    random_ops(f, DE_GPU_NULL, false, 1000);
}

TEST_F(gpu_scene, random_ops_readback_null_scatter, gsx) {
    random_ops(f, DE_GPU_NULL, true, 1000);
}

TEST_F(gpu_scene, random_ops_readback_metal, gsx) {
    random_ops(f, DE_GPU_METAL, true, 1000);
}

TEST_F(gpu_scene, random_ops_readback_vulkan, gsx) {
    random_ops(f, DE_GPU_VULKAN, true, 1000);
}

TEST_F(gpu_scene, random_ops_readback_d3d12, gsx) {
    random_ops(f, DE_GPU_D3D12, true, 1000);
}

/* ---------- growth_preserves: capacity doubling keeps every entry ---------- */

TEST_F(gpu_scene, growth_preserves, gsx) {
    gsx_open(f, &(open_opts){.backend = DE_GPU_NULL,
                             .instances = 400,
                             .lights = 8,
                             .initial_capacity = {16, 2, 2, 4, 1}});
    populate(f, 16, 2);
    ASSERT_TRUE(gsx_frame(f));
    const de__gpu_scene_table_info before = de__gpu_scene_table(f->gs, DE__GS_INSTANCES);
    EXPECT_EQ(before.capacity, 16u);
    const de__gpu_scene_stats *st = de__gpu_scene_stats_get(f->gs);
    const u32 growths = st->growths;

    /* 100 more: the instance table doubles 16 -> 128 in one growth; old entries come from the
     * old buffer on the GPU (only the new ones are staged). */
    populate(f, 100, 0);
    ASSERT_TRUE(gsx_frame(f));
    const de__gpu_scene_table_info after = de__gpu_scene_table(f->gs, DE__GS_INSTANCES);
    EXPECT_EQ(after.capacity, 128u);
    EXPECT_EQ(after.count, 116u);
    EXPECT_NE(after.buffer.id, before.buffer.id);
    EXPECT_NE(after.bindless, before.bindless);
    EXPECT_EQ(st->frame_entries[DE__GS_INSTANCES], 100u);
    EXPECT_EQ(st->frame_passes, 2u); /* grow + upload */
    EXPECT_GT(st->growths, growths);
    EXPECT_EQ(st->frame_entries[DE__GS_VIEWS], 1u); /* the table's new bindless index */
    /* The next frame copies nothing and keeps the new buffer. */
    static_frame(f);
    EXPECT_EQ(st->frame_bytes, 0u);
    EXPECT_EQ(de__gpu_scene_table(f->gs, DE__GS_INSTANCES).buffer.id, after.buffer.id);
    EXPECT_EQ(f->checked, f->frames);
}

/* ---------- budget_spreads_frames: a frame stages at most its budget; the rest follows ----------
 */

TEST_F(gpu_scene, budget_spreads_frames, gsx) {
    const u64 budget = 100u * sizeof(de__gs_instance);
    gsx_open(f, &(open_opts){.backend = DE_GPU_NULL,
                             .instances = 1000,
                             .lights = 4,
                             .staging_bytes = 256u * 1024u,
                             .budget_bytes = budget});
    populate(f, 1000, 4);
    f->check = false; /* incomplete until the last part is uploaded */
    ASSERT_TRUE(gsx_frame(f));
    const de__gpu_scene_stats *st = de__gpu_scene_stats_get(f->gs);
    EXPECT_LE(st->frame_bytes, budget);
    EXPECT_EQ(st->deferred_frames, 1u);
    u32 frames = 1;
    u64 instances = st->frame_entries[DE__GS_INSTANCES];
    while (st->frame_bytes != 0 && frames < 40) {
        static_frame(f);
        EXPECT_LE(st->frame_bytes, budget);
        instances += st->frame_entries[DE__GS_INSTANCES];
        ++frames;
    }
    EXPECT_EQ(instances, 1000u); /* every entry exactly once */
    EXPECT_GE(frames, 10u);
    EXPECT_LE(frames, 13u);
    f->check = true;
    static_frame(f);
    EXPECT_EQ(st->frame_bytes, 0u);
    EXPECT_EQ(f->checked, 1u);
}

/* ---------- slot_reuse: freed slots are reused, last freed first ---------- */

TEST_F(gpu_scene, slot_reuse, gsx) {
    gsx_open(f, &(open_opts){.backend = DE_GPU_NULL, .instances = 64, .lights = 4});
    frw *w = &f->w;
    frw_begin(w);
    u32 s[6];
    for (u32 i = 0; i < 6; ++i) {
        s[i] = frw_add_inst(w);
        w->f.instances.mesh[s[i]].id = 1u + i / 2u; /* meshes 1, 1, 2, 2, 3, 3 */
        memset(&w->f.instances.materials[(size_t)s[i] * DE__RW_MATERIALS], 0,
               sizeof(de_material) * DE__RW_MATERIALS);
        w->f.instances.materials[(size_t)s[i] * DE__RW_MATERIALS].id = 1u + i / 3u;
    }
    frw_end(w);
    ASSERT_TRUE(gsx_frame(f));
    const u32 m1 = de__gpu_scene_mesh_slot(f->gs, (de_mesh){1});
    const u32 m2 = de__gpu_scene_mesh_slot(f->gs, (de_mesh){2});
    const u32 m3 = de__gpu_scene_mesh_slot(f->gs, (de_mesh){3});
    EXPECT_EQ(m1, 0u); /* slots in first-use order */
    EXPECT_EQ(m2, 1u);
    EXPECT_EQ(m3, 2u);

    /* Mesh 2 loses its last instances: its slot is freed (and zeroed on the GPU). */
    frw_begin(w);
    frw_remove_inst(w, s[2]);
    frw_remove_inst(w, s[3]);
    frw_end(w);
    ASSERT_TRUE(gsx_frame(f));
    EXPECT_EQ(de__gpu_scene_mesh_slot(f->gs, (de_mesh){2}), DE__GS_NONE);
    EXPECT_EQ(de__gpu_scene_stats_get(f->gs)->live_meshes, 2u);
    {
        de__gs_mesh e;
        memcpy(&e, f->mirror[DE__GS_MESHES] + (size_t)m2 * sizeof e, sizeof e);
        EXPECT_EQ(e.flags, 0u);
    }
    /* A new mesh takes the freed slot; the render world reuses the instance slots (LIFO). */
    frw_begin(w);
    const u32 a = frw_add_inst(w);
    const u32 b = frw_add_inst(w);
    EXPECT_EQ(a, s[3]);
    EXPECT_EQ(b, s[2]);
    w->f.instances.mesh[a].id = 5;
    w->f.instances.mesh[b].id = 1;
    frw_end(w);
    ASSERT_TRUE(gsx_frame(f));
    EXPECT_EQ(de__gpu_scene_mesh_slot(f->gs, (de_mesh){5}), m2);
    EXPECT_EQ(de__gpu_scene_table(f->gs, DE__GS_MESHES).count, 3u);
    /* Two freed at once come back last-freed first. */
    frw_begin(w);
    for (u32 i = 0; i < 6; ++i)
        if (w->f.instances.entity[i] != 0 && (w->f.instances.mesh[i].id == 3u))
            frw_remove_inst(w, i);
    frw_end(w);
    ASSERT_TRUE(gsx_frame(f));
    EXPECT_EQ(de__gpu_scene_mesh_slot(f->gs, (de_mesh){3}), DE__GS_NONE);
    frw_begin(w);
    for (u32 i = 0; i < w->f.instances.count; ++i)
        if (w->f.instances.entity[i] != 0 && w->f.instances.mesh[i].id == 5u)
            frw_remove_inst(w, i);
    frw_end(w);
    ASSERT_TRUE(gsx_frame(f));
    frw_begin(w);
    const u32 c = frw_add_inst(w);
    w->f.instances.mesh[c].id = 7;
    const u32 d = frw_add_inst(w);
    w->f.instances.mesh[d].id = 8;
    frw_end(w);
    ASSERT_TRUE(gsx_frame(f));
    EXPECT_EQ(de__gpu_scene_mesh_slot(f->gs, (de_mesh){7}), m2); /* freed last */
    EXPECT_EQ(de__gpu_scene_mesh_slot(f->gs, (de_mesh){8}), m3);
    EXPECT_EQ(de__gpu_scene_table(f->gs, DE__GS_MESHES).count, 3u);
    /* Materials: 1 and 2 in slots 0 and 1. */
    EXPECT_EQ(de__gpu_scene_material_slot(f->gs, (de_material){1}), 0u);
    EXPECT_EQ(f->checked, f->frames);
}

/* ---------- origin_rebase_full_upload ---------- */

TEST_F(gpu_scene, origin_rebase_full_upload, gsx) {
    gsx_open(f, &(open_opts){.backend = DE_GPU_NULL, .instances = 3000, .lights = 20});
    populate(f, 3000, 20);
    ASSERT_TRUE(gsx_frame(f));
    static_frame(f);
    const de__gpu_scene_stats *st = de__gpu_scene_stats_get(f->gs);
    ASSERT_EQ(st->frame_bytes, 0u);
    /* A rebase: every instance and light once (one run each), plus the view (new origin). */
    frw_begin(&f->w);
    f->w.f.origin.y -= 1024.0;
    f->w.f.origin_changed = true;
    frw_move_inst(&f->w, 5); /* also changed: still uploaded once */
    frw_end(&f->w);
    ASSERT_TRUE(gsx_frame(f));
    EXPECT_EQ(st->frame_entries[DE__GS_INSTANCES], 3000u);
    EXPECT_EQ(st->frame_entries[DE__GS_LIGHTS], 20u);
    EXPECT_EQ(st->frame_entries[DE__GS_VIEWS], 1u);
    EXPECT_EQ(st->frame_bytes,
              3000u * sizeof(de__gs_instance) + 20u * sizeof(de__gs_light) + sizeof(de__gs_view));
    EXPECT_EQ(st->frame_copies, 3u);
    static_frame(f); /* the view's previous matrix catches up */
    static_frame(f);
    EXPECT_EQ(st->frame_bytes, 0u);
    EXPECT_EQ(f->checked, f->frames);
}

/* ---------- view_constants: previous view-projection, rebased onto the new origin ---------- */

static void view_vp(const de__gs_view *v, const de__gs_float4 rows[4], de_mat4 *out) {
    (void)v;
    for (u32 r = 0; r < 4; ++r)
        for (u32 c = 0; c < 4; ++c)
            out->m[c * 4u + r] = rows[r].v[c];
}

TEST_F(gpu_scene, view_constants, gsx) {
    gsx_open(f, &(open_opts){.backend = DE_GPU_NULL, .instances = 8, .lights = 2});
    populate(f, 4, 1);
    ASSERT_TRUE(gsx_frame(f));
    de__gs_view v;
    memcpy(&v, f->mirror[DE__GS_VIEWS], sizeof v);
    de_mat4 vp, prev;
    view_vp(&v, v.view_proj, &vp);
    view_vp(&v, v.prev_view_proj, &prev);
    EXPECT_MEMEQ(&vp, &prev, sizeof vp); /* first frame: prev = current */
    EXPECT_EQ(v.instances, de__gpu_scene_table(f->gs, DE__GS_INSTANCES).bindless);
    EXPECT_EQ(v.instance_count, 4u);
    EXPECT_EQ(v.light_count, 1u);
    EXPECT_TRUE(v.viewport.v[0] == 1280.0f);
    EXPECT_FLOAT_EQ(v.origin_high.v[0] + v.origin_low.v[0], 1024.0f, 0.0f);
    const de_mat4 old_vp = vp;

    /* The camera moves: prev = last frame's. */
    f->view.view.m[12] += 2.0f;
    static_frame(f);
    memcpy(&v, f->mirror[DE__GS_VIEWS], sizeof v);
    view_vp(&v, v.prev_view_proj, &prev);
    EXPECT_MEMEQ(&prev, &old_vp, sizeof prev);

    /* The origin moves by +1024 m in x (the camera does not): last frame's matrix is rebased, so
     * a point fixed in the world projects the same. */
    view_vp(&v, v.view_proj, &vp);
    const de_vec4 p_old = {3.0f, 4.0f, 1.0f, 1.0f}; /* relative to the old origin */
    const de_vec4 want = de_m4_mul_v4(&vp, p_old);
    frw_begin(&f->w);
    f->w.f.origin.x += 1024.0;
    f->w.f.origin_changed = true;
    frw_end(&f->w);
    f->view.view.m[12] -= 1024.0f * f->view.view.m[0]; /* the camera's matrix, rebased too */
    f->view.view.m[13] -= 1024.0f * f->view.view.m[1];
    f->view.view.m[14] -= 1024.0f * f->view.view.m[2];
    ASSERT_TRUE(gsx_frame(f));
    memcpy(&v, f->mirror[DE__GS_VIEWS], sizeof v);
    view_vp(&v, v.prev_view_proj, &prev);
    const de_vec4 p_new = {3.0f - 1024.0f, 4.0f, 1.0f, 1.0f};
    const de_vec4 got = de_m4_mul_v4(&prev, p_new);
    EXPECT_FLOAT_EQ(got.x, want.x, 1e-3f);
    EXPECT_FLOAT_EQ(got.y, want.y, 1e-3f);
    EXPECT_FLOAT_EQ(got.w, want.w, 1e-3f);
    EXPECT_FLOAT_EQ(v.origin_high.v[0] + v.origin_low.v[0], 2048.0f, 0.0f);
    /* Removing the view zeroes its entry. */
    de__gpu_scene_remove_view(f->gs, 0);
    f->set_view = false;
    static_frame(f);
    memcpy(&v, f->mirror[DE__GS_VIEWS], sizeof v);
    EXPECT_EQ(v.instances, 0u);
    EXPECT_TRUE(v.viewport.v[0] == 0.0f);
}

/* ---------- renderer_owns_scene: de_renderer_create makes the scene ---------- */

TEST(gpu_scene, renderer_owns_scene) {
    de_tracking_allocator track;
    de_allocator a = de_tracking_allocator_init(&track, NULL, DE_MEM_RENDER);
    de_gpu_device *d = NULL;
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &d), DE_OK);
    de_renderer *r = NULL;
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = d, .allocator = &a}, &r), DE_OK);
    ASSERT_TRUE(r->gpu_scene != NULL);
    const de__gpu_scene_table_info ti = de__gpu_scene_table(r->gpu_scene, DE__GS_INSTANCES);
    EXPECT_TRUE(DE_HANDLE_VALID(ti.buffer));
    EXPECT_NE(ti.bindless, 0u);
    EXPECT_EQ(ti.stride, (u32)sizeof(de__gs_instance));
    de_renderer_destroy(r);
    de_gpu_device_destroy(d);
    EXPECT_EQ(de_tracking_allocator_stats(&track).blocks_live, 0u);
}

/* ---------- renderer_frames: the renderer drives the scene (DENG-176 views) ---------- */

/* The real loop on the null backend, unpipelined: ECS camera and mesh renderers, Extract, the
 * kick; the renderer's frame runs the scene's uploads, and the view stage its scene constants. */
TEST(gpu_scene, renderer_frames) {
    de_tracking_allocator track;
    de_allocator a = de_tracking_allocator_init(&track, NULL, DE_MEM_RENDER);
    de_fp_env_init_thread();
    de_type_registry *types = NULL;
    de_world *w = NULL;
    de_gpu_device *d = NULL;
    de_renderer *r = NULL;
    ASSERT_EQ(de_type_registry_create(&a, &types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &a, .types = types}, &w), DE_OK);
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &d), DE_OK);
    ASSERT_EQ(
        de_renderer_create(&(de_renderer_desc){.device = d, .types = types, .allocator = &a}, &r),
        DE_OK);
    de_renderer_attach(r, w);
    const de_cvar pipelined = de_cvar_find("r.pipelined");
    ASSERT_TRUE(DE_HANDLE_VALID(pipelined));
    (void)de_cvar_set(pipelined, "0");
    const DeRenderComponents ids = *de__render_components(w);
    const de_builtin_component_ids bi = de_builtin_components(w);

    de_entity cam = DE_ENTITY_NULL;
    const de_component cc[2] = {bi.transform, ids.camera};
    ASSERT_EQ(de_entity_spawn_with(w, cc, 2, &cam), DE_OK);
    DeTransform *ct = (DeTransform *)de_entity_get(w, cam, bi.transform);
    ct->position = (de_dvec3){0, -10, 2};
    ct->rotation = (de_quat){0, 0, 0, 1};
    ct->scale = (de_vec3){1, 1, 1};
    de_entity mesh[500];
    const de_component mc[2] = {bi.transform, ids.mesh_renderer};
    for (u32 i = 0; i < 500; ++i) {
        ASSERT_EQ(de_entity_spawn_with(w, mc, 2, &mesh[i]), DE_OK);
        DeTransform *t = (DeTransform *)de_entity_get(w, mesh[i], bi.transform);
        t->position = (de_dvec3){(f64)(i % 25), (f64)(i / 25), 0};
        t->rotation = (de_quat){0, 0, 0, 1};
        t->scale = (de_vec3){1, 1, 1};
        DeMeshRenderer *m = (DeMeshRenderer *)de_entity_get(w, mesh[i], ids.mesh_renderer);
        m->mesh.id = 1u + i % 5u;
        m->materials[0].id = 100u + i % 3u;
    }
#define RUN_FRAME()                                                                                \
    do {                                                                                           \
        ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_POST_UPDATE, 1.0f / 60.0f, 0.0f), DE_OK);   \
        ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_EXTRACT, 1.0f / 60.0f, 0.0f), DE_OK);       \
        ASSERT_EQ(de__render_extract_and_kick(r, w), DE_OK);                                       \
    } while (0)
    RUN_FRAME();
    const de_render_stats *rs = de_renderer_stats(r);
    const de__gpu_scene_stats *st = de__gpu_scene_stats_get(r->gpu_scene);
    EXPECT_EQ(rs->gpu_scene_instances, 500u);
    EXPECT_EQ(rs->gpu_scene_meshes, 5u);
    EXPECT_EQ(rs->gpu_scene_materials, 3u);
    EXPECT_EQ(st->frame_entries[DE__GS_INSTANCES], 500u);
    EXPECT_EQ(st->frame_entries[DE__GS_VIEWS], 1u); /* the main view's constants */
    EXPECT_GE(rs->gpu_scene_upload_bytes, 500u * sizeof(de__gs_instance) + sizeof(de__gs_view));
    EXPECT_TRUE(de__gpu_scene_has_scatter(r->gpu_scene)); /* the null backend's placeholder */
    /* Static frames: nothing (after the view's previous matrix caught up). */
    RUN_FRAME();
    for (u32 i = 0; i < 5; ++i) {
        RUN_FRAME();
        EXPECT_EQ(rs->gpu_scene_upload_bytes, 0u);
    }
    /* One instance moves: its entry, and the next frame its previous transform. */
    ((DeTransform *)de_entity_get(w, mesh[123], bi.transform))->position.z = 5.0;
    RUN_FRAME();
    EXPECT_EQ(st->frame_entries[DE__GS_INSTANCES], 1u);
    EXPECT_EQ(st->frame_entries[DE__GS_VIEWS], 0u);
    RUN_FRAME();
    EXPECT_EQ(st->frame_entries[DE__GS_INSTANCES], 1u);
    RUN_FRAME();
    EXPECT_EQ(rs->gpu_scene_upload_bytes, 0u);
    /* The camera moves: the view's constants, in the view's graph. */
    ((DeTransform *)de_entity_get(w, cam, bi.transform))->position.x = 1.0;
    RUN_FRAME();
    EXPECT_EQ(st->frame_entries[DE__GS_VIEWS], 1u);
    EXPECT_EQ(st->frame_entries[DE__GS_INSTANCES], 0u);
    EXPECT_EQ(rs->gpu_scene_upload_bytes, sizeof(de__gs_view));
#undef RUN_FRAME
    (void)de_cvar_set(pipelined, "1");
    de_renderer_destroy(r);
    de_world_destroy(w);
    de_gpu_device_destroy(d);
    de_type_registry_destroy(types);
    EXPECT_EQ(de_tracking_allocator_stats(&track).blocks_live, 0u);
}

/* ---------- no_heap_after_warmup ---------- */

TEST_F(gpu_scene, no_heap_after_warmup, gsx) {
    gsx_open(
        f,
        &(open_opts){
            .backend = DE_GPU_NULL, .instances = 4096, .lights = 64, .scatter = true, .seed = 99});
    f->check = false;
    populate(f, 2000, 32);
    ASSERT_TRUE(gsx_frame(f));
    /* Warm-up: churn so every array reaches its working size. */
    for (u32 i = 0; i < 60; ++i)
        ASSERT_TRUE(random_frame(f, 40, true));
    const u32 instances = f->w.f.instances.count;
    const de_tracking_stats before = de_tracking_allocator_stats(&f->track);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    bool ok = true;
    for (u32 i = 0; i < 100 && ok; ++i) {
        /* Moves and modifications, removals and re-adds that reuse slots: no growth. */
        frw *w = &f->w;
        frw_begin(w);
        for (u32 k = 0; k < 30; ++k) {
            const u32 s = live_inst(w);
            if (s == DE__RW_NONE)
                continue;
            if (k % 3u == 0)
                frw_move_inst(w, s);
            else if (k % 3u == 1)
                frw_modify_inst(w, s);
            else
                frw_remove_inst(w, s);
        }
        while (w->ifree_n != 0 && w->f.instances.count == instances)
            (void)frw_add_inst(w);
        if (i % 10u == 5u) {
            w->f.origin.z += 1024.0;
            w->f.origin_changed = true;
        }
        f->view.view.m[12] += 0.5f;
        frw_end(w);
        ok = gsx_frame(f);
    }
    de_alloc_guard_end(&guard);
    ASSERT_TRUE(ok);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    const de_tracking_stats after = de_tracking_allocator_stats(&f->track);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_GT(de__gpu_scene_stats_get(f->gs)->total_bytes, 0u);
}
