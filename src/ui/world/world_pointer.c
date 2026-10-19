#include "world_internal.h"
#include "ui/widgets/widgets_internal.h"
#include "ui/render/draw_list.h"
#include <math.h>
bool de_ui__world_intersect(const DeUiWorldPanel *panel, const DeGlobalTransform *transform,
                            de_dvec3 origin, de_vec3 direction, de_vec2 *pixel, f32 *distance) {
    if (!panel || !transform || !pixel || !distance || !isfinite(panel->pixels_per_meter) ||
        panel->pixels_per_meter <= 0 || !isfinite(panel->size_m.x) || !isfinite(panel->size_m.y) ||
        panel->size_m.x <= 0 || panel->size_m.y <= 0)
        return false;
    /* Subtract doubles before conversion; the panel may be 100 km from the world origin. */
    const de_vec3 relative = de_dv3_relative(origin, transform->position);
    de_quat inverse = {-transform->rotation.x, -transform->rotation.y, -transform->rotation.z,
                       transform->rotation.w};
    de_vec3 local_origin = de_quat_rotate(inverse, relative);
    de_vec3 local_direction = de_quat_rotate(inverse, direction);
    const de_vec3 scale = transform->scale;
    if (!isfinite(scale.x) || !isfinite(scale.y) || !isfinite(scale.z) || fabsf(scale.x) < 1e-8f ||
        fabsf(scale.y) < 1e-8f || fabsf(scale.z) < 1e-8f)
        return false;
    local_origin.x /= scale.x;
    local_origin.y /= scale.y;
    local_origin.z /= scale.z;
    local_direction.x /= scale.x;
    local_direction.y /= scale.y;
    local_direction.z /= scale.z;
    if (!isfinite(local_direction.z) || fabsf(local_direction.z) < 1e-7f)
        return false;
    const f32 t = -local_origin.z / local_direction.z;
    if (!isfinite(t) || t < 0)
        return false;
    const f32 x = local_origin.x + t * local_direction.x;
    const f32 y = local_origin.y + t * local_direction.y;
    pixel->x = (x + panel->size_m.x * .5f) * panel->pixels_per_meter;
    pixel->y = (panel->size_m.y * .5f - y) * panel->pixels_per_meter;
    *distance = t;
    return isfinite(pixel->x) && isfinite(pixel->y);
}
bool de_ui__world_camera_ray(const de_view_frame_info *camera, de_vec2 physical, de_dvec3 *origin,
                             de_vec3 *direction) {
    de_vec4 viewport = camera->viewport_px;
    de_vec4 content = camera->content_px;
    f32 x = physical.x - viewport.x, y = physical.y - viewport.y;
    if (camera->pixel_art) {
        x -= content.x;
        y -= content.y;
    }
    f32 width = camera->pixel_art ? content.z : viewport.z;
    f32 height = camera->pixel_art ? content.w : viewport.w;
    if (!(width > 0) || !(height > 0))
        return false;
    f32 nx = x / width * 2 - 1, ny = 1 - y / height * 2;
    de_mat4 inverse = de_m4_inverse(&camera->view_projection);
    de_vec4 near = de_m4_mul_v4(&inverse, (de_vec4){nx, ny, 1, 1});
    de_vec4 far = de_m4_mul_v4(&inverse, (de_vec4){nx, ny, .001f, 1});
    if (!isfinite(near.w) || !isfinite(far.w) || fabsf(near.w) < 1e-8f || fabsf(far.w) < 1e-8f)
        return false;
    de_vec3 a = {near.x / near.w, near.y / near.w, near.z / near.w};
    de_vec3 b = {far.x / far.w, far.y / far.w, far.z / far.w};
    de_vec3 d = {b.x - a.x, b.y - a.y, b.z - a.z};
    f32 length = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (!(length > 0) || !isfinite(length))
        return false;
    *origin = (de_dvec3){camera->render_origin.x + a.x, camera->render_origin.y + a.y,
                         camera->render_origin.z + a.z};
    *direction = (de_vec3){d.x / length, d.y / length, d.z / length};
    return true;
}
bool de_ui__world_pointer(de_ui *ui, de_vec2 physical, bool pressed, bool released, bool down) {
    if (!ui || !ui->render || !ui->widgets)
        return false;
    de_ui_pointer_state *pointer = &ui->widgets->pointer;
    bool world_capture = pointer->captured.bits && pointer->root.bits;
    if (!world_capture &&
        de_ui__hit_test(ui, (de_entity){0}, de_ui__screen_pointer(ui, physical)).bits)
        return false;
    de_view_frame_info camera;
    if (!de_ui__world_camera(ui, &camera))
        return false;
    de_dvec3 origin;
    de_vec3 direction;
    if (!de_ui__world_camera_ray(&camera, physical, &origin, &direction))
        return false;
    struct de_ui_render *r = ui->render;
    de_component panel_type = de_component_find(ui->world, "DeUiWorldPanel");
    de_entity root = {0};
    de_vec2 pixel = {0};
    f32 closest = INFINITY;
    for (u32 i = 0; i < r->build.root_count; ++i) {
        const de_ui_root_list *candidate = &r->build.roots[i];
        if (!candidate->world || (world_capture && candidate->entity.bits != pointer->root.bits))
            continue;
        const DeUiWorldPanel *panel = de_entity_get_const(ui->world, candidate->entity, panel_type);
        const DeGlobalTransform *transform =
            de_entity_get_const(ui->world, candidate->entity, ui->hierarchy.global_transform);
        de_vec2 point;
        f32 distance;
        if (!panel || !transform ||
            !de_ui__world_intersect(panel, transform, origin, direction, &point, &distance))
            continue;
        if (!world_capture &&
            (point.x < 0 || point.y < 0 || point.x >= panel->size_m.x * panel->pixels_per_meter ||
             point.y >= panel->size_m.y * panel->pixels_per_meter))
            continue;
        if (distance < closest) {
            closest = distance;
            root = candidate->entity;
            pixel = point;
        }
    }
    if (!root.bits)
        return false;
    de_ui__pointer_route(ui, root, pixel, pressed, released, down);
    return pointer->consumed || world_capture;
}
