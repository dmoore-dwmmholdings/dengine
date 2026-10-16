/*
 * transparent_sort.c: the per-view transparent list of the forward pass (SPEC §12.5 step 6;
 * DENG-305). forward.h describes it: which materials are forward, the gather from the render
 * world and the GPU scene's CPU tables, and the back-to-front sort. No GPU work, no state.
 */
#include "render/passes/forward/forward.h"

#include "render/world/render_world.h"

#include <math.h>
#include <string.h>

/* =================================================================================================
 * The sort
 * ============================================================================================== */

bool de__transparent_before(const de__transparent_item *a, const de__transparent_item *b) {
    /* NaN: the farthest (drawn first), so it never breaks the order of the others. */
    const f32 da = isnan(a->depth) ? INFINITY : a->depth;
    const f32 db = isnan(b->depth) ? INFINITY : b->depth;
    if (da != db)
        return da > db;
    if (a->entity != b->entity)
        return a->entity < b->entity;
    return a->index < b->index;
}

void de__transparent_sort(de__transparent_item *items, u32 n, de__transparent_item *scratch) {
    if (items == NULL || scratch == NULL || n < 2u)
        return;
    /* Bottom-up merge sort, ping-ponging between the two arrays (stable: ties keep their order,
     * and the keys are a total order anyway). */
    de__transparent_item *src = items, *dst = scratch;
    for (u32 width = 1; width < n; width *= 2u) {
        for (u32 lo = 0; lo < n; lo += 2u * width) {
            const u32 mid = lo + width < n ? lo + width : n;
            const u32 hi = lo + 2u * width < n ? lo + 2u * width : n;
            u32 i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                dst[k++] = de__transparent_before(&src[j], &src[i]) ? src[j++] : src[i++];
            while (i < mid)
                dst[k++] = src[i++];
            while (j < hi)
                dst[k++] = src[j++];
        }
        de__transparent_item *t = src;
        src = dst;
        dst = t;
        if (width > n / 2u)
            break; /* this pass merged everything (and the next doubling could overflow) */
    }
    if (src != items)
        memcpy(items, src, (size_t)n * sizeof *items);
}

/* =================================================================================================
 * The gather
 * ============================================================================================== */

/* Per material slot: forward (bit 0), double-sided (bit 1), transmissive (bit 2). */
enum { MAT_FORWARD = 1u, MAT_DOUBLE_SIDED = 2u, MAT_TRANSMISSIVE = 4u };

u32 de__transparent_gather(const de__transparent_view *v, de_arena *arena,
                           de__transparent_item **out, de__transparent_counts *counts) {
    *out = NULL;
    de__transparent_counts c = {0};
    if (counts != NULL)
        *counts = c;
    if (v == NULL || v->gs == NULL || v->world == NULL || arena == NULL)
        return 0;
    const de__gpu_scene_table_info mt = de__gpu_scene_table(v->gs, DE__GS_MATERIALS);
    if (mt.count == 0)
        return 0;
    u8 *mat = (u8 *)de_arena_push_zero(arena, mt.count, 16);
    if (mat == NULL) {
        if (counts != NULL) {
            c.allocation_failed = true;
            *counts = c;
        }
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "forward: the frame arena is full");
        return 0;
    }
    for (u32 s = 0; s < mt.count; ++s) {
        const de__gs_material *m =
            (const de__gs_material *)de__gpu_scene_entry(v->gs, DE__GS_MATERIALS, s);
        if (m == NULL || !de__forward_material(m))
            continue;
        mat[s] =
            (u8)(MAT_FORWARD |
                 ((m->flags & DE__GS_MATERIAL_DOUBLE_SIDED) != 0u ? MAT_DOUBLE_SIDED : 0u) |
                 ((m->features & DE__MATERIAL_FEATURE_TRANSMISSION) != 0u ? MAT_TRANSMISSIVE : 0u));
        c.forward_materials++;
    }
    if (c.forward_materials == 0) {
        if (counts != NULL)
            *counts = c;
        return 0; /* the common case: nothing transparent anywhere */
    }

    const de__rw_instances *in = &v->world->instances;
    const de__vis_cull_view *cv = v->cull;
    const bool frustum = cv != NULL && (cv->flags & DE__VIS_CULL_FRUSTUM) != 0u;
    const u32 need_mesh = DE__GS_MESH_READY | DE__GS_MESH_MESHLETS;
    de__transparent_item *items = NULL;
    u32 n = 0, cap = 0;
    for (u32 s = 0; s < in->count; ++s) {
        if (in->entity[s] == 0 || (in->flags[s] & DE__RW_INST_HAS_BOUNDS) == 0)
            continue;
        u32 mesh_slot = DE__GS_NONE;
        const u32 *mats = NULL;
        if (!de__gpu_scene_instance_slots(v->gs, s, &mesh_slot, &mats) || mesh_slot == DE__GS_NONE)
            continue;
        const de__gs_mesh *mesh =
            (const de__gs_mesh *)de__gpu_scene_entry(v->gs, DE__GS_MESHES, mesh_slot);
        if (mesh == NULL || (mesh->flags & need_mesh) != need_mesh || mesh->lod_count == 0 ||
            mesh->lods[0].meshlet_count == 0)
            continue;
        u32 bits = 0;
        const u32 subs = mesh->submesh_count < DE__GS_INSTANCE_MATERIALS
                             ? mesh->submesh_count
                             : DE__GS_INSTANCE_MATERIALS;
        for (u32 k = 0; k < subs; ++k)
            if (mats[k] != DE__GS_NONE && mats[k] < mt.count)
                bits |= mat[mats[k]];
        if ((bits & MAT_FORWARD) == 0)
            continue;
        c.candidates++;
        if ((in->layer_mask[s] & v->layer_mask) == 0)
            continue;
        const de_aabb *b = &in->bounds[s];
        const f32 lo[3] = {b->min.x, b->min.y, b->min.z}, hi[3] = {b->max.x, b->max.y, b->max.z};
        if (frustum && !de__vis_cull_aabb(cv, lo, hi))
            continue;
        if (n == cap) {
            const u32 ncap = cap ? cap * 2u : 64u;
            de__transparent_item *grown =
                (de__transparent_item *)de_arena_push(arena, (size_t)ncap * sizeof *grown, 16);
            if (grown == NULL) {
                if (counts != NULL) {
                    c.allocation_failed = true;
                    *counts = c;
                }
                (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "forward: the frame arena is full");
                return 0;
            }
            if (n > 0)
                memcpy(grown, items, (size_t)n * sizeof *items);
            items = grown;
            cap = ncap;
        }
        const de_vec3 centre = {0.5f * (b->min.x + b->max.x), 0.5f * (b->min.y + b->max.y),
                                0.5f * (b->min.z + b->max.z)};
        const de_vec3 vp = de_m4_mul_point(&v->view_matrix, centre);
        items[n] = (de__transparent_item){
            .depth = -vp.z,
            .instance = s,
            .entity = in->entity[s],
            .index = n,
            .flags = ((bits & MAT_DOUBLE_SIDED) != 0u ? (u32)DE__FWD_ITEM_DOUBLE_SIDED : 0u) |
                     ((bits & MAT_TRANSMISSIVE) != 0u ? (u32)DE__FWD_ITEM_TRANSMISSIVE : 0u),
            .meshlet_first = mesh->lods[0].meshlet_first,
            .meshlet_count = mesh->lods[0].meshlet_count};
        n++;
    }
    c.listed = n;
    if (counts != NULL)
        *counts = c;
    *out = n > 0 ? items : NULL;
    return n;
}
