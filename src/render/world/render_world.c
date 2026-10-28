/*
 * render_world.c: the render-world buffers, the slot pools and the render origin (SPEC §12.4,
 * §5.5; DENG-175). The model is described in render_world.h.
 *
 * Arrays are grown in groups: every array of a group (the SoA arrays of a category, the pool's
 * bookkeeping) gets its new block first, and only when all of them succeed are the contents
 * moved and the old blocks freed. A failed growth leaves everything as it was.
 */
#include "render/world/render_world.h"
#include "render/skin/skin_internal.h"

#include "render/renderer_internal.h"
#include "render/sprite/sprite_internal.h"
#include "render/view_hidden.h"

#include <math.h>
#include <string.h>

/* ---------- Array groups ---------- */

typedef struct rw_array {
    void **p;
    size_t elem;
    size_t align;
} rw_array;

/* The public SoA arrays of a category (the same set in the pool and in every buffer). */
static u32 inst_arrays(de__rw_instances *x, rw_array *out) {
    out[0] = (rw_array){(void **)&x->entity, sizeof(u64), DE_ALIGNOF(u64)};
    out[1] = (rw_array){(void **)&x->mesh, sizeof(de_mesh), DE_ALIGNOF(de_mesh)};
    out[2] = (rw_array){(void **)&x->materials, sizeof(de_material) * DE__RW_MATERIALS,
                        DE_ALIGNOF(de_material)};
    out[3] = (rw_array){(void **)&x->transform, sizeof(de_affine), DE_ALIGNOF(de_affine)};
    out[4] = (rw_array){(void **)&x->prev_transform, sizeof(de_affine), DE_ALIGNOF(de_affine)};
    out[5] = (rw_array){(void **)&x->bounds, sizeof(de_aabb), DE_ALIGNOF(de_aabb)};
    out[6] = (rw_array){(void **)&x->layer_mask, sizeof(u32), DE_ALIGNOF(u32)};
    out[7] = (rw_array){(void **)&x->flags, sizeof(u8), DE_ALIGNOF(u8)};
    out[8] = (rw_array){(void **)&x->sprite, sizeof(DeSprite), DE_ALIGNOF(DeSprite)};
    return 9;
}

static u32 light_arrays(de__rw_lights *x, rw_array *out) {
    out[0] = (rw_array){(void **)&x->entity, sizeof(u64), DE_ALIGNOF(u64)};
    out[1] = (rw_array){(void **)&x->transform, sizeof(de_affine), DE_ALIGNOF(de_affine)};
    out[2] = (rw_array){(void **)&x->light, sizeof(DeLight), DE_ALIGNOF(DeLight)};
    return 3;
}

#define RW_MAX_ARRAYS 24u

static u32 cat_arrays(de__rw_kind k, void *soa, rw_array *out) {
    return k == DE__RW_KIND_INSTANCE ? inst_arrays((de__rw_instances *)soa, out)
                                     : light_arrays((de__rw_lights *)soa, out);
}

/* The pool's own per-slot arrays (instances also keep their mesh's local bounds). */
static u32 pool_arrays(de__rw_pool *p, bool bounds, rw_array *out) {
    u32 n = 0;
    out[n++] = (rw_array){(void **)&p->xform, sizeof(de_transform), DE_ALIGNOF(de_transform)};
    out[n++] = (rw_array){(void **)&p->prev_xform, sizeof(de_transform), DE_ALIGNOF(de_transform)};
    out[n++] = (rw_array){(void **)&p->version, sizeof(u64), DE_ALIGNOF(u64)};
    out[n++] = (rw_array){(void **)&p->seen, sizeof(u64), DE_ALIGNOF(u64)};
    out[n++] = (rw_array){(void **)&p->added, sizeof(u64), DE_ALIGNOF(u64)};
    out[n++] = (rw_array){(void **)&p->dirty_at, sizeof(u64), DE_ALIGNOF(u64)};
    out[n++] = (rw_array){(void **)&p->free, sizeof(u32), DE_ALIGNOF(u32)};
    out[n++] = (rw_array){(void **)&p->dirty, sizeof(u32), DE_ALIGNOF(u32)};
    out[n++] = (rw_array){(void **)&p->removed, sizeof(u32), DE_ALIGNOF(u32)};
    out[n++] = (rw_array){(void **)&p->moved, sizeof(u32), DE_ALIGNOF(u32)};
    out[n++] = (rw_array){(void **)&p->moved_prev, sizeof(u32), DE_ALIGNOF(u32)};
    if (bounds)
        out[n++] = (rw_array){(void **)&p->local_bounds, sizeof(de_aabb), DE_ALIGNOF(de_aabb)};
    return n;
}

static size_t arr_align(const rw_array *a) {
    return a->align < 16 ? 16 : a->align;
}

/* Resizes every array of a group from `old_cap` to `new_cap` elements, keeping the first `keep`
 * and zeroing the rest. All or nothing. */
static de_result arrays_resize(const de_allocator *al, const rw_array *arr, u32 n, u32 keep,
                               u32 old_cap, u32 new_cap) {
    void *fresh[RW_MAX_ARRAYS];
    DE_ASSERT(n <= RW_MAX_ARRAYS && keep <= old_cap && keep <= new_cap);
    for (u32 i = 0; i < n; ++i) {
        fresh[i] = DE_ALLOC(al, arr[i].elem * new_cap, arr_align(&arr[i]), DE_MEM_RENDER);
        if (fresh[i] == NULL) {
            for (u32 j = 0; j < i; ++j)
                DE_FREE(al, fresh[j], arr[j].elem * new_cap);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "render world: %u slots", new_cap);
        }
    }
    for (u32 i = 0; i < n; ++i) {
        void *old = *arr[i].p;
        if (old != NULL && keep > 0)
            memcpy(fresh[i], old, arr[i].elem * keep);
        memset((char *)fresh[i] + arr[i].elem * keep, 0, arr[i].elem * (new_cap - keep));
        if (old != NULL)
            DE_FREE(al, old, arr[i].elem * old_cap);
        *arr[i].p = fresh[i];
    }
    return DE_OK;
}

static void arrays_free(const de_allocator *al, const rw_array *arr, u32 n, u32 cap) {
    for (u32 i = 0; i < n; ++i) {
        if (*arr[i].p != NULL)
            DE_FREE(al, *arr[i].p, arr[i].elem * cap);
        *arr[i].p = NULL;
    }
}

static void arrays_copy_slot(const rw_array *dst, const rw_array *src, u32 n, u32 s) {
    for (u32 i = 0; i < n; ++i)
        memcpy((char *)*dst[i].p + dst[i].elem * s, (const char *)*src[i].p + src[i].elem * s,
               dst[i].elem);
}

/* memcpy of a list that may be empty (and then NULL: memcpy's pointers must not be NULL). */
static void copy_list(u32 *dst, const u32 *src, u32 n) {
    if (n > 0)
        memcpy(dst, src, sizeof(u32) * n);
}

static void arrays_zero_slot(const rw_array *arr, u32 n, u32 s) {
    for (u32 i = 0; i < n; ++i)
        memset((char *)*arr[i].p + arr[i].elem * s, 0, arr[i].elem);
}

/* ---------- Categories ---------- */

static de__rw_pool *pool_of(de__render_world *rw, de__rw_kind k) {
    return k == DE__RW_KIND_INSTANCE ? &rw->inst_pool : &rw->light_pool;
}

static void *soa_of(de__render_world *rw, de__rw_kind k) {
    return k == DE__RW_KIND_INSTANCE ? (void *)&rw->inst : (void *)&rw->lights;
}

static u32 *count_of(de__render_world *rw, de__rw_kind k) {
    return k == DE__RW_KIND_INSTANCE ? &rw->inst.count : &rw->lights.count;
}

static u32 *live_of(de__render_world *rw, de__rw_kind k) {
    return k == DE__RW_KIND_INSTANCE ? &rw->inst.live : &rw->lights.live;
}

static u64 *entity_of(de__render_world *rw, de__rw_kind k) {
    return k == DE__RW_KIND_INSTANCE ? rw->inst.entity : rw->lights.entity;
}

static de_affine *transform_of(de__render_world *rw, de__rw_kind k) {
    return k == DE__RW_KIND_INSTANCE ? rw->inst.transform : rw->lights.transform;
}

/* ---------- Lifetime ---------- */

de__render_world *de__rw_of(const de_renderer *r) {
    return r != NULL ? r->render_world : NULL;
}

de_result de__rw_init(de__render_world *rw, de_renderer *r, u32 frames_in_flight) {
    memset(rw, 0, sizeof *rw);
    rw->r = r;
    rw->alloc = r->alloc;
    rw->log = r->log;
    rw->frame_count = frames_in_flight + 1u;
    if (rw->frame_count > DE__RW_MAX_FRAMES)
        rw->frame_count = DE__RW_MAX_FRAMES;
    rw->origin_grid = DE__RW_ORIGIN_GRID_DEFAULT;
    for (u32 i = 0; i < DE__RW_MAX_FRAMES; ++i) {
        de_atomic_init_u32(&rw->frames[i].state, DE__RW_FREE);
        rw->frames[i].main_view = DE__RW_NONE;
    }
    return DE_OK;
}

static void frame_free(de__render_world *rw, de__render_world_frame *f) {
    for (u32 i = 0; i < 32; ++i)
        de__view_hidden_release(f->frozen_hidden[i]);
    de__skin_frame_free(rw->alloc, f);
    rw_array arr[RW_MAX_ARRAYS];
    u32 n = inst_arrays(&f->instances, arr);
    arr[n++] = (rw_array){(void **)&f->dirty_instances, sizeof(u32), DE_ALIGNOF(u32)};
    arr[n++] = (rw_array){(void **)&f->removed_instances, sizeof(u32), DE_ALIGNOF(u32)};
    arrays_free(rw->alloc, arr, n, f->inst_capacity);
    n = light_arrays(&f->lights, arr);
    arr[n++] = (rw_array){(void **)&f->dirty_lights, sizeof(u32), DE_ALIGNOF(u32)};
    arr[n++] = (rw_array){(void **)&f->removed_lights, sizeof(u32), DE_ALIGNOF(u32)};
    arrays_free(rw->alloc, arr, n, f->light_capacity);
    arr[0] = (rw_array){(void **)&f->views, sizeof(de__rw_view), DE_ALIGNOF(de__rw_view)};
    arrays_free(rw->alloc, arr, 1, f->view_capacity);
    f->inst_capacity = f->light_capacity = f->view_capacity = 0;
}

static void pool_free(de__render_world *rw, de__rw_kind k) {
    de__rw_pool *p = pool_of(rw, k);
    rw_array arr[RW_MAX_ARRAYS];
    u32 n = cat_arrays(k, soa_of(rw, k), arr);
    n += pool_arrays(p, k == DE__RW_KIND_INSTANCE, arr + n);
    arrays_free(rw->alloc, arr, n, p->capacity);
    arr[0] = (rw_array){(void **)&p->map_slot, sizeof(u32), DE_ALIGNOF(u32)};
    arr[1] = (rw_array){(void **)&p->map_gen, sizeof(u32), DE_ALIGNOF(u32)};
    arrays_free(rw->alloc, arr, 2, p->map_capacity);
    p->capacity = p->map_capacity = 0;
}

void de__rw_free(de__render_world *rw) {
    for (u32 i = 0; i < DE__RW_MAX_FRAMES; ++i)
        frame_free(rw, &rw->frames[i]);
    pool_free(rw, DE__RW_KIND_INSTANCE);
    pool_free(rw, DE__RW_KIND_LIGHT);
}

/* ---------- Frames ---------- */

de__render_world_frame *de__rw_begin(de__render_world *rw) {
    if (rw->filling != NULL)
        return rw->filling;
    de__rt_wait(rw); /* the frame before is rendered: its buffer and ours are free */
    rw->frame += 1u;
    de__render_world_frame *f = &rw->frames[rw->frame % rw->frame_count];
    DE_ASSERT(de_atomic_load_u32(&f->state, DE_ATOMIC_ACQUIRE) == DE__RW_FREE);
    de_atomic_store_u32(&f->state, DE__RW_FILLING, DE_ATOMIC_RELAXED);
    f->frame = rw->frame;
    f->origin = rw->origin;
    f->origin_changed = false;
    f->view_count = 0;
    for (u32 i = 0; i < 32; ++i) {
        de__view_hidden_release(f->frozen_hidden[i]);
        f->frozen_hidden[i] = NULL;
    }
    f->frozen_view_count = 0;
    f->skin_count = f->skin_joint_count = 0;
    f->main_view = DE__RW_NONE;
    memset(&f->environment, 0, sizeof f->environment);
    f->environment_count = 0;
    f->dirty_instance_count = f->removed_instance_count = 0;
    f->dirty_light_count = f->removed_light_count = 0;
    rw->origin_changed = false;
    rw->fill_start_ns = de_time_now_ns();
    rw->filling = f;
    return f;
}

de_result de__rw_frame_reserve_views(de__render_world *rw, de__render_world_frame *f, u32 n) {
    if (n <= f->view_capacity)
        return DE_OK;
    u32 cap = f->view_capacity < 4u ? 4u : f->view_capacity;
    while (cap < n)
        cap *= 2u;
    const rw_array arr = {(void **)&f->views, sizeof(de__rw_view), DE_ALIGNOF(de__rw_view)};
    const de_result res = arrays_resize(rw->alloc, &arr, 1, 0, f->view_capacity, cap);
    if (res == DE_OK)
        f->view_capacity = cap;
    return res;
}

/* Grows the arrays of one category of `f` to `cap` slots, keeping its contents. */
static de_result frame_reserve(de__render_world *rw, de__render_world_frame *f, de__rw_kind k,
                               u32 cap) {
    u32 *have = k == DE__RW_KIND_INSTANCE ? &f->inst_capacity : &f->light_capacity;
    if (cap <= *have)
        return DE_OK;
    rw_array arr[RW_MAX_ARRAYS];
    u32 n, keep;
    if (k == DE__RW_KIND_INSTANCE) {
        n = inst_arrays(&f->instances, arr);
        arr[n++] = (rw_array){(void **)&f->dirty_instances, sizeof(u32), DE_ALIGNOF(u32)};
        arr[n++] = (rw_array){(void **)&f->removed_instances, sizeof(u32), DE_ALIGNOF(u32)};
        keep = f->instances.count;
    } else {
        n = light_arrays(&f->lights, arr);
        arr[n++] = (rw_array){(void **)&f->dirty_lights, sizeof(u32), DE_ALIGNOF(u32)};
        arr[n++] = (rw_array){(void **)&f->removed_lights, sizeof(u32), DE_ALIGNOF(u32)};
        keep = f->lights.count;
    }
    const de_result res = arrays_resize(rw->alloc, arr, n, keep, *have, cap);
    if (res == DE_OK)
        *have = cap;
    return res;
}

/* ---------- Render origin ---------- */

static f64 snap(f64 x, f64 g) {
    return floor(x / g + 0.5) * g;
}

void de__rw_update_origin(de__render_world *rw, bool has_cam, de_dvec3 cam) {
    f64 g = de_cvar_float(rw->cv_grid);
    if (!(g >= 1.0)) /* also NaN, or no cvar */
        g = DE__RW_ORIGIN_GRID_DEFAULT;
    bool changed = !rw->origin_valid;
    if (has_cam) {
        const f64 h = 0.5 * g;
        const bool out = !rw->origin_valid || g != rw->origin_grid ||
                         fabs(cam.x - rw->origin.x) > h || fabs(cam.y - rw->origin.y) > h ||
                         fabs(cam.z - rw->origin.z) > h;
        if (out) {
            const de_dvec3 o = {snap(cam.x, g), snap(cam.y, g), snap(cam.z, g)};
            if (!rw->origin_valid || o.x != rw->origin.x || o.y != rw->origin.y ||
                o.z != rw->origin.z) {
                rw->origin = o;
                changed = true;
            }
        }
    }
    rw->origin_grid = g;
    rw->origin_valid = true;
    rw->origin_changed = changed;
}

/* ---------- Pools ---------- */

static de_result pool_grow(de__render_world *rw, de__rw_kind k, u32 need) {
    de__rw_pool *p = pool_of(rw, k);
    if (need <= p->capacity)
        return DE_OK;
    u32 cap = p->capacity < 64u ? 64u : p->capacity;
    while (cap < need)
        cap *= 2u;
    rw_array arr[RW_MAX_ARRAYS];
    u32 n = cat_arrays(k, soa_of(rw, k), arr);
    n += pool_arrays(p, k == DE__RW_KIND_INSTANCE, arr + n);
    /* The lists are rebuilt every frame, but the free stack and the moved lists carry over: keep
     * everything up to the old capacity. */
    const de_result res = arrays_resize(rw->alloc, arr, n, p->capacity, p->capacity, cap);
    if (res == DE_OK)
        p->capacity = cap;
    return res;
}

static de_result map_grow(de__render_world *rw, de__rw_pool *p, u32 index) {
    if (index < p->map_capacity)
        return DE_OK;
    u32 cap = p->map_capacity < 256u ? 256u : p->map_capacity;
    while (cap <= index)
        cap *= 2u;
    const rw_array arr[2] = {{(void **)&p->map_slot, sizeof(u32), DE_ALIGNOF(u32)},
                             {(void **)&p->map_gen, sizeof(u32), DE_ALIGNOF(u32)}};
    const u32 old = p->map_capacity;
    const de_result res = arrays_resize(rw->alloc, arr, 2, old, old, cap);
    if (res != DE_OK)
        return res;
    for (u32 i = old; i < cap; ++i)
        p->map_slot[i] = DE__RW_NONE;
    p->map_capacity = cap;
    return DE_OK;
}

void de__rw_pool_touch(de__render_world *rw, de__rw_kind k, u32 s) {
    de__rw_pool *p = pool_of(rw, k);
    p->version[s] = rw->frame;
    p->dirty_at[s] = rw->frame;
}

void de__rw_pool_begin(de__render_world *rw, de__rw_kind k) {
    de__rw_pool *p = pool_of(rw, k);
    p->dirty_count = p->removed_count = 0;
    u32 *t = p->moved_prev;
    p->moved_prev = p->moved;
    p->moved = t;
    p->moved_prev_count = p->moved_count;
    p->moved_count = 0;
    /* Moved last frame: this frame's previous transform is last frame's current one. */
    const u64 *ent = entity_of(rw, k);
    de_affine *cur = transform_of(rw, k);
    de_affine *prev =
        k == DE__RW_KIND_INSTANCE ? rw->inst.prev_transform : NULL; /* lights keep no prev */
    for (u32 i = 0; i < p->moved_prev_count; ++i) {
        const u32 s = p->moved_prev[i];
        if (ent[s] == 0)
            continue;
        p->prev_xform[s] = p->xform[s];
        if (prev != NULL) {
            prev[s] = cur[s];
            de__rw_pool_touch(rw, k, s);
        }
    }
}

u32 de__rw_pool_find(const de__render_world *rw, de__rw_kind k, de_entity e) {
    const de__rw_pool *p = k == DE__RW_KIND_INSTANCE ? &rw->inst_pool : &rw->light_pool;
    const u32 i = de_entity_index(e);
    if (i >= p->map_capacity || p->map_gen[i] != de_entity_generation(e))
        return DE__RW_NONE;
    return p->map_slot[i];
}

u32 de__rw_pool_take(de__render_world *rw, de__rw_kind k, de_entity e) {
    de__rw_pool *p = pool_of(rw, k);
    u32 *count = count_of(rw, k);
    const u32 index = de_entity_index(e);
    if (map_grow(rw, p, index) != DE_OK)
        return DE__RW_NONE;
    u32 s;
    if (p->free_count > 0) {
        s = p->free[--p->free_count];
    } else {
        if (pool_grow(rw, k, *count + 1u) != DE_OK)
            return DE__RW_NONE;
        s = (*count)++;
    }
    p->map_slot[index] = s;
    p->map_gen[index] = de_entity_generation(e);
    entity_of(rw, k)[s] = e.bits;
    p->added[s] = rw->frame;
    p->seen[s] = rw->frame;
    *live_of(rw, k) += 1u;
    de__rw_pool_touch(rw, k, s);
    return s;
}

/* The f32 matrices of slot `s` from its f64 transforms. */
static void slot_matrices(de__render_world *rw, de__rw_kind k, u32 s) {
    de__rw_pool *p = pool_of(rw, k);
    if (k == DE__RW_KIND_INSTANCE) {
        rw->inst.transform[s] = de_transform_to_affine_relative(&p->xform[s], rw->origin);
        rw->inst.prev_transform[s] = de_transform_to_affine_relative(&p->prev_xform[s], rw->origin);
        if (rw->inst.flags[s] & DE__RW_INST_SPRITE) {
            de_affine local = de__sprite_local_transform(&rw->inst.sprite[s], rw->r->plane_2d);
            rw->inst.transform[s] = de_affine_mul(&rw->inst.transform[s], &local);
            rw->inst.prev_transform[s] = de_affine_mul(&rw->inst.prev_transform[s], &local);
        }
        de__rw_inst_update_bounds(rw, s);
    } else {
        rw->lights.transform[s] = de_transform_to_affine_relative(&p->xform[s], rw->origin);
    }
}

void de__rw_inst_update_bounds(de__render_world *rw, u32 s) {
    if (rw->inst.flags[s] & DE__RW_INST_HAS_BOUNDS)
        rw->inst.bounds[s] =
            de_aabb_transform(rw->inst_pool.local_bounds[s], &rw->inst.transform[s]);
    else
        rw->inst.bounds[s] = de_aabb_empty();
}

void de__rw_pool_set_xform(de__render_world *rw, de__rw_kind k, u32 s, const de_transform *t,
                           bool fresh) {
    de__rw_pool *p = pool_of(rw, k);
    p->prev_xform[s] = fresh ? *t : p->xform[s];
    p->xform[s] = *t;
    if (!fresh)
        p->moved[p->moved_count++] = s; /* once per slot and frame: one changed pass */
    slot_matrices(rw, k, s);
    de__rw_pool_touch(rw, k, s);
}

/* Frees slot `s` (its entity is gone or no longer matches). */
static void pool_release(de__render_world *rw, de__rw_kind k, u32 s, const rw_array *arr, u32 n) {
    de__rw_pool *p = pool_of(rw, k);
    const de_entity e = {entity_of(rw, k)[s]};
    const u32 index = de_entity_index(e);
    if (index < p->map_capacity && p->map_slot[index] == s) {
        p->map_slot[index] = DE__RW_NONE;
        p->map_gen[index] = 0;
    }
    arrays_zero_slot(arr, n, s);
    if (k == DE__RW_KIND_INSTANCE)
        rw->inst.bounds[s] = de_aabb_empty();
    p->version[s] = rw->frame;
    p->free[p->free_count++] = s;
    p->removed[p->removed_count++] = s;
    *live_of(rw, k) -= 1u;
}

de_result de__rw_pool_end(de__render_world *rw, de__rw_kind k, de__render_world_frame *f,
                          bool sweep) {
    de__rw_pool *p = pool_of(rw, k);
    const u32 count = *count_of(rw, k);
    const u64 frame = rw->frame;
    const bool rebase = rw->origin_changed;
    rw_array src[RW_MAX_ARRAYS];
    const u32 n = cat_arrays(k, soa_of(rw, k), src);
    const u64 *ent = entity_of(rw, k);

    /* Sweep, rebase and the dirty list, in slot order (so both lists are ascending). */
    for (u32 s = 0; s < count; ++s) {
        if (ent[s] == 0)
            continue;
        if (sweep && p->seen[s] != frame) {
            pool_release(rw, k, s, src, n);
            continue;
        }
        if (rebase) {
            slot_matrices(rw, k, s);
            p->version[s] = frame;
        }
        if (p->dirty_at[s] == frame)
            p->dirty[p->dirty_count++] = s;
    }

    /* Copy what changed since this buffer last held this category. */
    de_result res = frame_reserve(rw, f, k, p->capacity);
    if (res != DE_OK)
        return DE_FAIL_WRAP(res, "render world: frame %llu", (unsigned long long)frame);
    rw_array dst[RW_MAX_ARRAYS];
    if (k == DE__RW_KIND_INSTANCE) {
        (void)inst_arrays(&f->instances, dst);
        for (u32 s = 0; s < count; ++s)
            if (p->version[s] > f->inst_synced)
                arrays_copy_slot(dst, src, n, s);
        f->instances.count = count;
        f->instances.live = rw->inst.live;
        f->inst_synced = frame;
        copy_list(f->dirty_instances, p->dirty, p->dirty_count);
        copy_list(f->removed_instances, p->removed, p->removed_count);
        f->dirty_instance_count = p->dirty_count;
        f->removed_instance_count = p->removed_count;
    } else {
        (void)light_arrays(&f->lights, dst);
        for (u32 s = 0; s < count; ++s)
            if (p->version[s] > f->light_synced)
                arrays_copy_slot(dst, src, n, s);
        f->lights.count = count;
        f->lights.live = rw->lights.live;
        f->light_synced = frame;
        copy_list(f->dirty_lights, p->dirty, p->dirty_count);
        copy_list(f->removed_lights, p->removed, p->removed_count);
        f->dirty_light_count = p->dirty_count;
        f->removed_light_count = p->removed_count;
    }
    return DE_OK;
}
