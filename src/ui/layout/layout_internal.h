#ifndef DE_UI_LAYOUT_INTERNAL_H
#define DE_UI_LAYOUT_INTERNAL_H
#include "ui/ui_internal.h"
typedef struct de_ui_layout {
    de_ui *ui;
    de_allocator allocator;
    bool world_root;
} de_ui_layout;
f32 de_ui__length(de_ui_len length, f32 available, f32 scale, f32 automatic);
de_vec2 de_ui__measure(de_ui_layout *layout, u32 index, de_vec2 available);
/* Assigned outer dimensions (bit0 width, bit1 height) have already passed flex distribution. */
de_vec2 de_ui__measure_assigned(de_ui_layout *layout, u32 index, de_vec2 available,
                                de_vec2 assigned, u32 axes);
void de_ui__arrange(de_ui_layout *layout, u32 index, de_vec4 rect, bool force);
void de_ui__flex(de_ui_layout *layout, u32 index, de_vec4 content);
void de_ui__grid(de_ui_layout *layout, u32 index, de_vec4 content);
de_vec2 de_ui__grid_measure(de_ui_layout *layout, u32 index, de_vec2 available);
bool de_ui__absolute(de_ui_layout *layout, u32 index, de_vec4 parent, de_vec4 *rect);
void de_ui__popups(de_ui_layout *layout);
de_vec2 de_ui__measure_text(de_ui *ui, de_entity entity, de_vec2 available);
de_vec2 de_ui__measure_image(de_ui *ui, de_entity entity, de_vec2 available);
#endif
