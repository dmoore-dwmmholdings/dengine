/*
 * transparent_sort.c: the per-view transparent list of the forward pass (SPEC §12.5 step 6;
 * DENG-305). forward.h describes it: which materials are forward, the gather from the render
 * world and the GPU scene's CPU tables, and the back-to-front sort. No GPU work, no state.
 */
#include "render/passes/forward/forward.h"

#include "render/world/render_world.h"
#include "render/sprite/sprite_sort.h"
#include "core/de_sort.h"

#include <math.h>
#include <string.h>

/* =================================================================================================
 * The sort
 * ============================================================================================== */

bool de__transparent_before(const de__transparent_item *a, const de__transparent_item *b) {
    if (a->always_on_top != b->always_on_top)
        return !a->always_on_top;
    if (a->layer != b->layer)
        return a->layer < b->layer;
    if (a->order != b->order)
        return a->order < b->order;
    /* NaN: the farthest (drawn first), so it never breaks the order of the others. */
    const f32 da = isnan(a->depth) ? INFINITY : a->depth;
    const f32 db = isnan(b->depth) ? INFINITY : b->depth;
    if (da != db)
        return da > db;
    if ((u32)a->entity != (u32)b->entity)
        return (u32)a->entity < (u32)b->entity;
    if (a->entity != b->entity)
        return a->entity < b->entity;
    return a->index < b->index;
}

typedef struct radix_scratch {
    void *memory;
    size_t bytes;
} radix_scratch;
static void *sort_alloc(void *user, size_t size, size_t align, u32 tag) {
    (void)align;
    (void)tag;
    radix_scratch *s = user;
    return size <= s->bytes ? s->memory : NULL;
}
static void sort_free(void *user, void *ptr, size_t size) {
    (void)user;
    (void)ptr;
    (void)size;
}

void de__transparent_sort(de__transparent_item *items, u32 n, de__transparent_item *scratch) {
    if (items == NULL || scratch == NULL || n < 2)
        return;
    DE_STATIC_ASSERT(sizeof(de__transparent_item) >= 2 * sizeof(de_sort_key64) + sizeof(u32),
                     "radix scratch fits");
    /* Preserve payloads once; sort compact metadata in the output array. */
    memcpy(scratch, items, (size_t)n * sizeof *items);
    de_sort_key64 *keys = (de_sort_key64 *)(void *)items;
    de_sort_key32 *entity_keys = (de_sort_key32 *)(void *)items;
    radix_scratch tmp = {entity_keys + n, (size_t)n * sizeof *entity_keys};
    de_allocator allocator = {.user = &tmp, .alloc = sort_alloc, .free = sort_free};
    bool has_overlay = false, has_scene = false, index_ordered = true, same_generation = true;
    /* Stable entity-index tie order is independent of extraction/archetype order. */
    for (u32 i = 0; i < n; ++i) {
        entity_keys[i] = (de_sort_key32){(u32)scratch[i].entity, i};
        has_overlay |= scratch[i].always_on_top;
        has_scene |= !scratch[i].always_on_top;
        index_ordered &= !i || scratch[i - 1].index <= scratch[i].index;
        same_generation &= (scratch[i].entity >> 32) == (scratch[0].entity >> 32);
    }
    /* Gather indices normally arrive in order. Only sort them when the caller
     * supplied a different order, so equal entity/depth ties still match before(). */
    if (!index_ordered) {
        for (u32 i = 0; i < n; ++i)
            entity_keys[i].key = scratch[i].index;
        if (de_radix_sort_u32(entity_keys, n, &allocator) != DE_OK)
            return;
        for (u32 i = 0; i < n; ++i)
            entity_keys[i].key = (u32)scratch[entity_keys[i].index].entity;
    }
    if (same_generation) {
        if (de_radix_sort_u32(entity_keys, n, &allocator) != DE_OK)
            return;
        /* Expand backwards without overwriting unread entity indices. */
        for (u32 i = n; i-- > 0;) {
            u32 index = entity_keys[i].index;
            const de__transparent_item *item = &scratch[index];
            keys[i] =
                (de_sort_key64){de__sprite_sort_key(item->layer, item->order, item->depth), index};
        }
    } else {
        for (u32 i = n; i-- > 0;) {
            u32 index = entity_keys[i].index;
            u64 entity = scratch[index].entity;
            /* Comparator orders entity index first, generation second. */
            keys[i] = (de_sort_key64){(entity << 32) | (entity >> 32), index};
        }
        tmp = (radix_scratch){keys + n, (size_t)n * sizeof *keys};
        if (de_radix_sort_u64(keys, n, &allocator) != DE_OK)
            return;
        for (u32 i = 0; i < n; ++i) {
            const de__transparent_item *item = &scratch[keys[i].index];
            keys[i].key = de__sprite_sort_key(item->layer, item->order, item->depth);
        }
    }
    tmp = (radix_scratch){keys + n, (size_t)n * sizeof *keys};
    if (de_radix_sort_u64(keys, n, &allocator) != DE_OK)
        return;
    /* Put the permutation after both key arrays. Forward payload writes cannot
     * reach an unread index: the last n u32s end at the last output item. */
    u32 *indices = (u32 *)(void *)((u8 *)items + (size_t)n * sizeof *items) - n;
    u32 dst = 0;
    if (has_overlay && has_scene) {
        for (u32 i = 0; i < n; ++i)
            if (!scratch[keys[i].index].always_on_top)
                indices[dst++] = (u32)keys[i].index;
        for (u32 i = 0; i < n; ++i)
            if (scratch[keys[i].index].always_on_top)
                indices[dst++] = (u32)keys[i].index;
    } else {
        for (u32 i = 0; i < n; ++i)
            indices[i] = (u32)keys[i].index;
    }
    for (u32 i = 0; i < n; ++i) {
        u32 source = indices[i];
        items[i] = scratch[source];
    }
}

/* =================================================================================================
 * The gather
 * ============================================================================================== */

/* Per material slot: forward (bit 0), double-sided (bit 1), transmissive (bit 2), full model (bit
 * 3). */
enum { MAT_FORWARD = 1u, MAT_DOUBLE_SIDED = 2u, MAT_TRANSMISSIVE = 4u, MAT_FULL_MODEL = 8u };

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
            (u8)(MAT_FORWARD | (de__forward_base_material(m) ? 0u : MAT_FULL_MODEL) |
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
            .layer = (in->flags[s] & DE__RW_INST_SPRITE) ? in->sprite[s].layer : 0,
            .order = (in->flags[s] & DE__RW_INST_SPRITE) ? in->sprite[s].order : 0,
            .instance = s,
            .entity = in->entity[s],
            .index = n,
            .flags = ((bits & MAT_FULL_MODEL) == 0u ? (u32)DE__FWD_ITEM_BASE_STANDARD : 0u) |
                     ((bits & MAT_DOUBLE_SIDED) != 0u ? (u32)DE__FWD_ITEM_DOUBLE_SIDED : 0u) |
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
