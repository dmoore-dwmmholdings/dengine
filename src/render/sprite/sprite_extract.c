#include "sprite_internal.h"
#include "render/renderer_internal.h"
#include <string.h>

de_affine de__sprite_local_transform(const DeSprite *s, de_plane_2d plane) {
    de_affine a = de_affine_identity();
    a.m[0] = s->size.x;
    a.m[9] = -s->pivot.x * s->size.x;
    if (plane == DE_PLANE_TOP_XY) {
        a.m[4] = s->size.y;
        a.m[10] = -s->pivot.y * s->size.y;
    } else {
        a.m[4] = 0;
        a.m[5] = s->size.y;
        a.m[7] = -1;
        a.m[8] = 0;
        a.m[11] = -s->pivot.y * s->size.y;
    }
    return a;
}

static void extract(const de_query_chunk *c, void *user) {
    de__render_world *rw = user;
    const DeGlobalTransform *g = c->columns[0];
    const DeSprite *sprites = c->columns[1];
    for (u32 i = 0; i < c->count; ++i) {
        u32 slot = de__rw_pool_find(rw, DE__RW_KIND_INSTANCE, c->entities[i]);
        bool fresh = slot == DE__RW_NONE;
        if (fresh)
            slot = de__rw_pool_take(rw, DE__RW_KIND_INSTANCE, c->entities[i]);
        if (slot == DE__RW_NONE)
            continue;
        rw->inst_pool.seen[slot] = rw->frame;
        const DeSprite *s = &sprites[i];
        de_affine local = de__sprite_local_transform(s, rw->r->plane_2d);
        de_affine entity = de_transform_to_affine_relative(&g[i], rw->origin);
        de_affine transform = de_affine_mul(&entity, &local);
        bool changed = fresh || rw->origin_changed ||
                       memcmp(&rw->inst.sprite[slot], s, sizeof *s) != 0 ||
                       memcmp(&rw->inst.transform[slot], &transform, sizeof transform) != 0;
        if (!changed)
            continue;
        rw->inst.sprite[slot] = *s;
        rw->inst.mesh[slot] = (de_mesh){DE__SPRITE_MESH_ID};
        memset(&rw->inst.materials[(size_t)slot * DE__RW_MATERIALS], 0,
               sizeof(de_material) * DE__RW_MATERIALS);
        rw->inst.flags[slot] =
            DE__RW_INST_SPRITE | DE__RW_INST_HAS_BOUNDS |
            (s->cast_shadows ? DE__RW_INST_CAST_SHADOWS | DE__RW_INST_VISIBLE_IN_RT : 0);
        rw->inst.layer_mask[slot] = 0xffffffffu;
        rw->inst_pool.local_bounds[slot] = (de_aabb){{0, 0, 0}, {1, 1, 0}};
        /* Retain the authored transform in the pool for origin rebases; the quad-local
         * transform is applied again at extraction, without modifying simulation state. */
        de__rw_pool_set_xform(rw, DE__RW_KIND_INSTANCE, slot, &g[i], fresh);
        rw->inst.prev_transform[slot] = fresh ? transform : rw->inst.prev_transform[slot];
        rw->inst.transform[slot] = transform;
        rw->inst.bounds[slot] = de_aabb_transform(rw->inst_pool.local_bounds[slot], &transform);
        de__rw_pool_touch(rw, DE__RW_KIND_INSTANCE, slot);
    }
}
void de__sprite_extract(de__render_world *rw, de_world *w, de_query query) {
    de_query_each(w, query, extract, rw);
}
