#include "ui_internal.h"
#include <stddef.h>
#include <string.h>
#define AUTH     DE_FIELD_DEFAULT
#define READONLY DE_FIELD_READONLY
#define F(T, member, kind, flags, min, max, units, tip, nested, count)                             \
    DE_FIELD_EX(T, member, kind, flags, {min, max, units, tip, nested, count})
static de_result type(de_type_registry *registry, const de_type_desc *desc, de_type_id *out) {
    *out = de_type_find(registry, desc->name);
    if (out->id) {
        const de_type_desc *old = de_type_get(registry, *out);
        if (!old || old->size != desc->size || old->align != desc->align ||
            old->field_count != desc->field_count)
            return DE_ERR_ALREADY_EXISTS;
        return DE_OK;
    }
    return de_type_register_ex(registry, desc, out, NULL);
}
static de_result enumeration(de_type_registry *r, const char *name, const de_enum_value *values,
                             u32 count) {
    if (de_type_find(r, name).id)
        return DE_OK;
    return de_enum_register(r, name, values, count, false);
}
static void node_construct(void *ptr) {
    DeUiNode *n = ptr;
    memset(n, 0, sizeof *n);
    n->align_items = DE_UI_ALIGN_STRETCH;
    n->align_self = DE_UI_ALIGN_AUTO;
    n->shrink = 1;
}
static void item_construct(void *ptr) {
    DeUiGridItem *i = ptr;
    memset(i, 0, sizeof *i);
    i->column_span = i->row_span = 1;
}
static void style_construct(void *ptr) {
    DeUiStyle *s = ptr;
    memset(s, 0, sizeof *s);
    s->opacity = 1;
    s->text_color = (de_vec4){1, 1, 1, 1};
}
static void image_construct(void *ptr) {
    DeUiImage *i = ptr;
    memset(i, 0, sizeof *i);
    i->uv_rect = (de_vec4){0, 0, 1, 1};
    i->tint = (de_vec4){1, 1, 1, 1};
}
static void text_construct(void *ptr) {
    DeUiText *t = ptr;
    memset(t, 0, sizeof *t);
    t->style.size_px = 16;
    t->style.line_height = 1;
    t->style.color = (de_vec4){1, 1, 1, 1};
}
static const de_field_info len_fields[] = {
    F(de_ui_len, unit, DE_FK_ENUM, AUTH, 0, 3, NULL,
      "AUTO intrinsic, PX reference pixels, PERCENT parent size, FR grid only", "de_ui_unit", 0),
    F(de_ui_len, value, DE_FK_F32, AUTH, -100000, 100000, "px",
      "Value in the selected unit;100 percent fills parent", NULL, 0)};
static const de_field_info text_style_fields[] = {
    F(de_text_style, font, DE_FK_ASSET, AUTH | DE_FIELD_ASSET_REF, 0, 0, NULL, "Primary font",
      "font", 0),
    F(de_text_style, size_px, DE_FK_F32, AUTH, 1, 2048, "px", "Font em size in reference pixels",
      NULL, 0),
    F(de_text_style, color, DE_FK_COLOR, AUTH, 0, 1, NULL, "Linear glyph tint", NULL, 0),
    F(de_text_style, letter_spacing, DE_FK_F32, AUTH, -100, 100, "px", "Additional glyph advance",
      NULL, 0),
    F(de_text_style, line_height, DE_FK_F32, AUTH, 0, 10, NULL,
      "Font line-height multiplier;zero uses one", NULL, 0),
    F(de_text_style, outline_width, DE_FK_F32, AUTH, 0, 64, "px", "MSDF outline width", NULL, 0),
    F(de_text_style, outline_color, DE_FK_COLOR, AUTH, 0, 1, NULL, "Linear outline tint", NULL, 0),
    F(de_text_style, shadow_offset, DE_FK_FIXED_ARRAY, AUTH, -128, 128, "px", "Shadow x/y offset",
      "f32", 2),
    F(de_text_style, shadow_color, DE_FK_COLOR, AUTH, 0, 1, NULL, "Linear shadow tint", NULL, 0),
    F(de_text_style, disable_features, DE_FK_U32, AUTH, 0, 15, NULL, "DE_TEXT_DISABLE flags", NULL,
      0)};
static const de_field_info node_fields[] = {
    F(DeUiNode, display, DE_FK_ENUM, AUTH, 0, 2, NULL, "Flex, grid, or excluded from layout",
      "de_ui_display", 0),
    F(DeUiNode, direction, DE_FK_ENUM, AUTH, 0, 3, NULL, "Main axis and child order", "de_ui_dir",
      0),
    F(DeUiNode, justify, DE_FK_ENUM, AUTH, 0, 5, NULL, "Main-axis distribution", "de_ui_align", 0),
    F(DeUiNode, align_items, DE_FK_ENUM, AUTH, 0, 3, NULL, "Default cross-axis alignment",
      "de_ui_align", 0),
    F(DeUiNode, align_self, DE_FK_ENUM, AUTH, 0, 6, NULL, "Cross-axis override;AUTO inherits",
      "de_ui_align", 0),
    F(DeUiNode, width, DE_FK_STRUCT, AUTH, 0, 0, "px", "Preferred width", "de_ui_len", 0),
    F(DeUiNode, height, DE_FK_STRUCT, AUTH, 0, 0, "px", "Preferred height", "de_ui_len", 0),
    F(DeUiNode, min_width, DE_FK_STRUCT, AUTH, 0, 0, "px", "Minimum width", "de_ui_len", 0),
    F(DeUiNode, min_height, DE_FK_STRUCT, AUTH, 0, 0, "px", "Minimum height", "de_ui_len", 0),
    F(DeUiNode, max_width, DE_FK_STRUCT, AUTH, 0, 0, "px", "Maximum width;AUTO is unbounded",
      "de_ui_len", 0),
    F(DeUiNode, max_height, DE_FK_STRUCT, AUTH, 0, 0, "px", "Maximum height;AUTO is unbounded",
      "de_ui_len", 0),
    F(DeUiNode, margin, DE_FK_FIXED_ARRAY, AUTH, 0, 0, "px", "Top,right,bottom,left outer spacing",
      "de_ui_len", 4),
    F(DeUiNode, padding, DE_FK_FIXED_ARRAY, AUTH, 0, 0, "px", "Top,right,bottom,left content inset",
      "de_ui_len", 4),
    F(DeUiNode, gap, DE_FK_F32, AUTH, 0, 10000, "px", "Gap between flex children", NULL, 0),
    F(DeUiNode, grow, DE_FK_F32, AUTH, 0, 1000, NULL, "Share of positive free space", NULL, 0),
    F(DeUiNode, shrink, DE_FK_F32, AUTH, 0, 1000, NULL, "Shrink weight multiplied by basis", NULL,
      0),
    F(DeUiNode, style_class, DE_FK_NAME, AUTH, 0, 0, NULL, "Stylesheet class", NULL, 0),
    F(DeUiNode, z_order, DE_FK_I32, AUTH, -32768, 32767, NULL, "Sibling stacking order", NULL, 0),
    F(DeUiNode, clip, DE_FK_BOOL, AUTH, 0, 0, NULL, "Clip descendants to this rectangle", NULL, 0),
    F(DeUiNode, safe_area, DE_FK_BOOL, AUTH, 0, 0, NULL,
      "Inset this node into the physical safe area", NULL, 0),
    F(DeUiNode, computed_rect, DE_FK_VEC4, READONLY, 0, 0, "px",
      "Computed physical x,y,width,height;not serialized", NULL, 0)};
static const de_field_info anchor_fields[] = {F(DeUiAnchor, anchor_min, DE_FK_VEC2, AUTH, 0, 1,
                                                NULL, "Normalized minimum parent anchor", NULL, 0),
                                              F(DeUiAnchor, anchor_max, DE_FK_VEC2, AUTH, 0, 1,
                                                NULL, "Normalized maximum parent anchor", NULL, 0),
                                              F(DeUiAnchor, offset_min, DE_FK_VEC2, AUTH, -100000,
                                                100000, "px", "Minimum edge offset", NULL, 0),
                                              F(DeUiAnchor, offset_max, DE_FK_VEC2, AUTH, -100000,
                                                100000, "px", "Maximum edge offset", NULL, 0)};
static const de_field_info grid_fields[] = {
    F(DeUiGrid, columns, DE_FK_FIXED_ARRAY, AUTH, 0, 0, NULL,
      "Fixed,percent,auto or fractional columns", "de_ui_len", 8),
    F(DeUiGrid, column_count, DE_FK_U32, AUTH, 0, 8, NULL, "Zero uses one fractional column", NULL,
      0),
    F(DeUiGrid, rows, DE_FK_FIXED_ARRAY, AUTH, 0, 0, NULL, "Fixed,percent,auto or fractional rows",
      "de_ui_len", 8),
    F(DeUiGrid, row_count, DE_FK_U32, AUTH, 0, 8, NULL, "Zero derives automatic rows", NULL, 0),
    F(DeUiGrid, column_gap, DE_FK_F32, AUTH, 0, 10000, "px", "Column gap", NULL, 0),
    F(DeUiGrid, row_gap, DE_FK_F32, AUTH, 0, 10000, "px", "Row gap", NULL, 0)};
static const de_field_info item_fields[] = {
    F(DeUiGridItem, column, DE_FK_U8, AUTH, 0, 7, NULL, "Zero-based column", NULL, 0),
    F(DeUiGridItem, row, DE_FK_U8, AUTH, 0, 7, NULL, "Zero-based row", NULL, 0),
    F(DeUiGridItem, column_span, DE_FK_U8, AUTH, 1, 8, NULL, "Columns occupied", NULL, 0),
    F(DeUiGridItem, row_span, DE_FK_U8, AUTH, 1, 8, NULL, "Rows occupied", NULL, 0)};
static const de_field_info popup_fields[] = {
    F(DeUiPopup, anchor, DE_FK_ENTITY, AUTH, 0, 0, NULL,
      "Entity beside which overlay is positioned", NULL, 0),
    F(DeUiPopup, placement, DE_FK_ENUM, AUTH, 0, 4, NULL, "Preferred anchor edge",
      "de_ui_popup_placement", 0),
    F(DeUiPopup, modal, DE_FK_BOOL, AUTH, 0, 0, NULL,
      "Modal overlay;focus trapping is S29 focus behavior", NULL, 0)};
static const de_field_info style_fields[] = {
    F(DeUiStyle, background, DE_FK_COLOR, READONLY, 0, 1, NULL, "Resolved background", NULL, 0),
    F(DeUiStyle, border_color, DE_FK_COLOR, READONLY, 0, 1, NULL, "Resolved border", NULL, 0),
    F(DeUiStyle, border_width, DE_FK_F32, READONLY, 0, 64, "px", "Resolved border width", NULL, 0),
    F(DeUiStyle, corner_radius, DE_FK_F32, READONLY, 0, 1000, "px", "Resolved corner radius", NULL,
      0),
    F(DeUiStyle, opacity, DE_FK_F32, READONLY, 0, 1, NULL, "Resolved opacity", NULL, 0),
    F(DeUiStyle, text_color, DE_FK_COLOR, READONLY, 0, 1, NULL, "Resolved text color", NULL, 0)};
static const de_field_info state_fields[] = {F(DeUiInteractionState, hovered, DE_FK_BOOL, READONLY,
                                               0, 0, NULL, "Pointer is over node", NULL, 0),
                                             F(DeUiInteractionState, pressed, DE_FK_BOOL, READONLY,
                                               0, 0, NULL, "Pointer capture is pressed", NULL, 0),
                                             F(DeUiInteractionState, focused, DE_FK_BOOL, READONLY,
                                               0, 0, NULL, "Node owns keyboard focus", NULL, 0),
                                             F(DeUiInteractionState, disabled, DE_FK_BOOL, READONLY,
                                               0, 0, NULL, "Resolved disabled state", NULL, 0)};
static const de_field_info image_fields[] = {
    F(DeUiImage, texture, DE_FK_ASSET, AUTH | DE_FIELD_ASSET_REF, 0, 0, NULL, "Image texture",
      "texture", 0),
    F(DeUiImage, uv_rect, DE_FK_VEC4, AUTH, 0, 1, NULL, "UV minimum x/y,maximum x/y", NULL, 0),
    F(DeUiImage, nine_slice, DE_FK_VEC4, AUTH, 0, 10000, "px", "Nine-slice source borders", NULL,
      0),
    F(DeUiImage, tint, DE_FK_COLOR, AUTH, 0, 1, NULL, "Linear image tint", NULL, 0)};
static const de_field_info text_fields[] = {
    F(DeUiText, loc_id, DE_FK_NAME, AUTH, 0, 0, NULL, "Localized string ID", NULL, 0),
    F(DeUiText, style, DE_FK_STRUCT, AUTH, 0, 0, NULL, "Font and text formatting", "de_text_style",
      0),
    F(DeUiText, align, DE_FK_ENUM, AUTH, 0, 3, NULL, "Paragraph alignment", "de_text_align", 0),
    F(DeUiText, text, DE_FK_STRING, AUTH, 0, 0, NULL,
      "Authored NUL-terminated label;runtime literal override is not serialized", NULL, 0)};
static const de_field_info event_fields[] = {
    F(de_ui_event, kind, DE_FK_ENUM, AUTH, 0, 5, NULL, "UI event kind", "de_ui_event_kind", 0),
    F(de_ui_event, target, DE_FK_ENTITY, AUTH, 0, 0, NULL, "Target UI entity", NULL, 0),
    F(de_ui_event, action, DE_FK_NAME, AUTH, 0, 0, NULL, "Named game action", NULL, 0),
    F(de_ui_event, value, DE_FK_F32, AUTH, 0, 0, NULL, "Changed numeric value", NULL, 0)};
#define DESC(T, fields, hook)                                                                      \
    {#T,                                                                                           \
     sizeof(T),                                                                                    \
     _Alignof(T),                                                                                  \
     1,                                                                                            \
     fields,                                                                                       \
     (u32)(sizeof(fields) / sizeof((fields)[0])),                                                  \
     {.construct = hook},                                                                          \
     true}
de_result de_ui__reflect(de_ui *ui, de_world *world) {
    de_type_registry *r = de_world_types(world);
    static const de_enum_value units[] = {
        {"AUTO", DE_UI_AUTO}, {"PX", DE_UI_PX}, {"PERCENT", DE_UI_PERCENT}, {"FR", DE_UI_FR}};
    static const de_enum_value displays[] = {
        {"FLEX", DE_UI_FLEX}, {"GRID", DE_UI_GRID}, {"NONE", DE_UI_NONE}};
    static const de_enum_value dirs[] = {{"ROW", DE_UI_ROW},
                                         {"COLUMN", DE_UI_COLUMN},
                                         {"ROW_REVERSE", DE_UI_ROW_REVERSE},
                                         {"COLUMN_REVERSE", DE_UI_COLUMN_REVERSE}};
    static const de_enum_value aligns[] = {{"START", DE_UI_ALIGN_START},
                                           {"CENTER", DE_UI_ALIGN_CENTER},
                                           {"END", DE_UI_ALIGN_END},
                                           {"STRETCH", DE_UI_ALIGN_STRETCH},
                                           {"SPACE_BETWEEN", DE_UI_ALIGN_SPACE_BETWEEN},
                                           {"SPACE_AROUND", DE_UI_ALIGN_SPACE_AROUND},
                                           {"AUTO", DE_UI_ALIGN_AUTO}};
    static const de_enum_value placements[] = {
        {"BOTTOM", 0}, {"TOP", 1}, {"LEFT", 2}, {"RIGHT", 3}, {"OVERLAY", 4}};
    static const de_enum_value text_aligns[] = {{"START", DE_ALIGN_START},
                                                {"CENTER", DE_ALIGN_CENTER},
                                                {"END", DE_ALIGN_END},
                                                {"JUSTIFY", DE_ALIGN_JUSTIFY}};
    static const de_enum_value events[] = {
        {"CLICK", DE_UI_EV_CLICK}, {"CHANGED", DE_UI_EV_CHANGED}, {"SUBMIT", DE_UI_EV_SUBMIT},
        {"FOCUS", DE_UI_EV_FOCUS}, {"BLUR", DE_UI_EV_BLUR},       {"HOVER", DE_UI_EV_HOVER}};
    DE_TRY(enumeration(r, "de_ui_unit", units, 4));
    DE_TRY(enumeration(r, "de_ui_display", displays, 3));
    DE_TRY(enumeration(r, "de_ui_dir", dirs, 4));
    DE_TRY(enumeration(r, "de_ui_align", aligns, 7));
    DE_TRY(enumeration(r, "de_ui_popup_placement", placements, 5));
    DE_TRY(enumeration(r, "de_text_align", text_aligns, 4));
    DE_TRY(enumeration(r, "de_ui_event_kind", events, 6));
    de_type_id id;
    de_type_desc length = DESC(de_ui_len, len_fields, NULL),
                 style = DESC(de_text_style, text_style_fields, NULL);
    DE_TRY(type(r, &length, &id));
    DE_TRY(type(r, &style, &id));
    const de_type_desc descriptions[] = {DESC(DeUiNode, node_fields, node_construct),
                                         DESC(DeUiAnchor, anchor_fields, NULL),
                                         DESC(DeUiGrid, grid_fields, NULL),
                                         DESC(DeUiGridItem, item_fields, item_construct),
                                         DESC(DeUiPopup, popup_fields, NULL),
                                         DESC(DeUiStyle, style_fields, style_construct),
                                         DESC(DeUiInteractionState, state_fields, NULL),
                                         DESC(DeUiImage, image_fields, image_construct),
                                         DESC(DeUiText, text_fields, text_construct)};
    de_component *components[] = {&ui->ids.node,      &ui->ids.anchor, &ui->ids.grid,
                                  &ui->ids.grid_item, &ui->ids.popup,  &ui->ids.style,
                                  &ui->ids.state,     &ui->ids.image,  &ui->ids.text};
    for (u32 i = 0; i < DE_UI_COMPONENTS; ++i) {
        DE_TRY(type(r, &descriptions[i], &id));
        *components[i] = de_component_register(world, id, DE_COMP_SNAPSHOT | DE_COMP_SERIALIZE);
        if (!components[i]->id)
            return DE_ERR_INTERNAL;
    }
    de_type_desc resource = {.name = "de_ui_resource",
                             .size = sizeof(de_ui_resource),
                             .align = _Alignof(de_ui_resource),
                             .version = 1,
                             .is_pod = true};
    DE_TRY(type(r, &resource, &id));
    de_ui_resource value = {ui};
    ui->resource = de_resource_insert(world, id, &value);
    if (!ui->resource.id)
        return DE_ERR_INTERNAL;
    de_type_desc event = DESC(de_ui_event, event_fields, NULL);
    DE_TRY(type(r, &event, &id));
    ui->event = de_event_register(world, id);
    if (!ui->event.id)
        return DE_ERR_INTERNAL;
    return DE_OK;
}
