#ifndef DE_UI_WORLD_INTERNAL_H
#define DE_UI_WORLD_INTERNAL_H
#include "ui/ui_internal.h"
struct de_ui_render;
struct de_ui_draw_list;
de_result de_ui__world_init(struct de_ui_render *render);
de_result de_ui__world_attach(struct de_ui_render *render);
void de_ui__world_destroy(struct de_ui_render *render);
void de_ui__world_extract(struct de_ui_render *render, struct de_ui_draw_list *packet);
bool de_ui__world_camera(de_ui *ui, de_view_frame_info *out);
bool de_ui__world_root_extent(de_ui *ui, de_entity root, de_vec4 *rect);
bool de_ui__world_intersect(const DeUiWorldPanel *panel, const DeGlobalTransform *transform,
                            de_dvec3 origin, de_vec3 direction, de_vec2 *pixel, f32 *distance);
bool de_ui__world_camera_ray(const de_view_frame_info *camera, de_vec2 physical, de_dvec3 *origin,
                             de_vec3 *direction);
bool de_ui__world_pointer(de_ui *ui, de_vec2 physical, bool pressed, bool released, bool down);
bool de_ui__pixel_prepare(de_ui *ui);
bool de_ui__pixel_extract(de_ui *ui);
de_vec2 de_ui__screen_pointer(de_ui *ui, de_vec2 physical);
void de_ui__pixel_snap_rect(de_vec4 *rect);
#endif
