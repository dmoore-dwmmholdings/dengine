/*
 * mesh_meshlets.c: meshlets of the mesh stage (DENG-103, SPEC §11.6, §12.5 steps 1-2): each
 * (LOD, submesh) triangle list is split into meshlets of at most 64 vertices and 124 triangles,
 * with a bounding sphere and a normal cone for the GPU culling of the visibility-buffer renderer
 * (frustum, occlusion and backface-cone tests). See model_internal.h and the meshlet records in
 * render/formats/de_cooked_mesh.h.
 *
 * The bounds are computed from the decoded (quantized) positions, the ones the GPU reads, so a
 * sphere contains its vertices as the runtime sees them.
 */
#include "model_internal.h"

#include <dengine/de_base.h>

#include "render/formats/de_cooked_mesh.h"

#include <math.h>
#include <meshoptimizer.h>
#include <string.h>

de_result de_model_meshlets_init(de_model_ir *ir, de_model_meshlets *ml, u64 index_total,
                                 u32 range_count) {
    memset(ml, 0, sizeof *ml);
    /* meshopt_buildMeshletsBound per range is at most index_total / 3 / max_triangles + 1 more
     * meshlets each (and a vertex-limited split is bounded by the triangle count). */
    u64 cap = meshopt_buildMeshletsBound((size_t)index_total, DE_COOKED_MESHLET_MAX_VERTICES,
                                         DE_COOKED_MESHLET_MAX_TRIANGLES) +
              (u64)range_count;
    if (cap > index_total / 3u + range_count)
        cap = index_total / 3u + range_count; /* never more meshlets than triangles */
    if (cap > UINT32_MAX || index_total > UINT32_MAX / 2u)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "mesh too large for meshlets");
    ml->cap = (u32)cap;
    ml->vertex_cap = (u32)index_total;
    ml->triangle_cap = (u32)(index_total + 3u * cap);
    ml->items =
        (de_model_meshlet *)de_model_ir_array(ir, (u64)ml->cap + 1u, sizeof(de_model_meshlet));
    ml->vertices = (u32 *)de_model_ir_array(ir, (u64)ml->vertex_cap + 1u, sizeof(u32));
    ml->triangles = (u8 *)de_model_ir_array(ir, (u64)ml->triangle_cap + 4u, 1);
    if (ml->items == NULL || ml->vertices == NULL || ml->triangles == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for meshlets");
    return DE_OK;
}

/* Widens `radius` so that the sphere contains every vertex of the meshlet by a margin that
 * survives f32 rounding in the runtime's distance test. */
static f32 contain_radius(const f32 center[3], f32 radius, const u32 *verts, u32 count,
                          const f32 *positions) {
    f64 r2 = (f64)radius * radius, mag = 0.0;
    for (u32 k = 0; k < 3; ++k)
        mag = fabs((f64)center[k]) > mag ? fabs((f64)center[k]) : mag;
    for (u32 i = 0; i < count; ++i) {
        const f32 *p = positions + 3u * (u64)verts[i];
        f64 d2 = 0.0;
        for (u32 k = 0; k < 3; ++k) {
            f64 d = (f64)p[k] - center[k];
            d2 += d * d;
            mag = fabs((f64)p[k]) > mag ? fabs((f64)p[k]) : mag;
        }
        r2 = d2 > r2 ? d2 : r2;
    }
    return (f32)(sqrt(r2) * (1.0 + 1e-6) + mag * 1e-6);
}

de_result de_model_meshlets_add(de_model_ir *ir, de_model_meshlets *ml, const u32 *indices,
                                u32 index_count, const f32 *positions, u32 vertex_count,
                                u32 submesh) {
    if (index_count == 0)
        return DE_OK;
    size_t bound = meshopt_buildMeshletsBound(index_count, DE_COOKED_MESHLET_MAX_VERTICES,
                                              DE_COOKED_MESHLET_MAX_TRIANGLES);
    struct meshopt_Meshlet *raw =
        (struct meshopt_Meshlet *)de_model_ir_array(ir, bound + 1u, sizeof *raw);
    u32 *mv = (u32 *)de_model_ir_array(ir, index_count, sizeof(u32));
    u8 *mt = (u8 *)de_model_ir_array(ir, (u64)index_count + 4u, 1);
    if (raw == NULL || mv == NULL || mt == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for meshlets");
    size_t count =
        meshopt_buildMeshlets(raw, mv, mt, indices, index_count, positions, vertex_count, 12u,
                              DE_COOKED_MESHLET_MAX_VERTICES, DE_COOKED_MESHLET_MAX_TRIANGLES,
                              DE_MODEL_MESHLET_CONE_WEIGHT);
    for (size_t i = 0; i < count; ++i) {
        const struct meshopt_Meshlet *r = &raw[i];
        if (r->vertex_count == 0 || r->vertex_count > DE_COOKED_MESHLET_MAX_VERTICES ||
            r->triangle_count == 0 || r->triangle_count > DE_COOKED_MESHLET_MAX_TRIANGLES)
            return DE_FAIL(DE_ERR_INTERNAL, "meshoptimizer made a meshlet over the limits");
        u32 tbytes = (3u * r->triangle_count + 3u) & ~3u;
        if (ml->count == ml->cap || ml->vertex_count + r->vertex_count > ml->vertex_cap ||
            ml->triangle_bytes + tbytes > ml->triangle_cap)
            return DE_FAIL(DE_ERR_INTERNAL, "meshlet storage estimate too small");
        u32 *verts = ml->vertices + ml->vertex_count;
        u8 *tris = ml->triangles + ml->triangle_bytes;
        memcpy(verts, mv + r->vertex_offset, (size_t)r->vertex_count * sizeof(u32));
        memcpy(tris, mt + r->triangle_offset, 3u * (size_t)r->triangle_count);
        memset(tris + 3u * r->triangle_count, 0, tbytes - 3u * r->triangle_count);
        /* Locality inside the meshlet (level 0: corners are not rotated, winding kept). */
        meshopt_optimizeMeshlet(verts, tris, r->triangle_count, r->vertex_count);
        struct meshopt_Bounds b = meshopt_computeMeshletBounds(verts, tris, r->triangle_count,
                                                               positions, vertex_count, 12u);
        de_model_meshlet *o = &ml->items[ml->count++];
        memset(o, 0, sizeof *o);
        o->vertex_offset = ml->vertex_count;
        o->triangle_offset = ml->triangle_bytes;
        o->vertex_count = r->vertex_count;
        o->triangle_count = r->triangle_count;
        o->submesh = submesh;
        memcpy(o->center, b.center, sizeof o->center);
        o->radius = contain_radius(b.center, b.radius, verts, r->vertex_count, positions);
        memcpy(o->cone_apex, b.cone_apex, sizeof o->cone_apex);
        memcpy(o->cone_axis, b.cone_axis, sizeof o->cone_axis);
        o->cone_cutoff = b.cone_cutoff;
        for (u32 k = 0; k < 3; ++k)
            o->cone_s8[k] = (i8)b.cone_axis_s8[k];
        o->cone_s8[3] = (i8)b.cone_cutoff_s8;
        ml->vertex_count += r->vertex_count;
        ml->triangle_bytes += tbytes;
    }
    return DE_OK;
}
