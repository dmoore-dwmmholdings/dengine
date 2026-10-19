#include "de_fuzz.h"
#include "ui/style/stylesheet_internal.h"
#include <stdlib.h>
#include <string.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536)
        return 0;
    de_ui_sheet_document *sheet = NULL;
    if (de_ui__sheet_parse(NULL, (const char *)data, size, &sheet) != DE_OK)
        return 0;
    de_type_registry *types = NULL;
    de_world *world = NULL;
    de_ui *ui = NULL;
    if (de_type_registry_create(NULL, &types) != DE_OK ||
        de_world_create(&(de_world_desc){.types = types}, &world) != DE_OK ||
        de_ui_create(&(de_ui_desc){.max_nodes = 8}, &ui) != DE_OK)
        abort();
    de_ui_attach(ui, world);
    de_ui_set_viewport(ui, 400, 200, (de_window_insets){0});
    de_ui_style_sheet_slot *slot = &ui->style->sheets[0];
    slot->sheet = sheet;
    if (sheet->rule_count) {
        slot->compiled = DE_ALLOC(ui->alloc, (u64)sheet->rule_count * sizeof *slot->compiled,
                                  _Alignof(de_ui_compiled_rule), DE_MEM_UI);
        if (!slot->compiled)
            abort();
    }
    if (de_ui__compile_sheet(ui->style, slot) != DE_OK)
        abort();
    ui->style->sheet_count = 1;
    ++ui->style->revision;
    de_entity entity;
    if (de_entity_spawn_with(world, &ui->ids.node, 1, &entity) != DE_OK)
        abort();
    DeUiNode *node = de_entity_get(world, entity, ui->ids.node);
    node->style_class = sheet->rule_count ? sheet->rules[0].class_name : (de_name){0};
    DeUiWidgetType *widget = de_entity_add(world, entity, ui->style->widget);
    widget->type = DE_UI_WIDGET_BUTTON;
    DeUiInteractionState *state = de_entity_add(world, entity, ui->ids.state);
    state->hovered = true;
    state->focused = true;
    state->pressed = true;
    state->disabled = true;
    if (de_world_run_schedule(world, DE_SCHEDULE_POST_UPDATE, .016f, 0) != DE_OK)
        abort();
    de_ui_tween(ui, entity, "opacity", .5f, .03f, DE_UI_EASE_LINEAR);
    if (de_world_run_schedule(world, DE_SCHEDULE_POST_UPDATE, .016f, 0) != DE_OK)
        abort();
    de_ui_destroy(ui);
    de_world_destroy(world);
    de_type_registry_destroy(types);
    return 0;
}
