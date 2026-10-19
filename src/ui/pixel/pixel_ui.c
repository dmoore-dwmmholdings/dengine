#include "ui/world/world_internal.h"
#include "ui/render/draw_list.h"
#include <math.h>
#include <string.h>
bool de_ui__pixel_prepare(de_ui *ui) {
    if ((!ui->desc.pixel_art && !ui->desc.view.id) || !ui->render)
        return false;
    struct de_ui_render *r = ui->render;
    r->pixel_layout_valid = false;
#if DE_HAS_RENDER
    if (ui->desc.renderer) {
        de_view_frame_info info;
        if (!de_view_layout_info(ui->desc.renderer, ui->desc.view, &info) ||
            (ui->desc.pixel_art && (!info.pixel_art || !info.pixel.integer_scale_only)))
            return false;
        r->pixel_layout = info;
        r->pixel_layout_valid = true;
        u32 width = ui->desc.pixel_art ? info.pixel.internal_width : info.output_width,
            height = ui->desc.pixel_art ? info.pixel.internal_height : info.output_height;
        if (ui->width != width || ui->height != height)
            ui->viewport_dirty = true;
        ui->width = width;
        ui->height = height;
        /* Letterboxes are outside the root's internal grid; safe insets cannot create fractions. */
        ui->safe = (de_window_insets){0};
        return true;
    }
#endif
    return false;
}
bool de_ui__pixel_extract(de_ui *ui) {
    if (!ui->desc.pixel_art && !ui->desc.view.id)
        return true;
#if DE_HAS_RENDER
    if (ui->desc.renderer) {
        struct de_ui_render *r = ui->render;
        de_view view = ui->desc.view.id ? ui->desc.view : de_renderer_main_view(ui->desc.renderer);
        de_view_frame_info exact;
        if (!r->pixel_layout_valid ||
            !de_renderer_extract_view_info(ui->desc.renderer, view, &exact))
            return false;
        const de_view_frame_info *built = &r->pixel_layout;
        return (!ui->desc.pixel_art ||
                (exact.pixel_art && exact.pixel.integer_scale_only &&
                 exact.pixel.internal_width == built->pixel.internal_width &&
                 exact.pixel.internal_height == built->pixel.internal_height)) &&
               exact.output_width == built->output_width &&
               exact.output_height == built->output_height &&
               memcmp(&exact.viewport_px, &built->viewport_px, sizeof exact.viewport_px) == 0;
    }
#endif
    return true;
}
void de_ui__pixel_snap_rect(de_vec4 *rect) {
    f32 right = roundf(rect->x + rect->z), bottom = roundf(rect->y + rect->w);
    rect->x = roundf(rect->x);
    rect->y = roundf(rect->y);
    rect->z = fmaxf(0, right - rect->x);
    rect->w = fmaxf(0, bottom - rect->y);
}

de_vec2 de_ui__screen_pointer(de_ui *ui, de_vec2 physical) {
    struct de_ui_render *r = ui ? ui->render : NULL;
    if (!r || !r->pixel_layout_valid || (!ui->desc.pixel_art && !ui->desc.view.id))
        return physical;
    const de_view_frame_info *info = &r->pixel_layout;
    physical.x -= info->viewport_px.x;
    physical.y -= info->viewport_px.y;
    if (ui->desc.pixel_art) {
        f32 sx = info->content_px.z / info->pixel.internal_width;
        f32 sy = info->content_px.w / info->pixel.internal_height;
        if (!(sx > 0) || !(sy > 0))
            return (de_vec2){-1, -1};
        physical.x = (physical.x - info->content_px.x) / sx;
        physical.y = (physical.y - info->content_px.y) / sy;
        de_view_frame_info camera;
        if (info->pixel.smooth_subpixel_scroll && de_ui__world_camera(ui, &camera)) {
            physical.x += camera.pixel_residual.x;
            physical.y -= camera.pixel_residual.y;
        }
    } else {
        physical.x *= info->output_width / info->viewport_px.z;
        physical.y *= info->output_height / info->viewport_px.w;
    }
    return physical;
}
