#ifndef DE_UI_NAV_TEST_UTIL_H
#define DE_UI_NAV_TEST_UTIL_H
#include "ui_test_util.h"
#include "ui/input/focus_internal.h"
#include "input_test_util.h"
typedef struct nav_fx {
    ui_fx base;
    de_input *input;
    de_entity root;
    de_event_reader events;
} nav_fx;
static inline void nav_fx_setup(nav_fx *f) {
    f->input = input_test_create(NULL);
    ASSERT_TRUE(f->input);
    ASSERT_EQ(de_type_registry_create(NULL, &f->base.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->base.types}, &f->base.world), DE_OK);
    ASSERT_EQ(
        de_ui_create(&(de_ui_desc){.input = f->input, .reference_height = 400, .max_nodes = 512},
                     &f->base.ui),
        DE_OK);
    de_ui_set_viewport(f->base.ui, 600, 400, (de_window_insets){0});
    de_ui_attach(f->base.ui, f->base.world);
    ASSERT_TRUE(f->base.ui->focus->capture.id);
    f->root = ui_node(&f->base, DE_ENTITY_NULL);
    f->events = de_event_reader_create(f->base.world, f->base.ui->event);
}
static inline void nav_fx_teardown(nav_fx *f) {
    ui_fx_teardown(&f->base);
    de_input_destroy(f->input);
}
static inline de_entity nav_node(nav_fx *f, de_entity parent, f32 x, f32 y, i32 tab,
                                 de_ui_widget_type type) {
    de_ui *ui = f->base.ui;
    de_world *w = f->base.world;
    de_entity e = ui_node(&f->base, parent);
    DeUiAnchor *a = de_entity_add(w, e, ui->ids.anchor);
    *a = (DeUiAnchor){.offset_min = {x, y}, .offset_max = {x + 40, y + 24}};
    DeUiFocusable *focus = de_entity_add(w, e, ui->focus->focusable);
    focus->tab_index = tab;
    (void)de_entity_add(w, e, ui->ids.state);
    de_component component = type == DE_UI_WIDGET_BUTTON   ? ui->widgets->button
                             : type == DE_UI_WIDGET_TOGGLE ? ui->widgets->toggle
                             : type == DE_UI_WIDGET_SLIDER ? ui->widgets->slider
                                                           : (de_component){0};
    if (component.id)
        (void)de_entity_add(w, e, component);
    else {
        DeUiWidgetType *tag = de_entity_add(w, e, de_component_find(w, "DeUiWidgetType"));
        tag->type = type;
    }
    return e;
}
static inline void nav_pre(nav_fx *f) {
    input_test_frame(f->input);
    ASSERT_EQ(de_world_run_schedule(f->base.world, DE_SCHEDULE_PRE_UPDATE, INPUT_TEST_DT, 0),
              DE_OK);
}
static inline void nav_fixed(nav_fx *f) {
    ASSERT_EQ(de_world_run_schedule(f->base.world, DE_SCHEDULE_FIXED_UPDATE, INPUT_TEST_DT, 0),
              DE_OK);
}
static inline void nav_post(nav_fx *f) {
    ui_frame(&f->base);
}
static inline void nav_frame(nav_fx *f) {
    nav_pre(f);
    nav_fixed(f);
    nav_post(f);
}
static inline void nav_key(nav_fx *f, de_key key) {
    input_test_key(f->input, key, true);
    nav_frame(f);
    input_test_key(f->input, key, false);
    nav_frame(f);
}
static inline u32 nav_events(nav_fx *f, de_ui_event_kind kind) {
    u32 count = 0;
    const de_ui_event *e;
    while ((e = de_event_next(f->base.world, &f->events)))
        if (e->kind == kind)
            ++count;
    return count;
}
#endif
