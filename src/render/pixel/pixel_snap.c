#include "pixel_internal.h"
#include "render/renderer_internal.h"
#include "render/scene/gpu_scene.h"
#include "render/world/render_world.h"
#include <string.h>
/* Every view gets an immutable per-frame copy. All its passes, including TLAS, share it. */
struct de__pixel_snapshots {
    de_renderer *r;
    struct {
        de_gpu_buffer buffer;
        u32 capacity;
    } views[DE__VIEW_MAX][DE__RENDER_MAX_FRAMES_IN_FLIGHT];
};
static void snap_stage(de__view_frame *vf, void *user) {
    de__pixel_snapshots *owner = user;
    de_renderer *r = owner->r;
    de__gpu_scene_view_instances(r->gpu_scene, (de_gpu_buffer){0});
    if (!vf->pixel_art || !vf->pixel_art->snap_sprites || !vf->world || !vf->camera)
        return;
    u32 slot = (vf->view.id & 255u) - 1u, ring = (u32)(vf->frame_index % r->frames_in_flight);
    if (slot >= DE__VIEW_MAX)
        return;
    u32 count = vf->world->instances.count;
    if (!count)
        return;
    if (count > owner->views[slot][ring].capacity) {
        u32 capacity = 64;
        while (capacity < count)
            capacity *= 2;
        de_gpu_buffer next = de_gpu_buffer_create(
            r->dev, &(de_gpu_buffer_desc){.size = (u64)capacity * sizeof(de__gs_instance),
                                          .usage = DE_USAGE_STORAGE,
                                          .memory = DE_MEM_UPLOAD,
                                          .name = "pixel view snapped instances"});
        if (!next.id)
            return;
        de_gpu_buffer_destroy(r->dev, owner->views[slot][ring].buffer);
        owner->views[slot][ring].buffer = next;
        owner->views[slot][ring].capacity = capacity;
    }
    de_gpu_buffer buffer = owner->views[slot][ring].buffer;
    de__gs_instance *instances = de_gpu_buffer_map(r->dev, buffer);
    f64 texel = (vf->camera->camera.ortho_height > 0 ? vf->camera->camera.ortho_height : 10) /
                vf->pixel_art->internal_height;
    const de__rw_instances *src = &vf->world->instances;
    de_dvec3 camera = {vf->world->origin.x + vf->camera->offset.x,
                       vf->world->origin.y + vf->camera->offset.y,
                       vf->world->origin.z + vf->camera->offset.z};
    for (u32 i = 0; i < count; ++i) {
        de__gpu_scene_pack_frame_instance(r->gpu_scene, vf->world, i, &instances[i]);
        if (!src->entity[i] || !(src->flags[i] & DE__RW_INST_SPRITE))
            continue;
        const DeSprite *sprite = &src->sprite[i];
        const de_affine *m = &src->transform[i];
        de_dvec3 position = {
            vf->world->origin.x + m->m[9] + m->m[0] * sprite->pivot.x + m->m[3] * sprite->pivot.y,
            vf->world->origin.y + m->m[10] + m->m[1] * sprite->pivot.x + m->m[4] * sprite->pivot.y,
            vf->world->origin.z + m->m[11] + m->m[2] * sprite->pivot.x + m->m[5] * sprite->pivot.y};
        de_dvec3 relative = {position.x - camera.x, position.y - camera.y, position.z - camera.z};
        de_vec2 ignored;
        de_dvec3 snapped = de__pixel_snap(relative, r->plane_2d, texel, &ignored);
        f32 delta[3] = {(f32)(snapped.x - relative.x), (f32)(snapped.y - relative.y),
                        (f32)(snapped.z - relative.z)};
        for (u32 axis = 0; axis < 3; ++axis) {
            instances[i].transform[axis].v[3] += delta[axis];
            instances[i].prev_transform[axis].v[3] += delta[axis];
            instances[i].aabb_min.v[axis] += delta[axis];
            instances[i].aabb_max.v[axis] += delta[axis];
            instances[i].sphere.v[axis] += delta[axis];
        }
    }
    de__gpu_scene_view_instances(r->gpu_scene, buffer);
}
de_result de__pixel_snapshots_create(de_renderer *r, de__pixel_snapshots **out) {
    *out = DE_NEW(r->alloc, de__pixel_snapshots, DE_MEM_RENDER);
    if (!*out)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "pixel snapshots");
    memset(*out, 0, sizeof **out);
    (*out)->r = r;
    return de__view_register_stage(r, DE__VIEW_SLOT_VISIBILITY, INT32_MIN, "pixel.snap", snap_stage,
                                   *out);
}
void de__pixel_snapshots_destroy(de_renderer *r, de__pixel_snapshots *owner) {
    if (!owner)
        return;
    de__view_unregister_stage(r, snap_stage, owner);
    for (u32 v = 0; v < DE__VIEW_MAX; ++v)
        for (u32 ring = 0; ring < DE__RENDER_MAX_FRAMES_IN_FLIGHT; ++ring)
            de_gpu_buffer_destroy(r->dev, owner->views[v][ring].buffer);
    DE_FREE(r->alloc, owner, sizeof *owner);
}
