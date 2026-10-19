#include "focus_internal.h"
#include <string.h>
/* All production device bindings live in a normal, rebindable public action map. Capture is
 * FixedUpdate-only because S21 replay publishes its resolved actions in the tick view. */
#define B(source_, code_) {.source = source_, .code = code_, .composite_slot = DE_COMPOSITE_NONE}
static de_action add(de_input *in, de_action_map map, const char *name, de_action_type type,
                     const de_binding *bindings, u32 count) {
    return de_action_add(in, map,
                         &(de_action_desc){.name = name,
                                           .type = type,
                                           .bindings = bindings,
                                           .binding_count = count,
                                           .rebindable = true});
}
static void defaults(de_input *in, de_action_map map) {
    static const de_binding nav[] = {
        {.source = DE_BIND_KEY, .code = DE_KEY_UP, .composite_slot = DE_COMPOSITE_UP},
        {.source = DE_BIND_KEY, .code = DE_KEY_DOWN, .composite_slot = DE_COMPOSITE_DOWN},
        {.source = DE_BIND_KEY, .code = DE_KEY_LEFT, .composite_slot = DE_COMPOSITE_LEFT},
        {.source = DE_BIND_KEY, .code = DE_KEY_RIGHT, .composite_slot = DE_COMPOSITE_RIGHT},
        {.source = DE_BIND_PAD_BUTTON, .code = DE_PAD_DPAD_UP, .composite_slot = DE_COMPOSITE_UP},
        {.source = DE_BIND_PAD_BUTTON,
         .code = DE_PAD_DPAD_DOWN,
         .composite_slot = DE_COMPOSITE_DOWN},
        {.source = DE_BIND_PAD_BUTTON,
         .code = DE_PAD_DPAD_LEFT,
         .composite_slot = DE_COMPOSITE_LEFT},
        {.source = DE_BIND_PAD_BUTTON,
         .code = DE_PAD_DPAD_RIGHT,
         .composite_slot = DE_COMPOSITE_RIGHT},
        {.source = DE_BIND_PAD_AXIS,
         .code = DE_BIND_LEFT_STICK,
         .composite_slot = DE_COMPOSITE_NONE,
         .deadzone = .2f}};
    static const de_binding submit[] = {B(DE_BIND_PAD_BUTTON, DE_PAD_SOUTH),
                                        B(DE_BIND_KEY, DE_KEY_RETURN),
                                        B(DE_BIND_KEY, DE_KEY_SPACE)};
    static const de_binding cancel[] = {B(DE_BIND_PAD_BUTTON, DE_PAD_EAST),
                                        B(DE_BIND_KEY, DE_KEY_ESCAPE)};
    static const de_binding next[] = {B(DE_BIND_PAD_BUTTON, DE_PAD_RSHOULDER),
                                      B(DE_BIND_KEY, DE_KEY_TAB)};
    static const de_binding previous[] = {B(DE_BIND_PAD_BUTTON, DE_PAD_LSHOULDER),
                                          {.source = DE_BIND_KEY,
                                           .code = DE_KEY_TAB,
                                           .composite_slot = DE_COMPOSITE_NONE,
                                           .modifier_key = DE_KEY_LSHIFT},
                                          {.source = DE_BIND_KEY,
                                           .code = DE_KEY_TAB,
                                           .composite_slot = DE_COMPOSITE_NONE,
                                           .modifier_key = DE_KEY_RSHIFT}};
    static const de_binding scroll[] = {{.source = DE_BIND_PAD_AXIS,
                                         .code = DE_BIND_RIGHT_STICK,
                                         .composite_slot = DE_COMPOSITE_NONE,
                                         .deadzone = .2f}};
    static const de_binding pointer[] = {B(DE_BIND_MOUSE_DELTA, DE_BIND_AXIS_XY)};
    (void)add(in, map, "navigate", DE_ACTION_AXIS2D, nav, 9);
    (void)add(in, map, "submit", DE_ACTION_BUTTON, submit, 3);
    (void)add(in, map, "cancel", DE_ACTION_BUTTON, cancel, 2);
    (void)add(in, map, "tab_next", DE_ACTION_BUTTON, next, 2);
    (void)add(in, map, "tab_prev", DE_ACTION_BUTTON, previous, 3);
    (void)add(in, map, "scroll", DE_ACTION_AXIS2D, scroll, 1);
    (void)add(in, map, "pointer_motion", DE_ACTION_AXIS2D, pointer, 1);
}
#undef B
void de_ui__actions_context(de_ui *ui) {
    de_ui_focus *f = ui->focus;
    de_input *in = ui->desc.input;
    if (!in || !f || !f->map.id)
        return;
    if (f->mode == DE_UI_INPUT_NONE) {
        if (f->owns_push) {
            de_action_map_pop(in, f->map);
            f->owns_push = false;
        }
        de_action_map_set_flags(in, f->map, f->old_flags);
        return;
    }
    de_action_map_set_flags(in, f->map,
                            f->scope.bits || f->mode == DE_UI_INPUT_MODAL
                                ? DE_INPUT_CONTEXT_BLOCKS_LOWER
                                : DE_INPUT_CONTEXT_PASSTHROUGH);
    if (!de_action_map_active(in, f->map) && !f->owns_push) {
        de_action_map_push(in, f->map);
        f->owns_push = true;
    }
}
static void capture(de_system_ctx *ctx) {
    de_ui_focus *f = ctx->user;
    if (f->mode == DE_UI_INPUT_NONE)
        return;
    de_input *in = f->ui->desc.input;
    de_ui_nav_sample sample = {.navigate = de_action_value2(in, f->navigate),
                               .pointer = de_action_value2(in, f->pointer),
                               .dt = ctx->dt,
                               .submit = de_action_triggered(in, f->submit),
                               .cancel = de_action_triggered(in, f->cancel),
                               .next = de_action_triggered(in, f->next),
                               .previous = de_action_triggered(in, f->previous)};
    /* Fixed storage allows thousands of catch-up ticks without allocating on the frame path.
     * A saturated host must drain PostUpdate before accepting another UI tick. */
    if (f->sample_count < f->sample_capacity)
        f->samples[f->sample_count++] = sample;
    else
        DE_ERROR(DE_LOG_CAT_GENERAL,
                 "UI navigation tick queue full; run PostUpdate between catch-up batches");
}
void de_ui__actions_attach(de_ui *ui) {
    de_ui_focus *f = ui->focus;
    de_input *in = ui->desc.input;
    if (!in)
        return;
    de_type_id resource_type = de_type_find(de_world_types(ui->world), "de_input_resource");
    de_resource_id resource =
        resource_type.id ? de_resource_find(ui->world, resource_type) : (de_resource_id){0};
    if (!resource.id) {
        if (de_input_attach(in, ui->world) != DE_OK)
            return;
        f->owns_input = true;
        resource_type = de_type_find(de_world_types(ui->world), "de_input_resource");
        resource = de_resource_find(ui->world, resource_type);
    }
    f->map = de_action_map_find(in, "ui");
    if (!f->map.id) {
        f->map = de_action_map_create(in, "ui");
        if (!f->map.id)
            return;
        defaults(in, f->map);
    }
    f->old_flags = de_action_map_flags(in, f->map);
    f->navigate = de_action_find(in, f->map, "navigate");
    f->submit = de_action_find(in, f->map, "submit");
    f->cancel = de_action_find(in, f->map, "cancel");
    f->next = de_action_find(in, f->map, "tab_next");
    f->previous = de_action_find(in, f->map, "tab_prev");
    f->scroll = de_action_find(in, f->map, "scroll");
    f->pointer = de_action_find(in, f->map, "pointer_motion");
    de_ui__actions_context(ui);
    const char *after[] = {"input.fixed_tick"};
    de_resource_access access = {resource, DE_READ};
    f->capture = de_system_add(ui->world, &(de_system_desc){.name = "ui.actions",
                                                            .schedule = DE_SCHEDULE_FIXED_UPDATE,
                                                            .fn = capture,
                                                            .user = f,
                                                            .flags = DE_SYSTEM_MAIN_THREAD,
                                                            .after = after,
                                                            .after_count = 1,
                                                            .resources = &access,
                                                            .resource_count = 1});
}
void de_ui__actions_detach(de_ui *ui) {
    de_ui_focus *f = ui->focus;
    de_input *in = ui->desc.input;
    if (!in)
        return;
    if (ui->world && f->capture.id)
        de_system_remove(ui->world, f->capture);
    f->capture = (de_system){0};
    if (f->map.id) {
        if (f->owns_push)
            de_action_map_pop(in, f->map);
        de_action_map_set_flags(in, f->map, f->old_flags);
    }
    if (f->owns_input)
        de_input_detach(in);
    f->owns_input = f->owns_push = false;
    f->map = (de_action_map){0};
}
