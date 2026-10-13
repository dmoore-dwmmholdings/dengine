/*
 * mesh_cook.c: the mesh stage of the model importer (DENG-103): one IR mesh to one DMSH v2 blob
 * (render/formats/de_cooked_mesh.h). model_cook.c calls de_model_mesh_cook once per mesh; the
 * steps are listed in model_internal.h. The layout is documented at the top of the format header,
 * which is the reference; this file only writes it.
 */
#include "model_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

#include "render/formats/de_cooked_mesh.h"

#include <math.h>
#include <meshoptimizer.h>
#include <string.h>

/* Zero bytes up to the next section start, then the offset of the section (from `start`). */
static u32 section(de_model_buf *b, u64 start) {
    u64 rem = (b->size - start) % DE_COOKED_MESH_ALIGN;
    if (rem != 0)
        de_model_buf_put(b, NULL, DE_COOKED_MESH_ALIGN - rem);
    return (u32)(b->size - start);
}

/* Bounds of the decoded positions of the vertices `idx[0 .. n)` (all `count` when idx is NULL). */
static void bounds_of(const f32 *dq, const u32 *idx, u32 n, f32 lo[3], f32 hi[3]) {
    for (u32 k = 0; k < 3; ++k) {
        lo[k] = INFINITY;
        hi[k] = -INFINITY;
    }
    for (u32 i = 0; i < n; ++i) {
        const f32 *p = dq + 3u * (u64)(idx != NULL ? idx[i] : i);
        for (u32 k = 0; k < 3; ++k) {
            lo[k] = p[k] < lo[k] ? p[k] : lo[k];
            hi[k] = p[k] > hi[k] ? p[k] : hi[k];
        }
    }
    if (n == 0)
        for (u32 k = 0; k < 3; ++k)
            lo[k] = hi[k] = 0.0f;
}

/*
 * Widens bounds of quantized positions by a few ulps of the largest coordinate a decode can give,
 * so they contain the decoded positions however a decoder rounds offset + q * scale (with or
 * without FMA contraction, which differ by up to an ulp of the larger term).
 */
static void widen(const de_model_quant *q, f32 lo[3], f32 hi[3]) {
    if (q->format[DE_MODEL_ATTR_POSITION] != DE_COOKED_VF_UNORM16X4)
        return;
    for (u32 k = 0; k < 3; ++k) {
        f64 mag = fabs((f64)q->pos_offset[k]) + 65535.0 * (f64)q->pos_scale[k];
        f64 margin = mag * (1.0 / 4194304.0); /* 2^-22: two ulps of f32 at `mag` */
        lo[k] = (f32)((f64)lo[k] - margin);
        hi[k] = (f32)((f64)hi[k] + margin);
    }
}

/* The mesh's bounding sphere: meshoptimizer's, widened to contain every vertex. */
static void sphere_of(const f32 *dq, u32 count, f32 out[4]) {
    struct meshopt_Bounds b = meshopt_computeSphereBounds(dq, count, 12u, NULL, 0);
    f64 r2 = (f64)b.radius * b.radius, mag = 0.0;
    for (u32 i = 0; i < count; ++i) {
        f64 d2 = 0.0;
        for (u32 k = 0; k < 3; ++k) {
            f64 d = (f64)dq[3u * (u64)i + k] - b.center[k];
            d2 += d * d;
            f64 a = fabs((f64)dq[3u * (u64)i + k]);
            mag = a > mag ? a : mag;
        }
        r2 = d2 > r2 ? d2 : r2;
    }
    memcpy(out, b.center, 3 * sizeof(f32));
    out[3] = (f32)(sqrt(r2) * (1.0 + 1e-6) + mag * 1e-6);
}

de_result de_model_mesh_cook(de_model_ir *ir, const de_model_mesh *m,
                             const de_model_mesh_settings *s, const de_asset_id *material_ids,
                             de_model_buf *out) {
    u32 sc = m->submesh_count;
    if (sc == 0 || sc > DE_COOKED_MESH_MAX_SUBMESHES)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "mesh \"%s\": %u submeshes (1 to %u are supported)",
                       m->name, sc, DE_COOKED_MESH_MAX_SUBMESHES);

    /* 1-3: LODs, index order, vertex order. */
    de_model_lods lods;
    DE_TRY(de_model_lods_build(ir, m, s->generate_lods, &lods));
    DE_TRY(de_model_lods_optimize(ir, m, &lods));
    u32 *order = NULL, vcount = 0;
    DE_TRY(de_model_lods_vertex_order(ir, m, &lods, &order, &vcount));
    if (vcount == 0) {
        /* No triangle at all: keep the vertices as they are, so the blob still has one. */
        for (u32 v = 0; v < m->vertex_count; ++v)
            order[v] = v;
        vcount = m->vertex_count;
    }
    if (vcount == 0)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "mesh \"%s\" has no vertices", m->name);

    /* 4: quantization, and the positions as the runtime decodes them. */
    de_model_quant q;
    de_model_quant_choose(m, order, vcount, s->max_position_error, &q);
    f32 *dq = (f32 *)de_model_ir_array(ir, (u64)vcount * 3u, sizeof(f32));
    if (dq == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for the decoded positions");
    for (u32 v = 0; v < vcount; ++v)
        de_model_quant_position(&q, m, order[v], dq + 3u * (u64)v);

    /* 5: meshlets per LOD and submesh; the ranges of both tables. */
    u64 index_total = 0;
    for (u32 k = 0; k < lods.count; ++k)
        index_total += lods.lod[k].index_count;
    u32 *ml_first = (u32 *)de_model_ir_array(ir, (u64)lods.count * sc, sizeof(u32));
    u32 *ml_count = (u32 *)de_model_ir_array(ir, (u64)lods.count * sc, sizeof(u32));
    if (ml_first == NULL || ml_count == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for meshlets");
    de_model_meshlets ml;
    memset(&ml, 0, sizeof ml);
    bool meshlets = s->meshlets && index_total > 0;
    if (meshlets)
        DE_TRY(de_model_meshlets_init(ir, &ml, index_total, lods.count * sc));
    for (u32 k = 0; k < lods.count; ++k)
        for (u32 j = 0; j < sc; ++j) {
            const de_model_lod *l = &lods.lod[k];
            ml_first[k * sc + j] = ml.count;
            if (meshlets)
                DE_TRY(de_model_meshlets_add(ir, &ml, l->indices + l->sub_first[j], l->sub_count[j],
                                             dq, vcount, j));
            ml_count[k * sc + j] = ml.count - ml_first[k * sc + j];
        }
    meshlets = meshlets && ml.count > 0;

    /* 6: the blob. Header first; its offsets are patched once the sections are written. */
    u32 streams = 0;
    for (u32 a = 0; a < DE_MODEL_ATTR_COUNT; ++a)
        streams += q.format[a] != 0 ? 1u : 0u;
    u64 start = out->size;
    de_model_buf_put(out, DE_COOKED_MESH_MAGIC, 4);
    de_model_buf_u32(out, DE_COOKED_MESH_VERSION);
    de_model_buf_u32(out, meshlets ? DE_COOKED_MESH_FLAG_MESHLETS : 0u);
    de_model_buf_u32(out, vcount);
    de_model_buf_u32(out, sc);
    de_model_buf_u32(out, lods.count);
    de_model_buf_u32(out, streams);
    de_model_buf_u32(out, DE_COOKED_INDEX_U32);
    de_model_buf_u32(out, (u32)index_total);
    de_model_buf_u32(out, meshlets ? ml.count : 0u);
    de_model_buf_u32(out, meshlets ? ml.vertex_count : 0u);
    de_model_buf_u32(out, meshlets ? ml.triangle_bytes : 0u);
    u64 at_total = out->size;
    de_model_buf_put(out, NULL, 8); /* total size, reserved */
    u64 at_offsets = out->size;
    de_model_buf_put(out, NULL, 32); /* 8 section offsets */
    f32 lo[3], hi[3], sphere[4];
    bounds_of(dq, NULL, vcount, lo, hi);
    widen(&q, lo, hi);
    sphere_of(dq, vcount, sphere);
    for (u32 k = 0; k < 3; ++k)
        de_model_buf_f32(out, lo[k]);
    for (u32 k = 0; k < 3; ++k)
        de_model_buf_f32(out, hi[k]);
    for (u32 k = 0; k < 4; ++k)
        de_model_buf_f32(out, sphere[k]);
    for (u32 k = 0; k < 3; ++k)
        de_model_buf_f32(out, q.pos_offset[k]);
    for (u32 k = 0; k < 3; ++k)
        de_model_buf_f32(out, q.pos_scale[k]);
    for (u32 set = 0; set < 2; ++set) {
        de_model_buf_f32(out, q.uv_offset[set][0]);
        de_model_buf_f32(out, q.uv_offset[set][1]);
        de_model_buf_f32(out, q.uv_scale[set][0]);
        de_model_buf_f32(out, q.uv_scale[set][1]);
    }
    de_model_buf_f32(out, s->lod_error);
    de_model_buf_f32(out, s->max_position_error);
    de_model_buf_put(out, NULL, DE_COOKED_MESH_HEADER_SIZE - (out->size - start));

    /* Stream table (filled below, once the stream offsets are known). */
    u32 off_streams = section(out, start);
    u64 stream_table = out->size;
    de_model_buf_put(out, NULL, (u64)streams * DE_COOKED_MESH_STREAM_SIZE);

    /* Submeshes: default material, bounds of the decoded LOD 0 triangles. */
    u32 off_sub = section(out, start);
    for (u32 j = 0; j < sc; ++j) {
        const de_model_submesh *sm = &m->submeshes[j];
        de_model_buf_u64(out, sm->material >= 0 ? material_ids[sm->material].hash : 0u);
        bounds_of(dq, lods.lod[0].indices + lods.lod[0].sub_first[j], lods.lod[0].sub_count[j], lo,
                  hi);
        widen(&q, lo, hi);
        for (u32 k = 0; k < 3; ++k)
            de_model_buf_f32(out, lo[k]);
        for (u32 k = 0; k < 3; ++k)
            de_model_buf_f32(out, hi[k]);
    }

    /* LODs. */
    u32 off_lod = section(out, start);
    u32 index_first = 0;
    for (u32 k = 0; k < lods.count; ++k) {
        const de_model_lod *l = &lods.lod[k];
        u32 used = 0;
        for (u32 i = 0; i < l->index_count; ++i)
            used = l->indices[i] + 1u > used ? l->indices[i] + 1u : used;
        u32 mfirst = ml_first[k * sc], mcount = 0;
        for (u32 j = 0; j < sc; ++j)
            mcount += ml_count[k * sc + j];
        de_model_buf_u32(out, index_first);
        de_model_buf_u32(out, l->index_count);
        de_model_buf_u32(out, meshlets ? mfirst : 0u);
        de_model_buf_u32(out, meshlets ? mcount : 0u);
        de_model_buf_u32(out, l->index_count / 3u);
        de_model_buf_u32(out, used);
        de_model_buf_f32(out, l->error);
        de_model_buf_u32(out, 0);
        index_first += l->index_count;
    }

    /* LOD-submesh ranges. */
    u32 off_lodsub = section(out, start);
    index_first = 0;
    for (u32 k = 0; k < lods.count; ++k) {
        const de_model_lod *l = &lods.lod[k];
        for (u32 j = 0; j < sc; ++j) {
            de_model_buf_u32(out, index_first + l->sub_first[j]);
            de_model_buf_u32(out, l->sub_count[j]);
            de_model_buf_u32(out, meshlets ? ml_first[k * sc + j] : 0u);
            de_model_buf_u32(out, meshlets ? ml_count[k * sc + j] : 0u);
        }
        index_first += l->index_count;
    }

    /* Meshlets. */
    u32 off_ml = 0, off_mlv = 0, off_mlt = 0;
    if (meshlets) {
        off_ml = section(out, start);
        for (u32 i = 0; i < ml.count; ++i) {
            const de_model_meshlet *o = &ml.items[i];
            de_model_buf_u32(out, o->vertex_offset);
            de_model_buf_u32(out, o->triangle_offset);
            de_model_buf_u32(out, o->vertex_count | (o->triangle_count << 8) | (o->submesh << 16));
            u8 cone[4];
            for (u32 k = 0; k < 4; ++k)
                cone[k] = (u8)o->cone_s8[k];
            de_model_buf_put(out, cone, 4);
            for (u32 k = 0; k < 3; ++k)
                de_model_buf_f32(out, o->center[k]);
            de_model_buf_f32(out, o->radius);
            for (u32 k = 0; k < 3; ++k)
                de_model_buf_f32(out, o->cone_apex[k]);
            de_model_buf_f32(out, o->cone_cutoff);
            for (u32 k = 0; k < 3; ++k)
                de_model_buf_f32(out, o->cone_axis[k]);
            de_model_buf_f32(out, 0.0f);
        }
    }

    /* Vertex streams, in semantic order (the IR attribute order is the stream order). */
    u32 row = 0;
    u8 elem[16];
    for (u32 a = 0; a < DE_MODEL_ATTR_COUNT; ++a) {
        if (q.format[a] == 0)
            continue;
        u32 off = section(out, start);
        u32 esize = de_cooked_vertex_format_size(q.format[a]);
        for (u32 v = 0; v < vcount; ++v) {
            de_model_quant_encode(&q, m, a, order[v], elem);
            de_model_buf_put(out, elem, esize);
        }
        u64 rec = stream_table + (u64)row * DE_COOKED_MESH_STREAM_SIZE;
        de_model_buf_patch_u32(out, rec, a);
        de_model_buf_patch_u32(out, rec + 4, q.format[a]);
        de_model_buf_patch_u32(out, rec + 8, off);
        de_model_buf_patch_u32(out, rec + 12, (u32)((u64)vcount * esize));
        ++row;
    }

    if (meshlets) {
        off_mlv = section(out, start);
        for (u32 i = 0; i < ml.vertex_count; ++i)
            de_model_buf_u32(out, ml.vertices[i]);
        off_mlt = section(out, start);
        de_model_buf_put(out, ml.triangles, ml.triangle_bytes);
    }

    u32 off_idx = section(out, start);
    for (u32 k = 0; k < lods.count; ++k)
        for (u32 i = 0; i < lods.lod[k].index_count; ++i)
            de_model_buf_u32(out, lods.lod[k].indices[i]);

    if (out->failed)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for mesh \"%s\"", m->name);
    if (out->size - start > UINT32_MAX)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "mesh \"%s\": the cooked mesh is over 4 GiB", m->name);
    de_model_buf_patch_u32(out, at_total, (u32)(out->size - start));
    const u32 offsets[8] = {off_streams, off_sub, off_lod, off_lodsub,
                            off_ml,      off_mlv, off_mlt, off_idx};
    for (u32 i = 0; i < 8; ++i)
        de_model_buf_patch_u32(out, at_offsets + 4u * i, offsets[i]);
    return DE_OK;
}
