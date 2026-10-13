/*
 * mesh_lod.c: the LOD chain and the index optimizations of the mesh stage (DENG-103, SPEC §11.6,
 * §11.8): simplification with meshoptimizer, seam locks between submeshes, vertex cache and
 * overdraw order, and the vertex fetch order that puts the coarse LODs' vertices first. See
 * model_internal.h.
 *
 * meshoptimizer is C++ behind its C API (DENG-2 Q1). It is deterministic for the same input and
 * version (pinned in third_party/vendor.toml), and each call here has its own state, so model
 * imports may run on several threads at once. Its temporary memory comes from operator new
 * (meshopt_setAllocator is process-wide, so the importer does not set it); this runs only in the
 * cooker.
 */
#include "model_internal.h"

#include <dengine/de_base.h>

#include "render/formats/de_cooked_mesh.h"

#include <float.h>
#include <meshoptimizer.h>
#include <stdlib.h>
#include <string.h>

#define OWNER_NONE  0xFFFFFFFFu
#define OWNER_MULTI 0xFFFFFFFEu

/* ---- Seam locks ------------------------------------------------------------------------------ */

typedef struct pos_key {
    u32 x, y, z, v;
} pos_key;

static u32 key_bits(f32 f) {
    if (f == 0.0f)
        f = 0.0f; /* -0 and +0 are the same position */
    u32 b;
    memcpy(&b, &f, 4);
    return b;
}

static int key_cmp(const void *pa, const void *pb) {
    const pos_key *a = (const pos_key *)pa, *b = (const pos_key *)pb;
    if (a->x != b->x)
        return a->x < b->x ? -1 : 1;
    if (a->y != b->y)
        return a->y < b->y ? -1 : 1;
    if (a->z != b->z)
        return a->z < b->z ? -1 : 1;
    return a->v < b->v ? -1 : a->v > b->v ? 1 : 0;
}

u8 *de_model_lod_seam_locks(de_model_ir *ir, const de_model_mesh *m) {
    if (m->submesh_count <= 1 || m->vertex_count == 0)
        return NULL;
    u32 *owner = (u32 *)de_model_ir_array(ir, m->vertex_count, sizeof(u32));
    u8 *lock = (u8 *)de_model_ir_array(ir, m->vertex_count, 1);
    pos_key *keys = (pos_key *)de_model_ir_array(ir, m->vertex_count, sizeof(pos_key));
    if (owner == NULL || lock == NULL || keys == NULL)
        return NULL;
    memset(owner, 0xFF, (size_t)m->vertex_count * sizeof(u32));
    for (u32 s = 0; s < m->submesh_count; ++s) {
        const de_model_submesh *sm = &m->submeshes[s];
        for (u32 i = 0; i < sm->index_count; ++i) {
            u32 v = m->indices[sm->index_first + i];
            if (owner[v] == OWNER_NONE)
                owner[v] = s;
            else if (owner[v] != s)
                owner[v] = OWNER_MULTI;
        }
    }
    u32 n = 0;
    for (u32 v = 0; v < m->vertex_count; ++v) {
        if (owner[v] == OWNER_NONE)
            continue;
        const f32 *p = m->position + 3u * (u64)v;
        keys[n++] = (pos_key){key_bits(p[0]), key_bits(p[1]), key_bits(p[2]), v};
    }
    /* Sorted by position, then vertex index: a total order, so qsort's result is unique. */
    qsort(keys, n, sizeof(pos_key), key_cmp);
    for (u32 i = 0; i < n;) {
        u32 j = i + 1u;
        while (j < n && keys[j].x == keys[i].x && keys[j].y == keys[i].y && keys[j].z == keys[i].z)
            ++j;
        bool seam = false;
        for (u32 k = i; k < j && !seam; ++k)
            seam = owner[keys[k].v] == OWNER_MULTI || owner[keys[k].v] != owner[keys[i].v];
        if (seam)
            for (u32 k = i; k < j; ++k)
                lock[keys[k].v] = meshopt_SimplifyVertex_Lock;
        i = j;
    }
    return lock;
}

/* ---- The LOD chain --------------------------------------------------------------------------- */

/* One submesh, as the simplifier sees it: its vertex range and local indices. */
typedef struct sub_input {
    u32 vfirst, vcount; /* the vertices its indices use: [vfirst, vfirst + vcount) */
    u32 *local;         /* its LOD 0 indices minus vfirst */
    u32 index_count;    /* LOD 0 */
    f32 *attrs;         /* per local vertex: the weighted attributes, or NULL */
    const u8 *lock;     /* per local vertex, or NULL */
    f32 scale;          /* meshopt_simplifyScale of its vertices */
} sub_input;

static de_result lod_alloc(de_model_ir *ir, const de_model_mesh *m, u32 index_count,
                           de_model_lod *l) {
    memset(l, 0, sizeof *l);
    l->indices = (u32 *)de_model_ir_array(ir, (u64)index_count + 1u, sizeof(u32));
    l->sub_first = (u32 *)de_model_ir_array(ir, m->submesh_count, sizeof(u32));
    l->sub_count = (u32 *)de_model_ir_array(ir, m->submesh_count, sizeof(u32));
    if (l->indices == NULL || l->sub_first == NULL || l->sub_count == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for a LOD");
    return DE_OK;
}

/* The attribute count and weights the simplifier gets for `m` (normals, then UV0). */
static u32 attr_layout(const de_model_mesh *m, f32 weights[5]) {
    u32 n = 0;
    if (m->normal != NULL)
        for (u32 k = 0; k < 3; ++k)
            weights[n++] = DE_MODEL_LOD_NORMAL_WEIGHT;
    if (m->uv[0] != NULL)
        for (u32 k = 0; k < 2; ++k)
            weights[n++] = DE_MODEL_LOD_UV_WEIGHT;
    return n;
}

static de_result sub_prepare(de_model_ir *ir, const de_model_mesh *m, u32 s, const u8 *locks,
                             u32 attr_count, sub_input *in) {
    const de_model_submesh *sm = &m->submeshes[s];
    memset(in, 0, sizeof *in);
    in->index_count = sm->index_count;
    if (sm->index_count == 0)
        return DE_OK;
    const u32 *src = m->indices + sm->index_first;
    u32 lo = UINT32_MAX, hi = 0;
    for (u32 i = 0; i < sm->index_count; ++i) {
        lo = src[i] < lo ? src[i] : lo;
        hi = src[i] > hi ? src[i] : hi;
    }
    in->vfirst = lo;
    in->vcount = hi - lo + 1u;
    in->local = (u32 *)de_model_ir_array(ir, sm->index_count, sizeof(u32));
    if (in->local == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for simplification");
    for (u32 i = 0; i < sm->index_count; ++i)
        in->local[i] = src[i] - lo;
    if (attr_count > 0) {
        in->attrs = (f32 *)de_model_ir_array(ir, (u64)in->vcount * attr_count, sizeof(f32));
        if (in->attrs == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for simplification");
        for (u32 v = 0; v < in->vcount; ++v) {
            f32 *a = in->attrs + (u64)v * attr_count;
            u64 g = (u64)lo + v;
            u32 k = 0;
            if (m->normal != NULL)
                for (u32 c = 0; c < 3; ++c)
                    a[k++] = m->normal[3 * g + c];
            if (m->uv[0] != NULL)
                for (u32 c = 0; c < 2; ++c)
                    a[k++] = m->uv[0][2 * g + c];
        }
    }
    in->lock = locks != NULL ? locks + lo : NULL;
    in->scale = meshopt_simplifyScale(m->position + 3u * (u64)lo, in->vcount, 12u);
    return DE_OK;
}

de_result de_model_lods_build(de_model_ir *ir, const de_model_mesh *m, bool generate,
                              de_model_lods *out) {
    memset(out, 0, sizeof *out);
    u32 sc = m->submesh_count;
    /* LOD 0: the source triangles. */
    DE_TRY(lod_alloc(ir, m, m->index_count, &out->lod[0]));
    de_model_lod *l0 = &out->lod[0];
    for (u32 s = 0; s < sc; ++s) {
        const de_model_submesh *sm = &m->submeshes[s];
        l0->sub_first[s] = l0->index_count;
        l0->sub_count[s] = sm->index_count;
        memcpy(l0->indices + l0->index_count, m->indices + sm->index_first,
               (size_t)sm->index_count * sizeof(u32));
        l0->index_count += sm->index_count;
    }
    out->count = 1;
    if (!generate || l0->index_count / 3u < DE_MODEL_LOD_MIN_TRIANGLES)
        return DE_OK;

    f32 weights[5];
    u32 attr_count = attr_layout(m, weights);
    const u8 *locks = de_model_lod_seam_locks(ir, m);
    if (sc > 1 && locks == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for the seam locks");
    sub_input *in = (sub_input *)de_model_ir_array(ir, sc, sizeof(sub_input));
    u32 *scratch = (u32 *)de_model_ir_array(ir, (u64)m->index_count + 1u, sizeof(u32));
    if (in == NULL || scratch == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for simplification");
    for (u32 s = 0; s < sc; ++s)
        DE_TRY(sub_prepare(ir, m, s, locks, attr_count, &in[s]));

    for (u32 k = 1; k < DE_COOKED_MESH_MAX_LODS; ++k) {
        const de_model_lod *prev = &out->lod[k - 1u];
        u32 prev_tris = prev->index_count / 3u;
        if (prev_tris < DE_MODEL_LOD_MIN_TRIANGLES)
            break;
        de_model_lod *l = &out->lod[k];
        DE_TRY(lod_alloc(ir, m, m->index_count, l));
        f32 error = prev->error;
        for (u32 s = 0; s < sc; ++s) {
            const sub_input *si = &in[s];
            l->sub_first[s] = l->index_count;
            l->sub_count[s] = 0;
            if (si->index_count == 0)
                continue;
            size_t target = (size_t)((si->index_count / 3u) >> k) * 3u;
            float rel = 0.0f;
            size_t n = meshopt_simplifyWithAttributes(
                scratch, si->local, si->index_count, m->position + 3u * (u64)si->vfirst, si->vcount,
                12u, si->attrs, (size_t)(attr_count > 0 ? attr_count : 1u) * 4u,
                attr_count > 0 ? weights : NULL, attr_count, si->lock, target, FLT_MAX, 0u, &rel);
            for (size_t i = 0; i < n; ++i)
                l->indices[l->index_count + i] = scratch[i] + si->vfirst;
            l->sub_count[s] = (u32)n;
            l->index_count += (u32)n;
            f32 abs_error = rel * si->scale;
            error = abs_error > error ? abs_error : error;
        }
        u32 tris = l->index_count / 3u;
        if (tris == 0 || (f64)tris > DE_MODEL_LOD_KEEP_RATIO * (f64)prev_tris)
            break; /* dropped: too little reduction */
        l->error = error;
        out->count = k + 1u;
    }
    return DE_OK;
}

/* ---- Index optimization ---------------------------------------------------------------------- */

de_result de_model_lods_optimize(de_model_ir *ir, const de_model_mesh *m, de_model_lods *lods) {
    u32 *tmp = (u32 *)de_model_ir_array(ir, (u64)m->index_count + 1u, sizeof(u32));
    if (tmp == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for index optimization");
    for (u32 k = 0; k < lods->count; ++k) {
        de_model_lod *l = &lods->lod[k];
        for (u32 s = 0; s < m->submesh_count; ++s) {
            u32 n = l->sub_count[s];
            if (n == 0)
                continue;
            u32 *r = l->indices + l->sub_first[s];
            meshopt_optimizeVertexCache(tmp, r, n, m->vertex_count);
            meshopt_optimizeOverdraw(r, tmp, n, m->position, m->vertex_count, 12u,
                                     DE_MODEL_OVERDRAW_THRESHOLD);
        }
    }
    return DE_OK;
}

de_result de_model_lods_vertex_order(de_model_ir *ir, const de_model_mesh *m, de_model_lods *lods,
                                     u32 **order, u32 *vertex_count) {
    u64 total = 0;
    for (u32 k = 0; k < lods->count; ++k)
        total += lods->lod[k].index_count;
    u32 *concat = (u32 *)de_model_ir_array(ir, total + 1u, sizeof(u32));
    u32 *remap = (u32 *)de_model_ir_array(ir, (u64)m->vertex_count + 1u, sizeof(u32));
    u32 *ord = (u32 *)de_model_ir_array(ir, (u64)m->vertex_count + 1u, sizeof(u32));
    if (concat == NULL || remap == NULL || ord == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for the vertex order");
    /* Coarsest LOD first: its vertices get the lowest numbers. */
    u64 at = 0;
    for (u32 k = lods->count; k-- > 0;) {
        memcpy(concat + at, lods->lod[k].indices, (size_t)lods->lod[k].index_count * sizeof(u32));
        at += lods->lod[k].index_count;
    }
    u32 n = (u32)meshopt_optimizeVertexFetchRemap(remap, concat, (size_t)total, m->vertex_count);
    for (u32 v = 0; v < m->vertex_count; ++v)
        if (remap[v] != ~0u)
            ord[remap[v]] = v;
    for (u32 k = 0; k < lods->count; ++k) {
        de_model_lod *l = &lods->lod[k];
        for (u32 i = 0; i < l->index_count; ++i)
            l->indices[i] = remap[l->indices[i]];
    }
    *order = ord;
    *vertex_count = n;
    return DE_OK;
}
