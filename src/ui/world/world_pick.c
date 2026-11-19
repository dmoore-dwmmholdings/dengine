/* world_pick.c: the editor's UI hit test and select hooks (DENG-779, item 3 of DENG-768).
 * de_ui_pick walks the UI records as de_ui__build_draw_list does (same roots, inherited clip and
 * popup depth, same draw filter) and sorts the hits by the reverse of its draw order, so it needs
 * no renderer. de_ui_pick_ray finds the nearest world panel under a ray with the pointer's own
 * intersection; de_ui_node_world_corners is that mapping's inverse. */
#include "world_internal.h"
#include <math.h>
#include <string.h>

typedef struct pick_candidate {
    u32 index, root, popup, tree, depth;
    i32 z;
} pick_candidate;
typedef struct pick_walk {
    de_vec4 clip;
    u32 popup, depth;
} pick_walk;

static bool contains(de_vec4 r, de_vec2 p) {
    return r.z > 0 && r.w > 0 && p.x >= r.x && p.y >= r.y && p.x < r.x + r.z && p.y < r.y + r.w;
}
static de_vec4 clip_intersect(de_vec4 a, de_vec4 b) {
    f32 x = fmaxf(a.x, b.x), y = fmaxf(a.y, b.y);
    return (de_vec4){x, y, fmaxf(0, fminf(a.x + a.z, b.x + b.z) - x),
                     fmaxf(0, fminf(a.y + a.w, b.y + b.w) - y)};
}
static bool is_root(const de_ui *ui, u32 index) {
    return ui->records[index].alive &&
           de_ui__find(ui, ui->records[index].parent) == DE_UI_NONE_INDEX;
}
static bool is_panel(const de_ui *ui, de_component panel, de_entity e) {
    return panel.id && de_entity_has(ui->world, e, panel);
}
/* Draw order (de_ui__build_draw_list's `less`), descending: the topmost first. */
static bool above(const pick_candidate *a, const pick_candidate *b) {
    if (a->root != b->root)
        return a->root > b->root;
    if (a->popup != b->popup)
        return a->popup > b->popup;
    if (a->z != b->z)
        return a->z > b->z;
    return a->tree > b->tree;
}

/* The hits under `point` in the subtree of the root record `root`, appended to `hits`. `tree`
 * counts the drawn nodes of the walk, so it matches the draw list's tree order. */
static void pick_root(de_ui *ui, u32 root, bool world, de_vec2 point, pick_walk *walk,
                      pick_candidate *hits, u32 *hit_count) {
    u32 tree = 0, index = root;
    for (u32 steps = 0; steps <= ui->record_count; ++steps) {
        const de_ui_record *v = &ui->records[index];
        const u32 parent = index == root ? DE_UI_NONE_INDEX : de_ui__find(ui, v->parent);
        pick_walk w =
            parent == DE_UI_NONE_INDEX
                ? (pick_walk){world ? v->node.computed_rect
                                    : (de_vec4){0, 0, (f32)ui->width, (f32)ui->height},
                              0, 0}
                : (pick_walk){walk[parent].clip, walk[parent].popup, walk[parent].depth + 1};
        if (v->node.clip)
            w.clip = clip_intersect(w.clip, v->node.computed_rect);
        if (de_entity_has(ui->world, v->entity, ui->ids.popup))
            ++w.popup;
        walk[index] = w;
        if (v->node.display != DE_UI_NONE && v->node.computed_rect.z > 0 &&
            v->node.computed_rect.w > 0 && w.clip.z > 0 && w.clip.w > 0) {
            const u32 order = tree++;
            if (contains(v->node.computed_rect, point) && contains(w.clip, point))
                hits[(*hit_count)++] =
                    (pick_candidate){index, root, w.popup, order, w.depth, v->node.z_order};
        }
        if (v->first_child != DE_UI_NONE_INDEX) {
            index = v->first_child;
            continue;
        }
        while (index != root && ui->records[index].next_sibling == DE_UI_NONE_INDEX) {
            index = de_ui__find(ui, ui->records[index].parent);
            if (index == DE_UI_NONE_INDEX)
                return;
        }
        if (index == root)
            return;
        index = ui->records[index].next_sibling;
    }
}

de_result de_ui_pick(de_ui *ui, de_entity root, de_vec2 point, de_ui_pick_hit *out, u32 capacity,
                     u32 *count) {
    if (!ui || !count || (capacity && !out))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_ui_pick: NULL argument");
    *count = 0;
    if (!ui->world || !ui->records)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_ui_pick: the UI is not attached");
    if (!isfinite(point.x) || !isfinite(point.y))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_ui_pick: the point is not finite");
    u32 only = DE_UI_NONE_INDEX;
    if (root.bits) {
        only = de_entity_alive(ui->world, root) ? de_ui__find(ui, root) : DE_UI_NONE_INDEX;
        if (only == DE_UI_NONE_INDEX || !is_root(ui, only))
            return DE_FAIL(DE_ERR_NOT_FOUND, "de_ui_pick: 0x%016llx is not a UI root",
                           (unsigned long long)root.bits);
    }
    if (!ui->record_count)
        return DE_OK;
    const u64 walk_size = (u64)ui->record_count * sizeof(pick_walk);
    const u64 hit_size = (u64)ui->record_count * sizeof(pick_candidate);
    pick_walk *walk = DE_ALLOC(ui->alloc, walk_size, _Alignof(pick_walk), DE_MEM_UI);
    pick_candidate *hits = DE_ALLOC(ui->alloc, hit_size, _Alignof(pick_candidate), DE_MEM_UI);
    if (!walk || !hits) {
        if (walk)
            DE_FREE(ui->alloc, walk, walk_size);
        if (hits)
            DE_FREE(ui->alloc, hits, hit_size);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_ui_pick: out of memory");
    }
    const de_component panel = de_component_find(ui->world, "DeUiWorldPanel");
    u32 n = 0;
    for (u32 i = 0; i < ui->record_count; ++i) {
        if (only != DE_UI_NONE_INDEX ? i != only : !is_root(ui, i))
            continue;
        const bool world = is_panel(ui, panel, ui->records[i].entity);
        if (only == DE_UI_NONE_INDEX && world)
            continue;
        pick_root(ui, i, world, point, walk, hits, &n);
    }
    /* Insertion sort, stable: the stack under one point is short. */
    for (u32 i = 1; i < n; ++i) {
        const pick_candidate c = hits[i];
        u32 j = i;
        for (; j > 0 && above(&c, &hits[j - 1]); --j)
            hits[j] = hits[j - 1];
        hits[j] = c;
    }
    for (u32 i = 0; i < n && i < capacity; ++i)
        out[i] = (de_ui_pick_hit){ui->records[hits[i].index].entity,
                                  ui->records[hits[i].root].entity,
                                  ui->records[hits[i].index].node.computed_rect, hits[i].depth};
    *count = n;
    DE_FREE(ui->alloc, walk, walk_size);
    DE_FREE(ui->alloc, hits, hit_size);
    return DE_OK;
}

de_result de_ui_pick_ray(de_ui *ui, de_dvec3 origin, de_vec3 direction, de_ui_panel_pick *panel,
                         de_ui_pick_hit *out, u32 capacity, u32 *count) {
    if (panel)
        memset(panel, 0, sizeof *panel);
    if (!ui || !count || (capacity && !out))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_ui_pick_ray: NULL argument");
    *count = 0;
    if (!ui->world || !ui->records)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_ui_pick_ray: the UI is not attached");
    if (!isfinite(origin.x) || !isfinite(origin.y) || !isfinite(origin.z) ||
        !isfinite(direction.x) || !isfinite(direction.y) || !isfinite(direction.z) ||
        (direction.x == 0 && direction.y == 0 && direction.z == 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_ui_pick_ray: the ray needs a finite origin and a non-zero direction");
    const de_component type = de_component_find(ui->world, "DeUiWorldPanel");
    if (!type.id)
        return DE_OK;
    de_ui_panel_pick best = {.distance = INFINITY};
    for (u32 i = 0; i < ui->record_count; ++i) {
        if (!is_root(ui, i))
            continue;
        const de_entity e = ui->records[i].entity;
        const DeUiWorldPanel *p = de_entity_get_const(ui->world, e, type);
        const DeGlobalTransform *t =
            p ? de_entity_get_const(ui->world, e, ui->hierarchy.global_transform) : NULL;
        de_vec2 pixel;
        f32 distance;
        if (!t || !de_ui__world_intersect(p, t, origin, direction, &pixel, &distance))
            continue;
        if (pixel.x < 0 || pixel.y < 0 || pixel.x >= p->size_m.x * p->pixels_per_meter ||
            pixel.y >= p->size_m.y * p->pixels_per_meter || !(distance < best.distance))
            continue;
        best = (de_ui_panel_pick){e, pixel, distance};
    }
    if (!best.panel.bits)
        return DE_OK;
    if (panel)
        *panel = best;
    /* The pointer adds the root rect's origin to the panel pixel; so does the pick. */
    const de_vec4 root_rect = ui->records[de_ui__find(ui, best.panel)].node.computed_rect;
    return de_ui_pick(ui, best.panel,
                      (de_vec2){best.pixel.x + root_rect.x, best.pixel.y + root_rect.y}, out,
                      capacity, count);
}

de_result de_ui_node_world_corners(de_ui *ui, de_entity node, de_dvec3 corners[4],
                                   de_entity *panel) {
    if (panel)
        *panel = (de_entity){0};
    if (!ui || !corners)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_ui_node_world_corners: NULL argument");
    if (!ui->world || !ui->records)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_ui_node_world_corners: the UI is not attached");
    u32 index = de_entity_alive(ui->world, node) ? de_ui__find(ui, node) : DE_UI_NONE_INDEX;
    if (index == DE_UI_NONE_INDEX)
        return DE_FAIL(DE_ERR_NOT_FOUND, "de_ui_node_world_corners: 0x%016llx is not a UI node",
                       (unsigned long long)node.bits);
    const de_vec4 rect = ui->records[index].node.computed_rect;
    for (u32 steps = 0; !is_root(ui, index) && steps <= ui->record_count; ++steps)
        index = de_ui__find(ui, ui->records[index].parent);
    const de_component type = de_component_find(ui->world, "DeUiWorldPanel");
    const de_entity root = index == DE_UI_NONE_INDEX ? (de_entity){0} : ui->records[index].entity;
    const DeUiWorldPanel *p =
        type.id && root.bits ? de_entity_get_const(ui->world, root, type) : NULL;
    const DeGlobalTransform *t =
        p ? de_entity_get_const(ui->world, root, ui->hierarchy.global_transform) : NULL;
    if (!t || !isfinite(p->pixels_per_meter) || !(p->pixels_per_meter > 0) ||
        !isfinite(p->size_m.x) || !isfinite(p->size_m.y))
        return DE_FAIL(DE_ERR_NOT_FOUND,
                       "de_ui_node_world_corners: 0x%016llx is not on a placed world panel",
                       (unsigned long long)node.bits);
    const de_vec4 root_rect = ui->records[index].node.computed_rect;
    const f32 x0 = rect.x - root_rect.x, y0 = rect.y - root_rect.y;
    const f32 xs[4] = {x0, x0 + rect.z, x0 + rect.z, x0};
    const f32 ys[4] = {y0, y0, y0 + rect.w, y0 + rect.w};
    for (u32 i = 0; i < 4; ++i) {
        /* de_ui__world_intersect's mapping, inverted: panel pixels to the panel's local metres
         * (centred, +y up), then scale, rotation and the f64 position. */
        const de_vec3 local = {(xs[i] / p->pixels_per_meter - p->size_m.x * .5f) * t->scale.x,
                               (p->size_m.y * .5f - ys[i] / p->pixels_per_meter) * t->scale.y,
                               0};
        const de_vec3 world = de_quat_rotate(t->rotation, local);
        corners[i] = (de_dvec3){t->position.x + world.x, t->position.y + world.y,
                                t->position.z + world.z};
    }
    if (panel)
        *panel = root;
    return DE_OK;
}
