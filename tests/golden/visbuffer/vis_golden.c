/* vis_golden.c — the multi-frame helper of the visibility-buffer golden scenes (DENG-240);
 * vis_golden.h describes it. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "vis_golden.h"

#include <dengine/de_debug.h>
#include <dengine/de_platform.h>

#include "render/formats/de_cooked_material.h"
#include "render/formats/de_cooked_mesh.h"

#if DE_GOLDEN_COOK
#include "cook/importers/model/model_internal.h"
#endif

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VG_WAIT_NS      (60ull * 1000000000ull)
#define VG_STREAM_ALIGN 256u /* the mesh loader's stream alignment (mesh_loader.c) */
#define VG_CHECK        8u   /* the cutout checker: 8 x 8 texels */
#define VG_TEX_CHECKER  1u   /* its fake texture handle */

static void *vg_zalloc(size_t n) {
    void *p = calloc(1, n > 0 ? n : 1);
    if (p == NULL) {
        fprintf(stderr, "vis golden: out of memory\n");
        abort();
    }
    return p;
}

static u64 vg_align(u64 v, u64 a) {
    return (v + a - 1u) / a * a;
}

static void vg_cvar(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (DE_HANDLE_VALID(cv))
        (void)de_cvar_set(cv, value);
    de_error_clear(); /* Dev-only cvars are absent in Shipping */
}

static const char *vg_err(void) {
    const de_error *e = de_error_last();
    return e != NULL ? e->message : "?";
}

/* ================================================================================================
 * The GPU scene's resolver (the fake asset records)
 * ============================================================================================== */

static const de_render_mesh *vg_res_mesh(void *user, de_mesh m) {
    const vg *v = (const vg *)user;
    return m.id >= 1u && m.id <= v->mesh_count ? &v->mesh[m.id - 1u].rm : NULL;
}
static const de_render_material *vg_res_material(void *user, de_material m) {
    const vg *v = (const vg *)user;
    return m.id >= 1u && m.id <= v->mat_count ? &v->mat[m.id - 1u] : NULL;
}
static u32 vg_res_texture(void *user, de_texture t) {
    const vg *v = (const vg *)user;
    return t.id == VG_TEX_CHECKER && v->checker_bindless != 0 ? v->checker_bindless
                                                              : DE_GPU_BINDLESS_INVALID;
}

/* ================================================================================================
 * Open / close
 * ============================================================================================== */

bool vg_open(vg *v, de_golden_ctx *ctx, u32 instances) {
    memset(v, 0, sizeof *v);
    v->ctx = ctx;
    v->dev = ctx->device;
    v->width = ctx->width;
    v->height = ctx->height;
    v->fov = 1.0f;
#if !DE_GOLDEN_COOK
    de_golden_skip(ctx, "this build has no cooker: the scenes cook their meshes (S11 importers)");
    return false;
#else
    if (!ctx->caps->base_tier_ok) {
        de_golden_skip(ctx, "below the Base tier (the vis frame needs it): %s",
                       ctx->caps->base_tier_reason);
        return false;
    }
    if (de_arena_create(&(de_arena_desc){.name = "vis_golden"}, &v->arena) != DE_OK) {
        de_golden_fail(ctx, "arena: %s", vg_err());
        return false;
    }
    for (u32 i = 0; i < VG_VARIANTS; ++i)
        if (de__rg_create(&(de__rg_desc){.device = v->dev, .name = i == 0 ? "vg_on" : "vg_off"},
                          &v->graph[i]) != DE_OK) {
            de_golden_fail(ctx, "render graph: %s", vg_err());
            return false;
        }
    v->fence = de_gpu_fence_create(v->dev, 0);
    de__gpu_scene_desc gd = {.device = v->dev,
                             .resolver = {.mesh = vg_res_mesh,
                                          .material = vg_res_material,
                                          .texture_bindless = vg_res_texture,
                                          .user = v},
                             .staging_bytes = (u64)32 << 20};
    gd.initial_capacity[DE__GS_INSTANCES] = instances;
    if (!DE_HANDLE_VALID(v->fence) || de__gpu_scene_create(&gd, &v->gs) != DE_OK ||
        de__vis_shared_create(&(de__vis_shared_desc){.device = v->dev}, &v->vis) != DE_OK) {
        de_golden_fail(ctx, "the GPU scene or the vis frame: %s", vg_err());
        return false;
    }
    if (!v->vis->enabled) {
        de_golden_skip(ctx, "the vis frame is off in this build: %s", v->vis->why);
        return false;
    }
    /* The fake render world. */
    v->capacity = instances;
    de__rw_instances *in = &v->world.instances;
    in->entity = (u64 *)vg_zalloc(instances * sizeof(u64));
    in->mesh = (de_mesh *)vg_zalloc(instances * sizeof(de_mesh));
    in->materials =
        (de_material *)vg_zalloc((size_t)instances * DE__RW_MATERIALS * sizeof(de_material));
    in->transform = (de_affine *)vg_zalloc(instances * sizeof(de_affine));
    in->prev_transform = (de_affine *)vg_zalloc(instances * sizeof(de_affine));
    in->bounds = (de_aabb *)vg_zalloc(instances * sizeof(de_aabb));
    in->layer_mask = (u32 *)vg_zalloc(instances * sizeof(u32));
    in->flags = (u8 *)vg_zalloc(instances);
    v->world.dirty_instances = (u32 *)vg_zalloc(instances * sizeof(u32));
    v->world.removed_instances = (u32 *)vg_zalloc(sizeof(u32));
    v->world.dirty_lights = (u32 *)vg_zalloc(sizeof(u32));
    v->world.removed_lights = (u32 *)vg_zalloc(sizeof(u32));
    /* Vulkan: one command list per submission. CI's VVL 1.3.275 checks bindless accesses against
     * the layout at the end of each list (DENG-235, DENG-238). */
    if (ctx->backend == DE_GPU_VULKAN)
        vg_cvar("r.rg_record_cost", "1048576");
    vg_cvar("r.cull_freeze", "0");
    v->can_cull_off = DE_HANDLE_VALID(de_cvar_find("r.cull_disable"));
    de_error_clear();
    return true;
#endif
}

bool vg_skip_without_cull_off(vg *v) {
    if (v->can_cull_off)
        return false;
    de_golden_skip(v->ctx, "this build cannot turn culling off (r.cull_disable is DEV_ONLY; "
                           "Shipping drops it), so culling on == off cannot be checked");
    return true;
}

static void vg_image_free(vg_image *img) {
    free(img->key);
    free(img->gb0);
    free(img->gb1);
    free(img->gb2);
    free(img->mot);
    memset(img, 0, sizeof *img);
}

void vg_close(vg *v) {
    if (v->dev != NULL)
        de_gpu_wait_idle(v->dev);
    for (u32 i = 0; i < VG_VARIANTS; ++i) {
        if (v->graph[i] != NULL) {
            de__rg_reset(v->graph[i]);
            de__rg_destroy(v->graph[i]);
        }
        vg_image_free(&v->img[i]);
    }
    if (v->dev != NULL)
        de_gpu_wait_idle(v->dev);
    de__vis_shared_destroy(v->vis);
    de__gpu_scene_destroy(v->gs);
    for (u32 m = 0; m < v->mesh_count; ++m)
        for (u32 b = 0; b < DE_RENDER_MESH_BUFFER_COUNT; ++b)
            if (DE_HANDLE_VALID(v->mesh[m].buf[b]))
                de_gpu_buffer_destroy(v->dev, v->mesh[m].buf[b]);
    if (v->dev != NULL) {
        for (u32 i = 0; i < VG_VARIANTS; ++i)
            if (DE_HANDLE_VALID(v->rb[i]))
                de_gpu_buffer_destroy(v->dev, v->rb[i]);
        if (DE_HANDLE_VALID(v->checker))
            de_gpu_texture_destroy(v->dev, v->checker);
        if (DE_HANDLE_VALID(v->checker_up))
            de_gpu_buffer_destroy(v->dev, v->checker_up);
        if (DE_HANDLE_VALID(v->fence))
            de_gpu_fence_destroy(v->dev, v->fence);
        de_gpu_wait_idle(v->dev);
    }
    de__rw_instances *in = &v->world.instances;
    free(in->entity);
    free(in->mesh);
    free(in->materials);
    free(in->transform);
    free(in->prev_transform);
    free(in->bounds);
    free(in->layer_mask);
    free(in->flags);
    free(v->world.dirty_instances);
    free(v->world.removed_instances);
    free(v->world.dirty_lights);
    free(v->world.removed_lights);
    if (v->arena != NULL)
        de_arena_destroy(v->arena);
    vg_cvar("r.rg_record_cost", "32");
    vg_cvar("r.raster_path", "auto");
    vg_cvar("r.cull_disable", "0");
    vg_cvar("r.occlusion", "1");
    memset(v, 0, sizeof *v);
}

/* ================================================================================================
 * Procedural meshes: glTF 2.0 in memory, cooked by the S11 model importer
 * ============================================================================================== */

typedef struct geo {
    f32 *pos, *nrm, *uv; /* engine space (Z up) */
    u32 *idx;
    u32 vc, ic, vcap, icap;
} geo;

static u32 geo_vert(geo *g, de_vec3 p, de_vec3 n, f32 u, f32 t) {
    if (g->vc == g->vcap) {
        g->vcap = g->vcap ? g->vcap * 2u : 256u;
        g->pos = (f32 *)realloc(g->pos, (size_t)g->vcap * 12u);
        g->nrm = (f32 *)realloc(g->nrm, (size_t)g->vcap * 12u);
        g->uv = (f32 *)realloc(g->uv, (size_t)g->vcap * 8u);
        if (g->pos == NULL || g->nrm == NULL || g->uv == NULL)
            abort();
    }
    const u32 i = g->vc++;
    g->pos[3 * i] = p.x, g->pos[3 * i + 1] = p.y, g->pos[3 * i + 2] = p.z;
    g->nrm[3 * i] = n.x, g->nrm[3 * i + 1] = n.y, g->nrm[3 * i + 2] = n.z;
    g->uv[2 * i] = u, g->uv[2 * i + 1] = t;
    return i;
}

static void geo_tri(geo *g, u32 a, u32 b, u32 c) {
    if (g->ic + 3u > g->icap) {
        g->icap = g->icap ? g->icap * 2u : 768u;
        g->idx = (u32 *)realloc(g->idx, (size_t)g->icap * 4u);
        if (g->idx == NULL)
            abort();
    }
    g->idx[g->ic++] = a;
    g->idx[g->ic++] = b;
    g->idx[g->ic++] = c;
}

/* A grid of n x n quads from corner `o` along `a` and `b` (normal a x b), UV0 over [0, uv]. */
static void geo_grid(geo *g, de_vec3 o, de_vec3 a, de_vec3 b, u32 n, f32 uv) {
    const de_vec3 nrm = de_v3_normalize(de_v3_cross(a, b));
    const u32 base = g->vc;
    for (u32 j = 0; j <= n; ++j)
        for (u32 i = 0; i <= n; ++i) {
            const f32 s = (f32)i / (f32)n, t = (f32)j / (f32)n;
            const de_vec3 p = de_v3_add(o, de_v3_add(de_v3_scale(a, s), de_v3_scale(b, t)));
            (void)geo_vert(g, p, nrm, s * uv, t * uv);
        }
    for (u32 j = 0; j < n; ++j)
        for (u32 i = 0; i < n; ++i) {
            const u32 v00 = base + j * (n + 1u) + i, v10 = v00 + 1u;
            const u32 v01 = v00 + n + 1u, v11 = v01 + 1u;
            geo_tri(g, v00, v10, v11);
            geo_tri(g, v00, v11, v01);
        }
}

static void geo_free(geo *g) {
    free(g->pos);
    free(g->nrm);
    free(g->uv);
    free(g->idx);
    memset(g, 0, sizeof *g);
}

/* ---------- glTF JSON with a base64 data-URI buffer ---------- */

typedef struct sbuf {
    char *s;
    size_t n, cap;
} sbuf;

static void sb_put(sbuf *b, const char *p, size_t n) {
    if (b->n + n + 1u > b->cap) {
        while (b->n + n + 1u > b->cap)
            b->cap = b->cap ? b->cap * 2u : 4096u;
        b->s = (char *)realloc(b->s, b->cap);
        if (b->s == NULL)
            abort();
    }
    memcpy(b->s + b->n, p, n);
    b->n += n;
    b->s[b->n] = '\0';
}

static void sb_printf(sbuf *b, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
static void sb_printf(sbuf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k > 0)
        sb_put(b, tmp, (size_t)k < sizeof tmp ? (size_t)k : sizeof tmp - 1u);
}

static void sb_base64(sbuf *b, const u8 *p, size_t n) {
    static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char q[4];
    for (size_t i = 0; i < n; i += 3) {
        const u32 v = ((u32)p[i] << 16) | (i + 1 < n ? (u32)p[i + 1] << 8 : 0u) |
                      (i + 2 < n ? (u32)p[i + 2] : 0u);
        q[0] = k[(v >> 18) & 63u];
        q[1] = k[(v >> 12) & 63u];
        q[2] = i + 1 < n ? k[(v >> 6) & 63u] : '=';
        q[3] = i + 2 < n ? k[v & 63u] : '=';
        sb_put(b, q, 4);
    }
}

/* glTF space is Y up: (X, Y, Z) engine = (x, -z, y) glTF, so glTF = (X, Z, -Y). */
static void to_gltf(const f32 *e, f32 *o) {
    o[0] = e[0];
    o[1] = e[2];
    o[2] = -e[1];
}

static char *geo_gltf(const geo *g, size_t *len) {
    const size_t pb = (size_t)g->vc * 12u, nb = pb, ub = (size_t)g->vc * 8u;
    const size_t ib = (size_t)g->ic * 4u, total = pb + nb + ub + ib;
    u8 *bin = (u8 *)vg_zalloc(total);
    f32 mn[3] = {INFINITY, INFINITY, INFINITY}, mx[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (u32 i = 0; i < g->vc; ++i) {
        f32 p[3], n[3];
        to_gltf(&g->pos[3 * i], p);
        to_gltf(&g->nrm[3 * i], n);
        memcpy(bin + (size_t)i * 12u, p, 12);
        memcpy(bin + pb + (size_t)i * 12u, n, 12);
        for (u32 c = 0; c < 3; ++c) {
            mn[c] = p[c] < mn[c] ? p[c] : mn[c];
            mx[c] = p[c] > mx[c] ? p[c] : mx[c];
        }
    }
    memcpy(bin + pb + nb, g->uv, ub);
    memcpy(bin + pb + nb + ub, g->idx, ib);
    sbuf b = {0};
    sb_printf(&b, "{\"asset\":{\"version\":\"2.0\",\"generator\":\"dengine vis golden\"},");
    sb_printf(&b,
              "\"buffers\":[{\"byteLength\":%zu,\"uri\":\"data:application/octet-stream;base64,",
              total);
    sb_base64(&b, bin, total);
    sb_printf(&b, "\"}],\"bufferViews\":[");
    sb_printf(&b, "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%zu},", pb);
    sb_printf(&b, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},", pb, nb);
    sb_printf(&b, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},", pb + nb, ub);
    sb_printf(&b, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}],", pb + nb + ub, ib);
    sb_printf(&b,
              "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":%u,\"type\":"
              "\"VEC3\",\"min\":[%.9g,%.9g,%.9g],\"max\":[%.9g,%.9g,%.9g]},",
              g->vc, (double)mn[0], (double)mn[1], (double)mn[2], (double)mx[0], (double)mx[1],
              (double)mx[2]);
    sb_printf(&b, "{\"bufferView\":1,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"},",
              g->vc);
    sb_printf(&b, "{\"bufferView\":2,\"componentType\":5126,\"count\":%u,\"type\":\"VEC2\"},",
              g->vc);
    sb_printf(&b, "{\"bufferView\":3,\"componentType\":5125,\"count\":%u,\"type\":\"SCALAR\"}],",
              g->ic);
    sb_printf(&b, "\"meshes\":[{\"name\":\"proc\",\"primitives\":[{\"attributes\":{\"POSITION\":0,"
                  "\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3}]}],");
    sb_printf(&b, "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}");
    free(bin);
    *len = b.n;
    return b.s;
}

/* Places a DMSH v2 blob in GPU buffers (UPLOAD memory, STORAGE: the vis passes read them through
 * read-only bindless views), laid out as src/render/loaders/mesh_loader.c does. */
static bool vg_mesh_from_blob(vg *v, vg_mesh *out, const u8 *p, u64 size) {
    de_cooked_mesh_view mv;
    const char *why = NULL;
    if (!de_cooked_mesh_parse(p, size, &mv, &why)) {
        de_golden_fail(v->ctx, "the cooked mesh does not parse: %s", why);
        return false;
    }
    de_render_mesh *m = &out->rm;
    m->vertex_count = mv.vertex_count;
    m->index_count = mv.index_count;
    m->meshlet_count = mv.meshlet_count;
    m->meshlet_vertex_count = mv.meshlet_vertex_count;
    m->meshlet_triangle_bytes = mv.meshlet_triangle_bytes;
    m->submesh_count = mv.submesh_count;
    m->lod_count = mv.lod_count;
    memcpy(m->bounds_min, mv.bounds_min, sizeof m->bounds_min);
    memcpy(m->bounds_max, mv.bounds_max, sizeof m->bounds_max);
    memcpy(m->sphere, mv.sphere, sizeof m->sphere);
    memcpy(m->position_offset, mv.position_offset, sizeof m->position_offset);
    memcpy(m->position_scale, mv.position_scale, sizeof m->position_scale);
    memcpy(m->uv_offset, mv.uv_offset, sizeof m->uv_offset);
    memcpy(m->uv_scale, mv.uv_scale, sizeof m->uv_scale);
    m->lod_error = mv.lod_error;
    m->max_position_error = mv.max_position_error;
    for (u32 l = 0; l < mv.lod_count; ++l)
        m->lods[l] = de_cooked_mesh_get_lod(&mv, l);
    out->triangles = mv.lod_count > 0 ? m->lods[0].triangle_count : 0u;
    out->bounds.min = (de_vec3){mv.bounds_min[0], mv.bounds_min[1], mv.bounds_min[2]};
    out->bounds.max = (de_vec3){mv.bounds_max[0], mv.bounds_max[1], mv.bounds_max[2]};

    /* The vertex buffer: every stream at a 256-byte boundary. */
    u64 vb = 0;
    for (u32 s = 0; s < DE_COOKED_STREAM_COUNT; ++s) {
        m->stream_offset[s] = UINT32_MAX;
        if (mv.streams[s] == NULL)
            continue;
        vb = vg_align(vb, VG_STREAM_ALIGN);
        m->stream_offset[s] = (u32)vb;
        m->stream_format[s] = mv.stream_format[s];
        vb += (u64)mv.vertex_count * de_cooked_vertex_format_size(mv.stream_format[s]);
    }
    const u8 *src[DE_RENDER_MESH_BUFFER_COUNT] = {NULL};
    u64 bytes[DE_RENDER_MESH_BUFFER_COUNT] = {0};
    bytes[DE_RENDER_MESH_VERTICES] = vb;
    src[DE_RENDER_MESH_MESHLETS] = mv.meshlets;
    bytes[DE_RENDER_MESH_MESHLETS] = (u64)mv.meshlet_count * DE_COOKED_MESHLET_SIZE;
    src[DE_RENDER_MESH_MESHLET_VERTICES] = mv.meshlet_vertices;
    bytes[DE_RENDER_MESH_MESHLET_VERTICES] = (u64)mv.meshlet_vertex_count * 4u;
    src[DE_RENDER_MESH_MESHLET_TRIANGLES] = mv.meshlet_triangles;
    bytes[DE_RENDER_MESH_MESHLET_TRIANGLES] = mv.meshlet_triangle_bytes;
    src[DE_RENDER_MESH_INDICES] = mv.indices;
    bytes[DE_RENDER_MESH_INDICES] = (u64)mv.index_count * 4u;
    for (u32 b = 0; b < DE_RENDER_MESH_BUFFER_COUNT; ++b) {
        if (bytes[b] == 0)
            continue;
        out->buf[b] =
            de_gpu_buffer_create(v->dev, &(de_gpu_buffer_desc){.size = vg_align(bytes[b], 16u),
                                                               .usage = DE_USAGE_STORAGE,
                                                               .memory = DE_MEM_UPLOAD,
                                                               .name = "vis golden mesh"});
        u8 *map =
            DE_HANDLE_VALID(out->buf[b]) ? (u8 *)de_gpu_buffer_map(v->dev, out->buf[b]) : NULL;
        if (map == NULL) {
            de_golden_fail(v->ctx, "mesh buffer: %s", vg_err());
            return false;
        }
        memset(map, 0, (size_t)vg_align(bytes[b], 16u));
        if (b == DE_RENDER_MESH_VERTICES) {
            for (u32 s = 0; s < DE_COOKED_STREAM_COUNT; ++s)
                if (mv.streams[s] != NULL)
                    memcpy(map + m->stream_offset[s], mv.streams[s],
                           (size_t)mv.vertex_count *
                               de_cooked_vertex_format_size(mv.stream_format[s]));
        } else {
            memcpy(map, src[b], (size_t)bytes[b]);
        }
        m->buffers[b] = out->buf[b];
        m->bindless[b] = de_gpu_buffer_bindless(v->dev, out->buf[b]);
        m->buffer_bytes[b] = bytes[b];
    }
    return true;
}

/* Cooks `g` through the glTF front end and the DMSH v2 mesh stage, then places it. */
static u32 vg_cook(vg *v, geo *g) {
    if (v->mesh_count >= VG_MAX_MESHES) {
        de_golden_fail(v->ctx, "more than %u meshes", VG_MAX_MESHES);
        geo_free(g);
        return 0;
    }
#if !DE_GOLDEN_COOK
    geo_free(g);
    return 0;
#else
    size_t len = 0;
    char *json = geo_gltf(g, &len);
    geo_free(g);
    de_model_ir ir;
    de_model_ir_init(&ir, de_heap_allocator(), "vis_golden");
    const de_model_source srcd = {json, (u64)len, "procedural.gltf", NULL, NULL};
    de_result r = de_model_gltf_load(&srcd, &ir);
    u32 id = 0;
    if (r == DE_OK && ir.mesh_count == 1) {
        de_model_ir_convert_axes(&ir, DE_MODEL_AXES_GLTF, 1.0f);
        de_model_ir_compute_bounds(&ir);
        const de_model_mesh_settings ms = {.generate_lods = true,
                                           .meshlets = true,
                                           .lod_error = 0.01f,
                                           .max_position_error = 0.0005f};
        de_model_buf b;
        de_model_buf_init(&b, &ir);
        r = de_model_mesh_cook(&ir, &ir.meshes[0], &ms, NULL, &b);
        if (r == DE_OK && !b.failed) {
            vg_mesh *out = &v->mesh[v->mesh_count];
            memset(out, 0, sizeof *out);
            if (vg_mesh_from_blob(v, out, b.data, b.size))
                id = ++v->mesh_count;
        } else {
            de_golden_fail(v->ctx, "cooking a procedural mesh: %s", vg_err());
        }
    } else {
        de_golden_fail(v->ctx, "the glTF front end: %s", r != DE_OK ? vg_err() : "no mesh");
    }
    de_model_ir_free(&ir);
    free(json);
    return id;
#endif
}

u32 vg_mesh_box(vg *v, de_vec3 h, u32 n) {
    geo g = {0};
    const f32 x = h.x, y = h.y, z = h.z;
    geo_grid(&g, (de_vec3){x, -y, -z}, (de_vec3){0, 2 * y, 0}, (de_vec3){0, 0, 2 * z}, n, 1.0f);
    geo_grid(&g, (de_vec3){-x, -y, -z}, (de_vec3){0, 0, 2 * z}, (de_vec3){0, 2 * y, 0}, n, 1.0f);
    geo_grid(&g, (de_vec3){-x, y, -z}, (de_vec3){0, 0, 2 * z}, (de_vec3){2 * x, 0, 0}, n, 1.0f);
    geo_grid(&g, (de_vec3){-x, -y, -z}, (de_vec3){2 * x, 0, 0}, (de_vec3){0, 0, 2 * z}, n, 1.0f);
    geo_grid(&g, (de_vec3){-x, -y, z}, (de_vec3){2 * x, 0, 0}, (de_vec3){0, 2 * y, 0}, n, 1.0f);
    geo_grid(&g, (de_vec3){-x, -y, -z}, (de_vec3){0, 2 * y, 0}, (de_vec3){2 * x, 0, 0}, n, 1.0f);
    return vg_cook(v, &g);
}

u32 vg_mesh_sphere(vg *v, f32 r, u32 seg) {
    geo g = {0};
    const u32 rings = seg / 2u;
    const f32 pi = 3.14159265358979f;
    for (u32 i = 0; i <= rings; ++i)
        for (u32 j = 0; j <= seg; ++j) {
            const f32 th = pi * (f32)i / (f32)rings, ph = 2.0f * pi * (f32)j / (f32)seg;
            const de_vec3 n = {sinf(th) * cosf(ph), sinf(th) * sinf(ph), cosf(th)};
            (void)geo_vert(&g, de_v3_scale(n, r), n, (f32)j / (f32)seg, (f32)i / (f32)rings);
        }
    for (u32 i = 0; i < rings; ++i)
        for (u32 j = 0; j < seg; ++j) {
            const u32 a = i * (seg + 1u) + j, b = a + seg + 1u;
            if (i > 0)
                geo_tri(&g, a, b, b + 1u);
            if (i + 1u < rings)
                geo_tri(&g, a, b + 1u, a + 1u);
        }
    return vg_cook(v, &g);
}

u32 vg_mesh_plane(vg *v, f32 size, u32 n, f32 uv) {
    geo g = {0};
    geo_grid(&g, (de_vec3){-0.5f * size, -0.5f * size, 0}, (de_vec3){size, 0, 0},
             (de_vec3){0, size, 0}, n, uv);
    return vg_cook(v, &g);
}

u32 vg_mesh_card(vg *v, f32 size, u32 n, f32 uv) {
    geo g = {0};
    geo_grid(&g, (de_vec3){-0.5f * size, 0, -0.5f * size}, (de_vec3){size, 0, 0},
             (de_vec3){0, 0, size}, n, uv);
    return vg_cook(v, &g);
}

/* ================================================================================================
 * Materials
 * ============================================================================================== */

u32 vg_material(vg *v, f32 r, f32 g, f32 b, bool double_sided) {
    if (v->mat_count >= VG_MAX_MATERIALS) {
        de_golden_fail(v->ctx, "more than %u materials", VG_MAX_MATERIALS);
        return 0;
    }
    de_render_material *m = &v->mat[v->mat_count];
    memset(m, 0, sizeof *m);
    m->standard.base_color = (de_vec4){r, g, b, 1.0f};
    m->standard.roughness = 0.5f;
    m->standard.ior = 1.5f;
    m->standard.specular = 1.0f;
    m->standard.double_sided = double_sided;
    m->standard.alpha_cutoff = 0.5f;
    return ++v->mat_count;
}

/* The checker (RGBA8, alpha 255 / 0), uploaded on a submission of its own and left in
 * SHADER_READ. */
static bool vg_checker(vg *v) {
    if (DE_HANDLE_VALID(v->checker))
        return true;
    v->checker = de_gpu_texture_create(
        v->dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                       .width = VG_CHECK,
                                       .height = VG_CHECK,
                                       .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST,
                                       .name = "vis golden checker"});
    v->checker_up = de_gpu_buffer_create(v->dev, &(de_gpu_buffer_desc){.size = 256u * VG_CHECK,
                                                                       .usage = DE_USAGE_COPY_SRC,
                                                                       .memory = DE_MEM_UPLOAD,
                                                                       .name = "checker upload"});
    u8 *up = DE_HANDLE_VALID(v->checker_up) ? (u8 *)de_gpu_buffer_map(v->dev, v->checker_up) : NULL;
    if (!DE_HANDLE_VALID(v->checker) || up == NULL) {
        de_golden_fail(v->ctx, "checker texture: %s", vg_err());
        return false;
    }
    for (u32 y = 0; y < VG_CHECK; ++y)
        for (u32 x = 0; x < VG_CHECK; ++x) {
            u8 *p = up + 256u * y + 4u * x;
            p[0] = 230, p[1] = 200, p[2] = 70;
            p[3] = ((x + y) & 1u) ? 0u : 255u;
        }
    de_gpu_cmd *c = de_gpu_cmd_begin(v->dev, DE_QUEUE_GRAPHICS, "checker upload");
    if (c == NULL) {
        de_golden_fail(v->ctx, "checker upload: %s", vg_err());
        return false;
    }
    de_gpu_barrier b = {
        .texture = v->checker, .before = DE_ACCESS_NONE, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_buffer_to_texture(c, v->checker, 0, 0, v->checker_up, 0, 256u);
    b.before = DE_ACCESS_COPY_DST;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(c, &b, 1);
    const u64 val = ++v->fence_value;
    const de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = v->fence, .signal_value = val};
    if (de_gpu_queue_submit(v->dev, DE_QUEUE_GRAPHICS, &s) != DE_OK ||
        de_gpu_fence_wait(v->dev, v->fence, val, VG_WAIT_NS) != DE_OK) {
        de_golden_fail(v->ctx, "checker upload: %s", vg_err());
        return false;
    }
    v->checker_bindless = de_gpu_texture_bindless(v->dev, v->checker, 0, false);
    return true;
}

u32 vg_material_cutout(vg *v) {
    if (!vg_checker(v))
        return 0;
    const u32 id = vg_material(v, 1.0f, 1.0f, 1.0f, true);
    if (id == 0)
        return 0;
    de_render_material *m = &v->mat[id - 1u];
    m->standard.alpha_mode = DE_COOKED_ALPHA_MASK;
    m->alpha_mode = DE_COOKED_ALPHA_MASK;
    m->standard.base_color_tex = (de_texture){.id = VG_TEX_CHECKER};
    m->textures[DE_COOKED_TEX_BASE_COLOR] = (de_texture){.id = VG_TEX_CHECKER};
    return id;
}

/* ================================================================================================
 * The world
 * ============================================================================================== */

void vg_set(vg *v, u32 s, u32 mesh, de_vec3 pos, f32 yaw, de_vec3 scale, u32 mat) {
    if (s >= v->capacity || mesh == 0 || mesh > v->mesh_count) {
        de_golden_fail(v->ctx, "vg_set: slot %u or mesh %u out of range", s, mesh);
        return;
    }
    de__rw_instances *in = &v->world.instances;
    const de_affine xf =
        de_affine_from_trs(pos, de_quat_axis_angle((de_vec3){0, 0, 1}, yaw), scale);
    const bool known = in->entity[s] != 0;
    in->entity[s] = 1000u + s;
    in->mesh[s].id = mesh;
    in->prev_transform[s] = known ? in->transform[s] : xf;
    in->transform[s] = xf;
    in->layer_mask[s] = 1u;
    in->bounds[s] = de_aabb_transform(v->mesh[mesh - 1u].bounds, &xf);
    in->flags[s] = (u8)DE__RW_INST_HAS_BOUNDS;
    for (u32 k = 0; k < DE__RW_MATERIALS; ++k)
        in->materials[(size_t)s * DE__RW_MATERIALS + k].id = mat;
    if (s >= in->count)
        in->count = s + 1u;
    if (v->world.frame > 0) {
        for (u32 i = 0; i < v->world.dirty_instance_count; ++i)
            if (v->world.dirty_instances[i] == s)
                return;
        v->world.dirty_instances[v->world.dirty_instance_count++] = s;
    }
}

/* ================================================================================================
 * Frames
 * ============================================================================================== */

enum { RB_IDS, RB_GB0, RB_GB1, RB_GB2, RB_MOT, RB_VIS, RB_N };

typedef struct rb_item {
    de_rg_resource r;
    bool texture;
    u32 pitch;
    u64 off, bytes;
} rb_item;

typedef struct rb_pass {
    de_gpu_buffer rb;
    rb_item it[RB_N];
} rb_pass;

static void rb_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rb_pass *p = (const rb_pass *)user;
    for (u32 i = 0; i < RB_N; ++i) {
        const rb_item *it = &p->it[i];
        if (!DE_HANDLE_VALID(it->r))
            continue;
        if (it->texture)
            de_gpu_cmd_copy_texture_to_buffer(cmd, p->rb, it->off, it->pitch,
                                              de_rg_texture(pass, it->r), 0, 0);
        else
            de_gpu_cmd_copy_buffer(cmd, p->rb, it->off, de_rg_buffer(pass, it->r), 0, it->bytes);
    }
}

static void *rows_of(const u8 *m, const rb_item *it, u32 w, u32 h, u32 bpp) {
    u8 *out = (u8 *)vg_zalloc((size_t)w * h * bpp);
    for (u32 y = 0; y < h; ++y)
        memcpy(out + (size_t)y * w * bpp, m + it->off + (u64)y * it->pitch, (size_t)w * bpp);
    return out;
}

/* One variant of the current frame. */
static bool vg_run_variant(vg *v, vg_variant var, bool first, const vg_frame_opts *o, u64 n,
                           const de_mat4 *view, const de_mat4 *proj) {
    de_rg *g = v->graph[var];
    vg_cvar("r.raster_path", o->path != NULL ? o->path : "auto");
    vg_cvar("r.cull_disable", var == VG_OFF ? "1" : "0");
    vg_cvar("r.occlusion", var == VG_OFF ? "0" : "1");
    const bool want_gb = o->gbuffer && var == VG_ON;
    de__vis_resolve_set_debug(v->vis, &(de__vis_resolve_debug){.readback = want_gb});
    de__rg_reset(g);
    de_arena_reset(v->arena);
    if (de__rg_begin(g, v->arena) != DE_OK ||
        (first && de__gpu_scene_add_passes(v->gs, g) != DE_OK)) {
        de_golden_fail(v->ctx, "frame %llu: graph begin: %s", (unsigned long long)n, vg_err());
        return false;
    }
    const de__vis_frame_desc d = {.gpu_scene = v->gs,
                                  .view = (de_view){.id = (u32)var + 1u},
                                  .view_slot = (u32)var,
                                  .frame_index = n,
                                  .frames_completed = n,
                                  .width = v->width,
                                  .height = v->height,
                                  .view_matrix = *view,
                                  .proj = *proj,
                                  .camera_position = v->eye,
                                  .origin = v->world.origin};
    de__vis_context *vc = NULL;
    if (de__vis_frame_build(v->vis, g, &d, &vc) != DE_OK || vc == NULL) {
        de_golden_fail(v->ctx, "frame %llu: the vis frame: %s", (unsigned long long)n, vg_err());
        return false;
    }
    vg_image *img = &v->img[var];
    vg_image_free(img);
    img->raster_path = vc->raster_path;
    rb_pass *rp = NULL;
    u32 list_entries = 0;
    if (DE_HANDLE_VALID(vc->vis_ids) && DE_HANDLE_VALID(vc->visible_meshlets)) {
        rp = (rb_pass *)de_arena_push_zero(v->arena, sizeof *rp, 16);
        if (rp == NULL) {
            de_golden_fail(v->ctx, "frame arena full");
            return false;
        }
        static const u32 bpp[5] = {4, 4, 4, 4, 4};
        const de_rg_resource tex[5] = {vc->vis_ids,
                                       want_gb ? vc->gbuffer[DE__GB0] : (de_rg_resource){0},
                                       want_gb ? vc->gbuffer[DE__GB1] : (de_rg_resource){0},
                                       want_gb ? vc->gbuffer[DE__GB2] : (de_rg_resource){0},
                                       want_gb ? vc->gbuffer[DE__GB_MOTION] : (de_rg_resource){0}};
        if (want_gb && !DE_HANDLE_VALID(tex[1])) {
            de_golden_fail(v->ctx, "frame %llu: the resolve made no G-buffer",
                           (unsigned long long)n);
            return false;
        }
        u64 at = 0;
        for (u32 i = 0; i < 5; ++i) {
            rb_item *it = &rp->it[i];
            it->r = tex[i];
            if (!DE_HANDLE_VALID(it->r))
                continue;
            it->texture = true;
            it->pitch = (u32)vg_align((u64)v->width * bpp[i], 256u);
            it->off = at;
            it->bytes = (u64)it->pitch * v->height;
            at = vg_align(at + it->bytes, 512u);
        }
        list_entries = vc->visible_capacity + vc->visible_alpha_capacity;
        rp->it[RB_VIS] = (rb_item){.r = vc->visible_meshlets,
                                   .off = at,
                                   .bytes = (u64)list_entries * sizeof(de__vis_meshlet)};
        at += rp->it[RB_VIS].bytes;
        if (at > v->rb_size[var]) {
            if (DE_HANDLE_VALID(v->rb[var])) {
                de_gpu_wait_idle(v->dev);
                de_gpu_buffer_destroy(v->dev, v->rb[var]);
            }
            v->rb[var] =
                de_gpu_buffer_create(v->dev, &(de_gpu_buffer_desc){.size = at,
                                                                   .usage = DE_USAGE_COPY_DST,
                                                                   .memory = DE_MEM_READBACK,
                                                                   .name = "vis golden readback"});
            if (!DE_HANDLE_VALID(v->rb[var])) {
                de_golden_fail(v->ctx, "readback buffer: %s", vg_err());
                return false;
            }
            v->rb_size[var] = at;
        }
        rp->rb = v->rb[var];
        de_rg_pass *p = de_rg_add_pass(g, "golden.readback", DE_QUEUE_GRAPHICS, rb_execute, rp);
        for (u32 i = 0; i < RB_N; ++i)
            if (DE_HANDLE_VALID(rp->it[i].r))
                de_rg_read(p, rp->it[i].r, DE_ACCESS_COPY_SRC);
        de_rg_write(p, de_rg_import_buffer(g, v->rb[var], DE_ACCESS_NONE), DE_ACCESS_COPY_DST);
        /* Back to the layouts the bindless reads expect before the list ends (VVL checks them
         * against the end of the list): the depth sampled, the G-buffer storage. */
        p = de_rg_add_pass(g, "golden.restore", DE_QUEUE_GRAPHICS, NULL, NULL);
        de_rg_pass_side_effect(p);
        de_rg_read(p, vc->depth, DE_ACCESS_SHADER_READ);
        for (u32 i = 1; i < 5; ++i)
            if (DE_HANDLE_VALID(tex[i]))
                de_rg_write(p, tex[i], DE_ACCESS_SHADER_WRITE);
    }
    const u64 val = ++v->fence_value;
    de_result r = de__rg_finalize(g);
    if (r == DE_OK)
        r = de__rg_compile(g);
    if (r == DE_OK)
        r = de__rg_execute(g, &(de__rg_execute_desc){
                                  .frame_index = n, .frame_fence = v->fence, .frame_value = val});
    if (first)
        de__gpu_scene_frame_end(v->gs, g, r == DE_OK);
    if (r != DE_OK) {
        de_golden_fail(v->ctx, "frame %llu: %s", (unsigned long long)n, vg_err());
        return false;
    }
    if (de_gpu_fence_wait(v->dev, v->fence, val, VG_WAIT_NS) != DE_OK) {
        de_golden_fail(v->ctx, "frame %llu: the GPU did not finish", (unsigned long long)n);
        return false;
    }
    de__rg_reset(g); /* before the arena is reused by the next graph */
    img->stats = de__vis_view_stats(v->vis, (u32)var);
    img->mstats = de__vis_view_meshlet_stats(v->vis, (u32)var);
    const size_t px = (size_t)v->width * v->height;
    img->key = (u64 *)vg_zalloc(px * sizeof(u64));
    for (size_t i = 0; i < px; ++i)
        img->key[i] = VG_EMPTY;
    img->valid = true;
    if (rp == NULL)
        return true; /* nothing drawn: every pixel empty */
    const u8 *m = (const u8 *)de_gpu_buffer_map(v->dev, v->rb[var]);
    u32 *ids = (u32 *)rows_of(m, &rp->it[RB_IDS], v->width, v->height, 4);
    const de__vis_meshlet *list = (const de__vis_meshlet *)(m + rp->it[RB_VIS].off);
    for (size_t i = 0; i < px; ++i) {
        const u32 id = ids[i];
        if (id == DE__VIS_ID_EMPTY)
            continue;
        const u32 e = de__vis_id_meshlet(id);
        if (e >= list_entries) {
            img->key[i] = VG_EMPTY - 1u; /* an ID outside the list: always a mismatch */
            continue;
        }
        const de__vis_meshlet vm = list[e];
        img->key[i] = ((u64)vm.instance << 32) | ((u64)vm.meshlet << DE__VIS_ID_TRIANGLE_BITS) |
                      de__vis_id_triangle(id);
    }
    free(ids);
    if (want_gb) {
        img->gb0 = (u8 *)rows_of(m, &rp->it[RB_GB0], v->width, v->height, 4);
        img->gb1 = (u8 *)rows_of(m, &rp->it[RB_GB1], v->width, v->height, 4);
        img->gb2 = (u8 *)rows_of(m, &rp->it[RB_GB2], v->width, v->height, 4);
        img->mot = (u8 *)rows_of(m, &rp->it[RB_MOT], v->width, v->height, 4);
    }
    return true;
}

bool vg_frame(vg *v, const vg_frame_opts *o) {
    if (de_golden_stopped(v->ctx))
        return false;
    const u64 n = v->frames++;
    de__render_world_frame *w = &v->world;
    w->frame++;
    w->origin_changed = w->frame == 1;
    w->instances.live = 0;
    for (u32 s = 0; s < w->instances.count; ++s)
        w->instances.live += w->instances.entity[s] != 0;
    if (de__gpu_scene_update(v->gs, w) != DE_OK) {
        de_golden_fail(v->ctx, "frame %llu: GPU scene update: %s", (unsigned long long)n, vg_err());
        return false;
    }
    const de_mat4 view = de_m4_look_at(v->eye, v->target, (de_vec3){0.0f, 0.0f, 1.0f});
    const de_mat4 proj = de_m4_perspective_rz(v->fov, (f32)v->width / (f32)v->height, 0.1f);
    const de__gpu_view_params vp = {.view = view,
                                    .proj = proj,
                                    .camera_position = v->eye,
                                    .width = v->width,
                                    .height = v->height};
    for (u32 slot = 0; slot < VG_VARIANTS; ++slot)
        if (de__gpu_scene_set_view(v->gs, slot, &vp) != DE_OK) {
            de_golden_fail(v->ctx, "frame %llu: view constants: %s", (unsigned long long)n,
                           vg_err());
            return false;
        }
    bool first = true, ok = true;
    if (o->on) {
        ok = vg_run_variant(v, VG_ON, first, o, n, &view, &proj);
        first = false;
    }
    if (ok && o->off)
        ok = vg_run_variant(v, VG_OFF, first, o, n, &view, &proj);
    /* The previous transforms catch up: a slot that moved this frame is dirty once more with its
     * previous transform equal to the current one (no motion next frame unless it moves again),
     * as the render world does. */
    u32 nd = 0;
    for (u32 i = 0; i < w->dirty_instance_count; ++i) {
        const u32 s = w->dirty_instances[i];
        if (memcmp(&w->instances.prev_transform[s], &w->instances.transform[s],
                   sizeof(de_affine)) != 0) {
            w->instances.prev_transform[s] = w->instances.transform[s];
            w->dirty_instances[nd++] = s;
        }
    }
    w->dirty_instance_count = nd;
    return ok;
}

u32 vg_compare(const vg *v, const vg_image *a, const vg_image *b, u32 *first_x, u32 *first_y) {
    u32 diff = 0;
    const size_t px = (size_t)v->width * v->height;
    for (size_t i = 0; i < px; ++i) {
        if (a->key[i] == b->key[i])
            continue;
        if (diff == 0 && first_x != NULL && first_y != NULL) {
            *first_x = (u32)(i % v->width);
            *first_y = (u32)(i / v->width);
        }
        ++diff;
    }
    return diff;
}

bool vg_check_budget(vg *v, const char *what, u32 frame, u32 diff, u32 fx, u32 fy) {
    const f64 px = (f64)v->width * v->height;
    const u32 budget = (u32)floor(px * VG_TIE_BUDGET);
    if (diff > 0)
        printf("  %s, frame %u: %u of %.0f pixels differ (budget %u; first at %u, %u)\n", what,
               frame, diff, px, budget, fx, fy);
    if (diff > budget) {
        de_golden_fail(v->ctx,
                       "%s, frame %u: %u pixels differ, more than the edge-tie budget %u "
                       "(0.01%% of %.0f; first at %u, %u)",
                       what, frame, diff, budget, px, fx, fy);
        return false;
    }
    return true;
}

u32 vg_covered(const vg *v, const vg_image *img) {
    u32 n = 0;
    const size_t px = (size_t)v->width * v->height;
    for (size_t i = 0; i < px; ++i)
        n += img->key[i] != VG_EMPTY;
    return n;
}

/* ================================================================================================
 * Output
 * ============================================================================================== */

static u32 vg_hash(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void vg_paint_ids(const vg *v, const vg_image *img, u8 *rgba, u32 pitch, u32 x0, u32 y0) {
    for (u32 y = 0; y < v->height; ++y)
        for (u32 x = 0; x < v->width; ++x) {
            u8 *p = rgba + (size_t)(y0 + y) * pitch + (size_t)(x0 + x) * 4u;
            const u64 k = img->key[(size_t)y * v->width + x];
            p[3] = 255;
            if (k == VG_EMPTY) {
                p[0] = p[1] = p[2] = 0;
                continue;
            }
            if (k == VG_EMPTY - 1u) {
                p[0] = 255, p[1] = 0, p[2] = 255;
                continue;
            }
            const u32 inst = (u32)(k >> 32), meshlet = (u32)(k & 0xFFFFFFFFu) >> 7;
            const u32 h = vg_hash(inst * 2654435761u + 17u);
            const f32 shade = (vg_hash(meshlet + 101u) & 1u) ? 1.0f : 0.8f;
            p[0] = (u8)((64u + (h & 0xBFu)) * shade);
            p[1] = (u8)((64u + ((h >> 8) & 0xBFu)) * shade);
            p[2] = (u8)((64u + ((h >> 16) & 0xBFu)) * shade);
        }
}

void vg_present(vg *v, const u8 *rgba, u32 pitch) {
    de_golden_ctx *ctx = v->ctx;
    const u32 up_pitch = (u32)vg_align((u64)ctx->width * 4u, 256u);
    const de_gpu_buffer up = de_golden_upload_buffer(ctx, NULL, (u64)up_pitch * ctx->height,
                                                     DE_USAGE_COPY_SRC, "vis golden picture");
    if (!DE_HANDLE_VALID(up))
        return;
    u8 *m = (u8 *)de_gpu_buffer_map(ctx->device, up);
    if (m == NULL) {
        de_golden_fail(ctx, "mapping the picture buffer");
        return;
    }
    for (u32 y = 0; y < ctx->height; ++y)
        memcpy(m + (size_t)y * up_pitch, rgba + (size_t)y * pitch, (size_t)ctx->width * 4u);
    de_golden_target_barrier(ctx, DE_ACCESS_COPY_DST);
    de_gpu_cmd_copy_buffer_to_texture(ctx->cmd, ctx->target, 0, 0, up, 0, up_pitch);
}

void vg_show_ids(vg *v, const vg_image *img) {
    const u32 pitch = v->ctx->width * 4u;
    u8 *rgba = (u8 *)vg_zalloc((size_t)pitch * v->ctx->height);
    if (img->valid && v->width <= v->ctx->width && v->height <= v->ctx->height)
        vg_paint_ids(v, img, rgba, pitch, 0, 0);
    vg_present(v, rgba, pitch);
    free(rgba);
}
