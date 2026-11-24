/*
 * world_proxy.c: the far proxy bake of `de-cook world` (DENG-830). See world_proxy.h.
 *
 * Built with DE_DETERMINISTIC_FLAGS (tools/cook/CMakeLists.txt), like the mesh stage, so the
 * simplifier's float decisions are the same on every host.
 */
#include "world_proxy.h"

#include "importers/model/model_internal.h"
#include "importers/model/model_ir.h"
#include "reflect/ser_internal.h"

#include <dengine/de_platform.h>

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <meshoptimizer.h>
#include <stdio.h>
#include <string.h>

/* One loaded model; `ok` false: it could not be loaded (reported once). */
typedef struct wp_model {
    char *vpath;
    f64 scale;
    bool ok;
    de_model_ir ir;
    de_model_names names;
    void **bufs; /* the source file and every file it read: valid until destroy */
    u64 *sizes;
    u32 buf_count, buf_cap;
} wp_model;

struct de__wproxy {
    const de_allocator *a;
    de__wproxy_warn_fn warn;
    void *user;
    wp_model *models;
    u32 model_count, model_cap;
};

static void wp_warnf(de__wproxy *b, const de__wproxy_item *it, const char *fmt, ...)
    DE_PRINTF_FMT(3, 4);
static void wp_warnf(de__wproxy *b, const de__wproxy_item *it, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (b->warn != NULL)
        b->warn(b->user, it, msg);
}

de__wproxy *de__wproxy_create(const de_allocator *a, de__wproxy_warn_fn warn, void *user) {
    de__wproxy *b = (de__wproxy *)DE_ALLOC(a, sizeof *b, DE_ALIGNOF(de__wproxy), DE_MEM_GENERAL);
    if (b == NULL)
        return NULL;
    memset(b, 0, sizeof *b);
    b->a = a;
    b->warn = warn;
    b->user = user;
    return b;
}

static void wp_model_free(de__wproxy *b, wp_model *m) {
    if (m->ok)
        de_model_ir_free(&m->ir);
    for (u32 i = 0; i < m->buf_count; ++i)
        DE_FREE(b->a, m->bufs[i], m->sizes[i] + 1);
    if (m->bufs != NULL) {
        DE_FREE(b->a, m->bufs, sizeof *m->bufs * m->buf_cap);
        DE_FREE(b->a, m->sizes, sizeof *m->sizes * m->buf_cap);
    }
    if (m->vpath != NULL)
        DE_FREE(b->a, m->vpath, strlen(m->vpath) + 1);
}

void de__wproxy_destroy(de__wproxy *b) {
    if (b == NULL)
        return;
    for (u32 i = 0; i < b->model_count; ++i)
        wp_model_free(b, &b->models[i]);
    if (b->models != NULL)
        DE_FREE(b->a, b->models, sizeof *b->models * b->model_cap);
    DE_FREE(b->a, b, sizeof *b);
}

/* ---- Loading ------------------------------------------------------------------------------- */

static bool wp_keep(de__wproxy *b, wp_model *m, void *data, u64 size) {
    if (m->buf_count == m->buf_cap) {
        u32 cap = m->buf_cap ? m->buf_cap * 2u : 4u;
        void **nb = (void **)DE_ALLOC(b->a, sizeof *nb * cap, DE_ALIGNOF(void *), DE_MEM_GENERAL);
        u64 *ns = (u64 *)DE_ALLOC(b->a, sizeof *ns * cap, DE_ALIGNOF(u64), DE_MEM_GENERAL);
        if (nb == NULL || ns == NULL) {
            if (nb != NULL)
                DE_FREE(b->a, nb, sizeof *nb * cap);
            if (ns != NULL)
                DE_FREE(b->a, ns, sizeof *ns * cap);
            return false;
        }
        if (m->buf_count > 0) {
            memcpy(nb, m->bufs, sizeof *nb * m->buf_count);
            memcpy(ns, m->sizes, sizeof *ns * m->buf_count);
            DE_FREE(b->a, m->bufs, sizeof *m->bufs * m->buf_cap);
            DE_FREE(b->a, m->sizes, sizeof *m->sizes * m->buf_cap);
        }
        m->bufs = nb;
        m->sizes = ns;
        m->buf_cap = cap;
    }
    m->bufs[m->buf_count] = data;
    m->sizes[m->buf_count] = size;
    m->buf_count++;
    return true;
}

typedef struct wp_read_ctx {
    de__wproxy *b;
    wp_model *m;
} wp_read_ctx;

/* A glTF buffer or image URI, relative to the model's directory. */
static de_result wp_read(void *user, const char *rel, const void **data, u64 *size) {
    wp_read_ctx *x = (wp_read_ctx *)user;
    char raw[DE_FS_PATH_MAX], vpath[DE_FS_PATH_MAX];
    const char *slash = strrchr(x->m->vpath, '/');
    int dir_len = slash != NULL ? (int)(slash - x->m->vpath) + 1 : 0;
    int n = snprintf(raw, sizeof raw, "%.*s%s", dir_len, x->m->vpath, rel);
    if (n < 0 || (size_t)n >= sizeof raw || de_path_normalize(raw, vpath, sizeof vpath) != DE_OK)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "bad file path `%s`", rel);
    void *bytes = NULL;
    u64 len = 0;
    DE_TRY(de_file_read_all(vpath, x->b->a, &bytes, &len));
    if (!wp_keep(x->b, x->m, bytes, len)) {
        DE_FREE(x->b->a, bytes, len + 1);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "proxy bake: file list");
    }
    *data = bytes;
    *size = len;
    return DE_OK;
}

static bool wp_ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), k = strlen(suffix);
    if (n < k)
        return false;
    for (size_t i = 0; i < k; ++i) {
        char c = s[n - k + i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != suffix[i])
            return false;
    }
    return true;
}

/* The model of `it`, loaded on first use; NULL when it cannot be (warned once) or out of memory
 * (`*oom`). */
static wp_model *wp_model_get(de__wproxy *b, const de__wproxy_item *it, bool *oom) {
    for (u32 i = 0; i < b->model_count; ++i)
        if (b->models[i].scale == it->scale && strcmp(b->models[i].vpath, it->model_vpath) == 0)
            return b->models[i].ok ? &b->models[i] : NULL;
    if (b->model_count == b->model_cap) {
        u32 cap = b->model_cap ? b->model_cap * 2u : 8u;
        wp_model *nm =
            (wp_model *)DE_ALLOC(b->a, sizeof *nm * cap, DE_ALIGNOF(wp_model), DE_MEM_GENERAL);
        if (nm == NULL) {
            *oom = true;
            return NULL;
        }
        if (b->model_count > 0) {
            memcpy(nm, b->models, sizeof *nm * b->model_count);
            DE_FREE(b->a, b->models, sizeof *b->models * b->model_cap);
        }
        b->models = nm;
        b->model_cap = cap;
    }
    wp_model *m = &b->models[b->model_count];
    memset(m, 0, sizeof *m);
    size_t len = strlen(it->model_vpath);
    m->vpath = (char *)DE_ALLOC(b->a, len + 1, 1, DE_MEM_GENERAL);
    if (m->vpath == NULL) {
        *oom = true;
        return NULL;
    }
    memcpy(m->vpath, it->model_vpath, len + 1);
    m->scale = it->scale;
    b->model_count++;

    if (!wp_ends_with(m->vpath, ".gltf") && !wp_ends_with(m->vpath, ".glb")) {
        wp_warnf(b, it, "far proxy: `%s` is not a glTF model; its meshes are left out", m->vpath);
        return NULL;
    }
    void *data = NULL;
    u64 size = 0;
    de_result r = de_file_read_all(m->vpath, b->a, &data, &size);
    if (r != DE_OK) {
        wp_warnf(b, it, "far proxy: cannot read `%s`: %s", m->vpath, de_result_str(r));
        return NULL;
    }
    if (!wp_keep(b, m, data, size)) {
        DE_FREE(b->a, data, size + 1);
        *oom = true;
        return NULL;
    }
    wp_read_ctx rx = {b, m};
    de_model_source src = {data, size, m->vpath, wp_read, &rx};
    de_model_ir_init(&m->ir, b->a, m->vpath);
    de_error_clear();
    r = de_model_gltf_load(&src, &m->ir);
    if (r == DE_OK) {
        de_model_ir_convert_axes(&m->ir, DE_MODEL_AXES_GLTF, (f32)m->scale);
        r = de_model_names_build(&m->ir, &m->names);
    }
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        wp_warnf(b, it, "far proxy: cannot load `%s`: %s", m->vpath,
                 e != NULL && e->message[0] ? e->message : de_result_str(r));
        de_model_ir_free(&m->ir);
        return NULL;
    }
    m->ok = true;
    return m;
}

/* ---- Geometry ------------------------------------------------------------------------------ */

/* Triangles are grouped by base colour: one glTF primitive (and material) per group. Past
 * WP_MAX_GROUPS colours, a triangle joins the group of the nearest colour. */
#define WP_MAX_GROUPS 8u
#define WP_MIN_TRIS 12u

typedef struct wp_geo {
    const de_allocator *a;
    f32 *pos; /* 3 per vertex, 3 vertices per triangle (not indexed) */
    u8 *grp;  /* the group of each triangle */
    u32 tris, cap;
    u32 groups;
    f32 key[WP_MAX_GROUPS][3];   /* the colour that opened the group */
    f64 color[WP_MAX_GROUPS][3]; /* area-weighted sums */
    f64 area[WP_MAX_GROUPS];
} wp_geo;

static bool wp_geo_reserve(wp_geo *g, u32 more) {
    if (g->tris + more <= g->cap)
        return true;
    u32 cap = g->cap ? g->cap : 256u;
    while (cap < g->tris + more)
        cap *= 2u;
    f32 *np = (f32 *)DE_ALLOC(g->a, sizeof(f32) * 9u * cap, 16, DE_MEM_GENERAL);
    u8 *ng = (u8 *)DE_ALLOC(g->a, cap, 16, DE_MEM_GENERAL);
    if (np == NULL || ng == NULL) {
        if (np != NULL)
            DE_FREE(g->a, np, sizeof(f32) * 9u * cap);
        if (ng != NULL)
            DE_FREE(g->a, ng, cap);
        return false;
    }
    if (g->tris > 0) {
        memcpy(np, g->pos, sizeof(f32) * 9u * g->tris);
        memcpy(ng, g->grp, g->tris);
    }
    if (g->pos != NULL) {
        DE_FREE(g->a, g->pos, sizeof(f32) * 9u * g->cap);
        DE_FREE(g->a, g->grp, g->cap);
    }
    g->pos = np;
    g->grp = ng;
    g->cap = cap;
    return true;
}

/* The group of colour `rgb`: an equal one, a new one, or (all taken) the nearest. */
static u32 wp_group(wp_geo *g, const f64 rgb[3]) {
    const f32 c[3] = {(f32)rgb[0], (f32)rgb[1], (f32)rgb[2]};
    u32 best = 0;
    f64 best_d = -1.0;
    for (u32 i = 0; i < g->groups; ++i) {
        f64 d = 0.0;
        for (int k = 0; k < 3; ++k)
            d += ((f64)g->key[i][k] - c[k]) * ((f64)g->key[i][k] - c[k]);
        if (d == 0.0)
            return i;
        if (best_d < 0.0 || d < best_d) {
            best_d = d;
            best = i;
        }
    }
    if (g->groups < WP_MAX_GROUPS) {
        memcpy(g->key[g->groups], c, sizeof c);
        return g->groups++;
    }
    return best;
}

static void wp_xform(const f64 m[12], const f32 *p, f64 out[3]) {
    for (int r = 0; r < 3; ++r)
        out[r] = m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
}

static u32 wp_find_mesh(const wp_model *m, const char *name) {
    if (name[0] == '\0')
        return m->ir.mesh_count == 1 ? 0u : UINT32_MAX;
    for (u32 i = 0; i < m->ir.mesh_count; ++i)
        if (m->names.meshes != NULL && strcmp(m->names.meshes[i], name) == 0)
            return i;
    return UINT32_MAX;
}

/* Appends the triangles of `it` in the cell frame. False: out of memory. */
static bool wp_add(de__wproxy *b, wp_geo *g, const de__wproxy_item *it) {
    bool oom = false;
    wp_model *m = wp_model_get(b, it, &oom);
    if (m == NULL)
        return !oom;
    u32 mi = wp_find_mesh(m, it->mesh);
    if (mi == UINT32_MAX) {
        wp_warnf(b, it, "far proxy: `%s` has no mesh `%s`", m->vpath, it->mesh);
        return true;
    }
    const de_model_mesh *mesh = &m->ir.meshes[mi];
    const f64 *t = it->m;
    const f64 det = t[0] * (t[5] * t[10] - t[6] * t[9]) - t[1] * (t[4] * t[10] - t[6] * t[8]) +
                    t[2] * (t[4] * t[9] - t[5] * t[8]);
    const bool flip = det < 0.0;
    for (u32 sm = 0; sm < mesh->submesh_count; ++sm) {
        const de_model_submesh *s = &mesh->submeshes[sm];
        const u32 n = s->index_count / 3u;
        if (!wp_geo_reserve(g, n))
            return false;
        f64 rgb[3] = {1.0, 1.0, 1.0};
        if (sm < 8u && (it->color_mask & (1u << sm)) != 0u)
            for (int c = 0; c < 3; ++c)
                rgb[c] = it->colors[sm][c];
        else if (s->material >= 0 && (u32)s->material < m->ir.material_count)
            for (int c = 0; c < 3; ++c)
                rgb[c] = m->ir.materials[s->material].params.base_color[c];
        const u32 grp = wp_group(g, rgb);
        for (u32 k = 0; k < n; ++k) {
            f64 v[3][3];
            for (u32 c = 0; c < 3; ++c) {
                u32 idx = mesh->indices[s->index_first + k * 3u + (flip ? 2u - c : c)];
                if (idx >= mesh->vertex_count)
                    idx = 0;
                wp_xform(t, &mesh->position[(size_t)idx * 3u], v[c]);
            }
            f32 *o = &g->pos[(size_t)g->tris * 9u];
            for (u32 c = 0; c < 3; ++c)
                for (u32 d = 0; d < 3; ++d)
                    o[c * 3u + d] = (f32)v[c][d];
            const f64 e1[3] = {v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2]};
            const f64 e2[3] = {v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2]};
            const f64 cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2],
                      cz = e1[0] * e2[1] - e1[1] * e2[0];
            const f64 area = 0.5 * sqrt(cx * cx + cy * cy + cz * cz);
            for (int c = 0; c < 3; ++c)
                g->color[grp][c] += rgb[c] * area;
            g->area[grp] += area;
            g->grp[g->tris] = (u8)grp;
            g->tris++;
        }
    }
    return true;
}

/* ---- Output -------------------------------------------------------------------------------- */

static bool wp_base64(de_strbuf *out, const u8 *p, size_t n) {
    static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (de_strbuf_reserve(out, (n + 2) / 3 * 4) != DE_OK)
        return false;
    for (size_t i = 0; i < n; i += 3) {
        u32 v = (u32)p[i] << 16;
        if (i + 1 < n)
            v |= (u32)p[i + 1] << 8;
        if (i + 2 < n)
            v |= p[i + 2];
        char q[4] = {k[(v >> 18) & 63], k[(v >> 12) & 63], i + 1 < n ? k[(v >> 6) & 63] : '=',
                     i + 2 < n ? k[v & 63] : '='};
        if (de_strbuf_append(out, (de_str){q, 4}) != DE_OK)
            return false;
    }
    return true;
}

static bool wp_f32s(de_strbuf *out, const f32 *v, u32 n) {
    bool ok = de_strbuf_append_char(out, '[') == DE_OK;
    for (u32 i = 0; ok && i < n; ++i) {
        char num[DE__SER_NUM_CAP];
        (void)de__ser_format_f32(v[i], num);
        ok = (i == 0 || de_strbuf_append_char(out, ',') == DE_OK) &&
             de_strbuf_append_cstr(out, num) == DE_OK;
    }
    return ok && de_strbuf_append_char(out, ']') == DE_OK;
}

/* The glTF of the flat triangles `tri` (9 floats each, engine axes): one primitive per group,
 * group `i` being the next `count[i]` triangles with base colour `rgb[i]`. */
static bool wp_write(const de_allocator *a, de_strbuf *out, const f32 *tri, const u32 *count,
                     const f32 (*rgb)[3], u32 groups) {
    u32 tris = 0;
    for (u32 i = 0; i < groups; ++i)
        tris += count[i];
    const u32 vc = tris * 3u;
    const size_t vbytes = (size_t)vc * 12u, ibytes = (size_t)vc * 4u;
    const size_t total = vbytes * 2u + ibytes;
    u8 *bin = (u8 *)DE_ALLOC(a, total, 16, DE_MEM_GENERAL);
    if (bin == NULL)
        return false;
    f32 *pos = (f32 *)bin, *nrm = (f32 *)(bin + vbytes);
    u32 *idx = (u32 *)(bin + vbytes * 2u);
    f32 lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (u32 t = 0; t < tris; ++t) {
        f64 p[3][3];
        for (u32 c = 0; c < 3; ++c) {
            const f32 *e = &tri[(size_t)t * 9u + c * 3u];
            /* engine (x, y, z) -> glTF (x, z, -y): the importer's conversion undoes it */
            p[c][0] = e[0];
            p[c][1] = e[2];
            p[c][2] = -(f64)e[1];
        }
        const f64 e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
        const f64 e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
        f64 n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                    e1[0] * e2[1] - e1[1] * e2[0]};
        const f64 len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 0.0)
            for (int d = 0; d < 3; ++d)
                n[d] /= len;
        else {
            n[0] = 0.0;
            n[1] = 1.0;
            n[2] = 0.0;
        }
        for (u32 c = 0; c < 3; ++c) {
            const u32 v = t * 3u + c;
            for (u32 d = 0; d < 3; ++d) {
                pos[v * 3u + d] = (f32)p[c][d];
                nrm[v * 3u + d] = (f32)n[d];
                if (pos[v * 3u + d] < lo[d])
                    lo[d] = pos[v * 3u + d];
                if (pos[v * 3u + d] > hi[d])
                    hi[d] = pos[v * 3u + d];
            }
            idx[v] = v;
        }
    }
    bool ok = de_strbuf_append_cstr(
                  out, "{\"asset\":{\"version\":\"2.0\",\"generator\":\"de-cook world far proxy "
                       "(DENG-830)\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{"
                       "\"name\":\"proxy\",\"mesh\":0}],\"meshes\":[{\"name\":\"proxy\","
                       "\"primitives\":[") == DE_OK;
    for (u32 i = 0; ok && i < groups; ++i)
        ok = de_strbuf_appendf(out,
                               "%s{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":%u,"
                               "\"material\":%u}",
                               i ? "," : "", i + 2u, i) == DE_OK;
    ok = ok && de_strbuf_append_cstr(out, "]}],\"materials\":[") == DE_OK;
    for (u32 i = 0; ok && i < groups; ++i) {
        char num[3][DE__SER_NUM_CAP];
        for (int c = 0; c < 3; ++c)
            (void)de__ser_format_f32(rgb[i][c], num[c]);
        ok = de_strbuf_appendf(out,
                               "%s{\"name\":\"proxy%u\",\"pbrMetallicRoughness\":{"
                               "\"baseColorFactor\":[%s,%s,%s,1.0],\"metallicFactor\":0.0,"
                               "\"roughnessFactor\":1.0}}",
                               i ? "," : "", i, num[0], num[1], num[2]) == DE_OK;
    }
    ok = ok && de_strbuf_appendf(out,
                                 "],\"buffers\":[{\"byteLength\":%llu,\"uri\":\"data:application/"
                                 "octet-stream;base64,",
                                 (unsigned long long)total) == DE_OK;
    ok = ok && wp_base64(out, bin, total);
    ok = ok &&
         de_strbuf_appendf(
             out,
             "\"}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%llu,"
             "\"target\":34962},{\"buffer\":0,\"byteOffset\":%llu,\"byteLength\":%llu,"
             "\"target\":34962},{\"buffer\":0,\"byteOffset\":%llu,\"byteLength\":%llu,"
             "\"target\":34963}],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
             "\"count\":%u,\"type\":\"VEC3\",\"min\":",
             (unsigned long long)vbytes, (unsigned long long)vbytes, (unsigned long long)vbytes,
             (unsigned long long)(vbytes * 2u), (unsigned long long)ibytes, vc) == DE_OK;
    ok = ok && wp_f32s(out, lo, 3) && de_strbuf_append_cstr(out, ",\"max\":") == DE_OK &&
         wp_f32s(out, hi, 3);
    ok = ok && de_strbuf_appendf(out,
                                 "},{\"bufferView\":1,\"componentType\":5126,\"count\":%u,"
                                 "\"type\":\"VEC3\"}",
                                 vc) == DE_OK;
    u32 first = 0;
    for (u32 i = 0; ok && i < groups; ++i) {
        ok = de_strbuf_appendf(out,
                               ",{\"bufferView\":2,\"byteOffset\":%llu,\"componentType\":5125,"
                               "\"count\":%u,\"type\":\"SCALAR\"}",
                               (unsigned long long)first * 12u, count[i] * 3u) == DE_OK;
        first += count[i];
    }
    ok = ok && de_strbuf_append_cstr(out, "]}\n") == DE_OK;
    DE_FREE(a, bin, total);
    return ok;
}

/* Welds and simplifies the `n` triangles at `src` (9 floats each) to `ratio` of them (never
 * below WP_MIN_TRIS) and writes the result to `out` (9 floats per triangle; room for `n`).
 * Returns the triangle count (at most `n`); the scratch arrays have room for 3n entries. */
static u32 wp_simplify(const f32 *src, u32 n, f32 ratio, f32 *out, u32 *remap, u32 *idx,
                       u32 *dst, f32 *verts) {
    const u32 index_count = n * 3u;
    /* Weld by position, so the simplifier sees one connected surface per source mesh. */
    const size_t unique = meshopt_generateVertexRemap(remap, NULL, index_count, src, index_count,
                                                      sizeof(f32) * 3u);
    meshopt_remapVertexBuffer(verts, src, index_count, sizeof(f32) * 3u, remap);
    meshopt_remapIndexBuffer(idx, NULL, index_count, remap);

    /* Never below WP_MIN_TRIS (a box), so small groups keep their shape. */
    f64 want = floor((f64)n * (f64)ratio + 0.5);
    if (want < (f64)WP_MIN_TRIS)
        want = (f64)WP_MIN_TRIS;
    const size_t target = (size_t)want * 3u;
    size_t got = index_count;
    if (target < index_count) {
        float err = 0.0f;
        got = meshopt_simplify(dst, idx, index_count, verts, unique, sizeof(f32) * 3u, target,
                               1.0f, 0u, &err);
        if (got > target * 2u) { /* the borders held it back: drop topology */
            size_t sloppy = meshopt_simplifySloppy(dst, idx, index_count, verts, unique,
                                                   sizeof(f32) * 3u, NULL, target, 1.0f, &err);
            if (sloppy > 0)
                got = sloppy;
            else
                got = meshopt_simplify(dst, idx, index_count, verts, unique, sizeof(f32) * 3u,
                                       target, 1.0f, 0u, &err);
        }
        if (got == 0) { /* nothing left: keep the source */
            memcpy(dst, idx, sizeof(u32) * index_count);
            got = index_count;
        }
    } else {
        memcpy(dst, idx, sizeof(u32) * index_count);
    }
    for (size_t i = 0; i < got; ++i)
        memcpy(&out[i * 3u], &verts[(size_t)dst[i] * 3u], sizeof(f32) * 3u);
    return (u32)(got / 3u);
}

de_result de__wproxy_bake(de__wproxy *b, const de__wproxy_item *items, u32 n, f32 ratio,
                          de_strbuf *out, u32 *tris) {
    de_strbuf_clear(out);
    *tris = 0;
    wp_geo g;
    memset(&g, 0, sizeof g);
    g.a = b->a;
    de_result res = DE_OK;
    u32 *remap = NULL, *idx = NULL, *dst = NULL;
    f32 *verts = NULL, *src = NULL, *res_pos = NULL;
    u32 index_count = 0;
    for (u32 i = 0; i < n; ++i)
        if (!wp_add(b, &g, &items[i])) {
            res = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "far proxy bake");
            goto done;
        }
    if (g.tris == 0)
        goto done;
    index_count = g.tris * 3u;
    remap = (u32 *)DE_ALLOC(b->a, sizeof(u32) * index_count, 16, DE_MEM_GENERAL);
    idx = (u32 *)DE_ALLOC(b->a, sizeof(u32) * index_count, 16, DE_MEM_GENERAL);
    dst = (u32 *)DE_ALLOC(b->a, sizeof(u32) * index_count, 16, DE_MEM_GENERAL);
    verts = (f32 *)DE_ALLOC(b->a, sizeof(f32) * 3u * index_count, 16, DE_MEM_GENERAL);
    src = (f32 *)DE_ALLOC(b->a, sizeof(f32) * 3u * index_count, 16, DE_MEM_GENERAL);
    res_pos = (f32 *)DE_ALLOC(b->a, sizeof(f32) * 3u * index_count, 16, DE_MEM_GENERAL);
    if (remap == NULL || idx == NULL || dst == NULL || verts == NULL || src == NULL ||
        res_pos == NULL) {
        res = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "far proxy bake");
        goto done;
    }
    /* Each colour group on its own, in group order. */
    u32 count[WP_MAX_GROUPS] = {0}, groups = 0, total = 0;
    f32 rgb[WP_MAX_GROUPS][3];
    for (u32 k = 0; k < g.groups; ++k) {
        u32 m = 0;
        for (u32 t = 0; t < g.tris; ++t)
            if (g.grp[t] == k)
                memcpy(&src[(size_t)m++ * 9u], &g.pos[(size_t)t * 9u], sizeof(f32) * 9u);
        if (m == 0)
            continue;
        const u32 got =
            wp_simplify(src, m, ratio, &res_pos[(size_t)total * 9u], remap, idx, dst, verts);
        if (got == 0)
            continue;
        for (int c = 0; c < 3; ++c)
            rgb[groups][c] = g.area[k] > 0.0 ? (f32)(g.color[k][c] / g.area[k]) : g.key[k][c];
        count[groups++] = got;
        total += got;
    }
    if (total == 0)
        goto done;
    if (!wp_write(b->a, out, res_pos, count, (const f32(*)[3])rgb, groups)) {
        res = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "far proxy bake");
        goto done;
    }
    *tris = total;

done:
    if (remap != NULL)
        DE_FREE(b->a, remap, sizeof(u32) * index_count);
    if (idx != NULL)
        DE_FREE(b->a, idx, sizeof(u32) * index_count);
    if (dst != NULL)
        DE_FREE(b->a, dst, sizeof(u32) * index_count);
    if (verts != NULL)
        DE_FREE(b->a, verts, sizeof(f32) * 3u * index_count);
    if (src != NULL)
        DE_FREE(b->a, src, sizeof(f32) * 3u * index_count);
    if (res_pos != NULL)
        DE_FREE(b->a, res_pos, sizeof(f32) * 3u * index_count);
    if (g.pos != NULL) {
        DE_FREE(b->a, g.pos, sizeof(f32) * 9u * g.cap);
        DE_FREE(b->a, g.grp, g.cap);
    }
    return res;
}
