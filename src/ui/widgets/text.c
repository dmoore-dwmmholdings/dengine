#include "widgets_internal.h"
#include "ui/style/stylesheet_internal.h"
#include "ui_default_style.h"
#include <math.h>
#include <string.h>
/* Private ownership record: an authored selector must survive content/behavior changes. */
typedef struct de_ui_auto_widget {
    de_ui_widget_type type;
} de_ui_auto_widget;
static void slider_construct(void *ptr) {
    memset(ptr, 0, sizeof(DeUiSlider));
    ((DeUiSlider *)ptr)->max = 1;
    ((DeUiSlider *)ptr)->step = .1f;
}
static de_component component(de_ui *ui, const char *name, u32 size, u32 align,
                              const de_field_info *fields, u32 count, void (*construct)(void *),
                              bool authored) {
    de_type_registry *types = de_world_types(ui->world);
    de_type_id type = de_type_find(types, name);
    if (!type.id && de_type_register_ex(types,
                                        &(de_type_desc){.name = name,
                                                        .size = size,
                                                        .align = align,
                                                        .version = 1,
                                                        .is_pod = true,
                                                        .fields = fields,
                                                        .field_count = count,
                                                        .hooks = {.construct = construct}},
                                        &type, NULL) != DE_OK)
        return (de_component){0};
    return de_component_register(ui->world, type,
                                 authored ? DE_COMP_SERIALIZE | DE_COMP_SNAPSHOT : 0);
}
#define F(T, field, kind, lo, hi, units, tip)                                                      \
    DE_FIELD_EX(T, field, kind, DE_FIELD_DEFAULT, {lo, hi, units, tip, NULL, 0})
static de_result reflect_widgets(de_ui *ui) {
    static const de_field_info buttons[] = {
        F(DeUiButton, disabled, DE_FK_BOOL, 0, 1, NULL,
          "Disable all pointer and submit activation"),
        F(DeUiButton, on_click, DE_FK_NAME, 0, 0, NULL, "CLICK action name")};
    static const de_field_info toggles[] = {
        F(DeUiToggle, on, DE_FK_BOOL, 0, 1, NULL, "Selected toggle state")};
    static const de_field_info sliders[] = {
        F(DeUiSlider, value, DE_FK_F32, 0, 0, NULL, "Current clamped value"),
        F(DeUiSlider, min, DE_FK_F32, 0, 0, NULL, "Minimum"),
        F(DeUiSlider, max, DE_FK_F32, 0, 0, NULL, "Maximum"),
        F(DeUiSlider, step, DE_FK_F32, 0, 0, NULL, "Zero means continuous drag")};
    static const de_field_info progress[] = {
        F(DeUiProgress, value, DE_FK_F32, 0, 1, NULL, "Determinate fraction"),
        F(DeUiProgress, indeterminate, DE_FK_BOOL, 0, 1, NULL, "Animate without a known fraction")};
    de_ui_widgets *w = ui->widgets;
    w->button = component(ui, "DeUiButton", sizeof(DeUiButton), _Alignof(DeUiButton), buttons, 2,
                          NULL, true);
    w->toggle = component(ui, "DeUiToggle", sizeof(DeUiToggle), _Alignof(DeUiToggle), toggles, 1,
                          NULL, true);
    w->slider = component(ui, "DeUiSlider", sizeof(DeUiSlider), _Alignof(DeUiSlider), sliders, 4,
                          slider_construct, true);
    w->progress = component(ui, "DeUiProgress", sizeof(DeUiProgress), _Alignof(DeUiProgress),
                            progress, 2, NULL, true);
    w->auto_widget = component(ui, "de_ui_auto_widget", sizeof(de_ui_auto_widget),
                               _Alignof(de_ui_auto_widget), NULL, 0, NULL, false);
    return w->button.id && w->toggle.id && w->slider.id && w->progress.id && w->auto_widget.id
               ? DE_OK
               : DE_ERR_INTERNAL;
}
#undef F
bool de_ui__widget_enabled(de_ui *ui, de_entity entity) {
    if (!ui || !ui->widgets || !ui->world || !de_entity_alive(ui->world, entity))
        return false;
    for (u32 depth = 0; entity.bits && depth < ui->capacity; ++depth) {
        const DeUiNode *node = de_entity_get_const(ui->world, entity, ui->ids.node);
        if (node && node->display == DE_UI_NONE)
            return false;
        const DeUiButton *button = de_entity_get_const(ui->world, entity, ui->widgets->button);
        const DeUiInteractionState *state = de_entity_get_const(ui->world, entity, ui->ids.state);
        if ((button && button->disabled) || (state && state->disabled))
            return false;
        entity = de_entity_parent(ui->world, entity);
    }
    return !entity.bits;
}
de_ui_widget_table *de_ui__widget_table(de_ui *ui) {
    return ui && ui->widgets ? &ui->widgets->table : NULL;
}
static void collect(const de_query_chunk *chunk, void *user) {
    de_ui_widgets *w = user;
    if (!chunk->count)
        return;
    /* A whole archetype slice can be skipped when it has no widget/content.
     * Keep automatic tags so removing the last authored component clears them. */
    const de_component relevant[] = {w->button,       w->toggle,        w->slider,     w->progress,
                                     w->ui->ids.text, w->ui->ids.image, w->auto_widget};
    bool found = false;
    for (u32 i = 0; i < sizeof relevant / sizeof relevant[0]; ++i)
        if (relevant[i].id && de_entity_has(w->ui->world, chunk->entities[0], relevant[i])) {
            found = true;
            break;
        }
    if (!found)
        return;
    for (u32 i = 0; i < chunk->count && w->entity_count < w->ui->capacity; ++i)
        w->entities[w->entity_count++] = chunk->entities[i];
}
static void widget_tag(de_ui *ui, de_entity e, de_ui_widget_type type, bool found) {
    de_component owner = ui->widgets->auto_widget, widget = ui->style->widget;
    const DeUiWidgetType *old = de_entity_get_const(ui->world, e, widget);
    const de_ui_auto_widget *automatic = de_entity_get_const(ui->world, e, owner);
    if (old && (!automatic || old->type != automatic->type)) {
        /* A pre-existing or explicitly retagged selector belongs to the author. */
        if (automatic)
            de_entity_remove(ui->world, e, owner);
        return;
    }
    if (!found) {
        if (automatic) {
            de_entity_remove(ui->world, e, widget);
            de_entity_remove(ui->world, e, owner);
        }
        return;
    }
    if (old && old->type == type)
        return;
    /* Add ownership first: structural changes may invalidate component pointers. */
    de_ui_auto_widget *tag_owner = de_entity_add(ui->world, e, owner);
    if (!tag_owner)
        return;
    DeUiWidgetType *tag = de_entity_add(ui->world, e, widget);
    if (tag) {
        tag->type = type;
        tag_owner = de_entity_get(ui->world, e, owner);
        tag_owner->type = type;
    }
}
static void widget_update(de_system_ctx *ctx) {
    de_ui_widgets *w = ctx->user;
    de_ui *ui = w->ui;
    if (isfinite(ctx->dt) && ctx->dt > 0)
        w->time = fmodf(w->time + ctx->dt, 3600);
    w->entity_count = 0;
    de_query_each(ui->world, ctx->queries[0], collect, w);
    for (u32 i = 0; i < w->entity_count; ++i) {
        de_entity e = w->entities[i];
        de_ui_widget_type type = DE_UI_WIDGET_PANEL;
        bool found = true;
        if (de_entity_has(ui->world, e, w->button))
            type = DE_UI_WIDGET_BUTTON;
        else if (de_entity_has(ui->world, e, w->toggle))
            type = DE_UI_WIDGET_TOGGLE;
        else if (de_entity_has(ui->world, e, w->slider))
            type = DE_UI_WIDGET_SLIDER;
        else if (de_entity_has(ui->world, e, w->progress))
            type = DE_UI_WIDGET_PROGRESS;
        else if (de_entity_has(ui->world, e, ui->ids.text))
            type = DE_UI_WIDGET_TEXT;
        else if (de_entity_has(ui->world, e, ui->ids.image))
            type = DE_UI_WIDGET_IMAGE;
        else
            found = false;
        widget_tag(ui, e, type, found);
        if (found && type == DE_UI_WIDGET_BUTTON) {
            const DeUiButton *button = de_entity_get_const(ui->world, e, w->button);
            const DeUiInteractionState *old_state =
                de_entity_get_const(ui->world, e, ui->ids.state);
            if (!old_state || old_state->disabled != button->disabled) {
                DeUiInteractionState *state = old_state
                                                  ? de_entity_get(ui->world, e, ui->ids.state)
                                                  : de_entity_add(ui->world, e, ui->ids.state);
                if (state) {
                    state->disabled = button->disabled;
                    if (button->disabled)
                        state->hovered = state->pressed = false;
                }
            }
        }
    }
}
de_result de_ui__widgets_init(de_ui *ui) {
    de_ui_widgets *w = DE_NEW(ui->alloc, de_ui_widgets, DE_MEM_UI);
    if (!w)
        return DE_ERR_OUT_OF_MEMORY;
    memset(w, 0, sizeof *w);
    w->ui = ui;
    ui->widgets = w;
    w->entities = DE_ALLOC(ui->alloc, (u64)ui->capacity * sizeof *w->entities, _Alignof(de_entity),
                           DE_MEM_UI);
    if (!w->entities)
        return DE_ERR_OUT_OF_MEMORY;
    w->table.entries[DE_UI_WIDGET_BUTTON].submit = de_ui__button_submit;
    w->table.entries[DE_UI_WIDGET_TOGGLE].submit = de_ui__toggle_submit;
    w->table.entries[DE_UI_WIDGET_SLIDER].increment = de_ui__slider_increment;
    w->table.entries[DE_UI_WIDGET_SLIDER].decrement = de_ui__slider_decrement;
    return de_ui__load_stylesheet_source(ui, (const char *)de_ui_default_style,
                                         sizeof de_ui_default_style - 1, true);
}
void de_ui__widgets_detach(de_ui *ui) {
    if (!ui->widgets)
        return;
    de_ui_widgets *w = ui->widgets;
    if (ui->world && w->update_system.id)
        de_system_remove(ui->world, w->update_system);
    if (ui->world && w->pointer_system.id)
        de_system_remove(ui->world, w->pointer_system);
    w->update_system = w->pointer_system = (de_system){0};
    w->pointer = (de_ui_pointer_state){0};
}
void de_ui__widgets_shutdown(de_ui *ui) {
    if (!ui->widgets)
        return;
    de_ui__widgets_detach(ui);
    de_ui_widgets *w = ui->widgets;
    if (w->entities)
        DE_FREE(ui->alloc, w->entities, (u64)ui->capacity * sizeof *w->entities);
    DE_FREE(ui->alloc, w, sizeof *w);
    ui->widgets = NULL;
}
void de_ui__widgets_attach(de_ui *ui) {
    if (!ui->widgets || reflect_widgets(ui) != DE_OK)
        return;
    de_query_term term = {ui->ids.node, DE_READ};
    de_query_desc query = {.terms = &term, .term_count = 1};
    const char *before[] = {DE_UI_STYLE_SYSTEM, DE_UI_LAYOUT_SYSTEM};
    ui->widgets->update_system = de_system_add(
        ui->world, &(de_system_desc){.name = "ui.widgets",
                                     .schedule = DE_SCHEDULE_POST_UPDATE,
                                     .fn = widget_update,
                                     .user = ui->widgets,
                                     .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE,
                                     .queries = &query,
                                     .query_count = 1,
                                     .before = before,
                                     .before_count = 2});
    ui->widgets->pointer_system = de_system_add(
        ui->world, &(de_system_desc){.name = "ui.pointer",
                                     .schedule = DE_SCHEDULE_PRE_UPDATE,
                                     .fn = de_ui__pointer_system,
                                     .user = ui->widgets,
                                     .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE});
    de_ui__image_register(ui);
    de_ui__widgets_register_draw(ui);
}
