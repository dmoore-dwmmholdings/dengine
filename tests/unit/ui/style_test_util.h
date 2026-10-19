#ifndef DE_UI_STYLE_TEST_UTIL_H
#define DE_UI_STYLE_TEST_UTIL_H
#include "ui_test_util.h"
#include "ui/style/stylesheet_internal.h"
static inline de_result style_install(de_ui *ui, const char *source) {
    de_ui_style_system *state = ui->style;
    /* Isolate cascade tests from the separately verified embedded widget theme. */
    if (state->builtin_count) {
        u32 builtin = state->builtin_count;
        for (u32 i = 0; i < builtin; ++i) {
            de_ui_style_sheet_slot *slot = &state->sheets[i];
            if (slot->compiled)
                DE_FREE(ui->alloc, slot->compiled,
                        (u64)slot->sheet->rule_count * sizeof *slot->compiled);
            de_ui__sheet_release(slot->sheet);
        }
        memmove(state->sheets, state->sheets + builtin,
                (state->sheet_count - builtin) * sizeof state->sheets[0]);
        state->sheet_count -= builtin;
        state->builtin_count = 0;
    }

    if (state->sheet_count >= DE_UI_STYLE_SHEETS)
        return DE_ERR_OUT_OF_MEMORY;
    de_ui_style_sheet_slot slot = {0};
    DE_TRY(de_ui__sheet_parse(ui->alloc, source, strlen(source), &slot.sheet));
    if (slot.sheet->rule_count)
        slot.compiled = DE_ALLOC(ui->alloc, (u64)slot.sheet->rule_count * sizeof *slot.compiled,
                                 _Alignof(de_ui_compiled_rule), DE_MEM_UI);
    if (slot.sheet->rule_count && !slot.compiled) {
        de_ui__sheet_release(slot.sheet);
        return DE_ERR_OUT_OF_MEMORY;
    }
    de_result result = de_ui__compile_sheet(state, &slot);
    if (result != DE_OK) {
        if (slot.compiled)
            DE_FREE(ui->alloc, slot.compiled, (u64)slot.sheet->rule_count * sizeof *slot.compiled);
        de_ui__sheet_release(slot.sheet);
        return result;
    }
    state->sheets[state->sheet_count++] = slot;
    ++state->revision;
    return DE_OK;
}
static inline DeUiStyle style_get(ui_fx *f, de_entity e) {
    const DeUiStyle *style = de_entity_get_const(f->world, e, f->ui->ids.style);
    return style ? *style : (DeUiStyle){0};
}
static inline void style_step(ui_fx *f, f32 dt) {
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_POST_UPDATE, dt, 0), DE_OK);
}
static inline de_entity style_button(ui_fx *f, de_entity parent, const char *class_name) {
    de_entity e = ui_node(f, parent);
    DeUiWidgetType *widget = de_entity_add(f->world, e, f->ui->style->widget);
    widget->type = DE_UI_WIDGET_BUTTON;
    if (class_name)
        ui_node_edit(f, e)->style_class = de_name_from(class_name);
    return e;
}
#endif
