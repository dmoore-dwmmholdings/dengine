#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "ui_test_util.h"
#include "ui/binding/binding_internal.h"
#include "ui/style/stylesheet_internal.h"
#include "de_alloc_guard.h"
#include <float.h>
#include <limits.h>
#include <stdio.h>

typedef struct binding_alloc_failure {
    u32 calls, fail_at, live;
} binding_alloc_failure;
static void *binding_failure_alloc(void *user, size_t size, size_t align, u32 tag) {
    binding_alloc_failure *s = user;
    if (s->calls++ == s->fail_at) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "injected binding allocation failure");
        return NULL;
    }
    void *p = DE_ALLOC(de_heap_allocator(), size, align, tag);
    if (p) {
        ++s->live;
        memset(p, 0xa5, size);
    }
    return p;
}
static void binding_failure_free(void *user, void *ptr, size_t size) {
    binding_alloc_failure *s = user;
    if (ptr) {
        EXPECT_GT(s->live, 0u);
        --s->live;
        DE_FREE(de_heap_allocator(), ptr, size);
    }
}
TEST(ui_binding, partial_initialization_allocation_failures_unwind) {
    for (u32 at = 0; at < 4; ++at) {
        binding_alloc_failure state = {.fail_at = at};
        de_allocator allocator = {
            .alloc = binding_failure_alloc, .free = binding_failure_free, .user = &state};
        de_ui ui = {.alloc = &allocator, .capacity = 8, .hash_capacity = 16};
        EXPECT_EQ(de_ui__binding_init(&ui), at < 3 ? DE_ERR_OUT_OF_MEMORY : DE_OK);
        de_ui__binding_shutdown(&ui);
        EXPECT_TRUE(ui.binding == NULL);
        EXPECT_EQ(state.live, 0u);
        /* Cleanup is idempotent after each failed or successful initialization. */
        de_ui__binding_shutdown(&ui);
    }
}

typedef struct bind_item {
    i32 count;
} bind_item;
typedef struct bind_inventory {
    bind_item slots[3];
} bind_inventory;
typedef struct bind_data {
    f32 current, moved, readonly_value;
    f64 precise;
    bool flag;
    i8 signed8;
    i16 signed16;
    i32 signed32;
    i64 signed64;
    u8 unsigned8;
    u16 unsigned16;
    u32 unsigned32;
    u64 unsigned64;
    de_name label;
    char title[64];
    de_vec4 tint;
    bind_inventory inventory;
} bind_data;
static const de_field_info data_fields[] = {
    DE_FIELD(bind_data, current, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(bind_data, moved, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(bind_data, readonly_value, DE_FK_F32, DE_FIELD_DEFAULT | DE_FIELD_READONLY),
    DE_FIELD(bind_data, precise, DE_FK_F64, DE_FIELD_DEFAULT),
    DE_FIELD(bind_data, flag, DE_FK_BOOL, DE_FIELD_DEFAULT),
    DE_FIELD(bind_data, signed8, DE_FK_I8, DE_FIELD_DEFAULT),
    DE_FIELD(bind_data, signed16, DE_FK_I16, DE_FIELD_DEFAULT),
    DE_FIELD(bind_data, signed32, DE_FK_I32, DE_FIELD_DEFAULT),
    DE_FIELD(bind_data, signed64, DE_FK_I64, DE_FIELD_DEFAULT),
    DE_FIELD(bind_data, unsigned8, DE_FK_U8, DE_FIELD_DEFAULT),
    DE_FIELD(bind_data, unsigned16, DE_FK_U16, DE_FIELD_DEFAULT),
    DE_FIELD(bind_data, unsigned32, DE_FK_U32, DE_FIELD_DEFAULT),
    DE_FIELD(bind_data, unsigned64, DE_FK_U64, DE_FIELD_DEFAULT),
    DE_FIELD(bind_data, label, DE_FK_NAME, DE_FIELD_DEFAULT),
    DE_FIELD(bind_data, title, DE_FK_STRING, DE_FIELD_DEFAULT),
    DE_FIELD(bind_data, tint, DE_FK_COLOR, DE_FIELD_DEFAULT),
    DE_FIELD_EX(bind_data, inventory, DE_FK_STRUCT, DE_FIELD_DEFAULT,
                {0, 0, NULL, NULL, "BindInventory", 0})};
typedef struct bind_fx {
    ui_fx ui;
    de_entity source;
    de_component data;
    de_type_id type;
    u32 changed;
} bind_fx;
static void bind_fx_setup(bind_fx *f) {
    ui_fx_setup(&f->ui);
    static const de_field_info item[] = {DE_FIELD(bind_item, count, DE_FK_I32, DE_FIELD_DEFAULT)};
    static const de_field_info inv[] = {DE_FIELD_EX(bind_inventory, slots, DE_FK_FIXED_ARRAY,
                                                    DE_FIELD_DEFAULT,
                                                    {0, 0, NULL, NULL, "BindItem", 3})};
    de_type_id t;
    ASSERT_EQ(de_type_register_ex(f->ui.types,
                                  &(de_type_desc){.name = "BindItem",
                                                  .size = sizeof(bind_item),
                                                  .align = _Alignof(bind_item),
                                                  .version = 1,
                                                  .is_pod = true,
                                                  .fields = item,
                                                  .field_count = 1},
                                  &t, NULL),
              DE_OK);
    ASSERT_EQ(de_type_register_ex(f->ui.types,
                                  &(de_type_desc){.name = "BindInventory",
                                                  .size = sizeof(bind_inventory),
                                                  .align = _Alignof(bind_inventory),
                                                  .version = 1,
                                                  .is_pod = true,
                                                  .fields = inv,
                                                  .field_count = 1},
                                  &t, NULL),
              DE_OK);
    ASSERT_EQ(de_type_register_ex(
                  f->ui.types,
                  &(de_type_desc){.name = "BindData",
                                  .size = sizeof(bind_data),
                                  .align = _Alignof(bind_data),
                                  .version = 1,
                                  .is_pod = true,
                                  .fields = data_fields,
                                  .field_count = sizeof data_fields / sizeof data_fields[0]},
                  &f->type, NULL),
              DE_OK);
    f->data = de_component_register(f->ui.world, f->type, DE_COMP_SNAPSHOT);
    ASSERT_TRUE(f->data.id);
    ASSERT_EQ(de_entity_spawn_with(f->ui.world, &f->data, 1, &f->source), DE_OK);
    bind_data *data = de_entity_get(f->ui.world, f->source, f->data);
    data->current = .42f;
    data->moved = .75f;
    data->readonly_value = .2f;
    data->precise = .625;
    data->flag = true;
    data->signed8 = -8;
    data->signed16 = -160;
    data->signed32 = -3200;
    data->signed64 = INT64_MIN;
    data->unsigned8 = 8;
    data->unsigned16 = 160;
    data->unsigned32 = 3200;
    data->unsigned64 = UINT64_MAX;
    memcpy(data->title, "Captain", 8);
    data->label = de_name_from("play");
    data->tint = (de_vec4){.1f, .2f, .3f, .4f};
    data->inventory.slots[2].count = 42;
    ASSERT_TRUE(f->ui.ui->binding->system.id);
}
static void bind_fx_teardown(bind_fx *f) {
    ui_fx_teardown(&f->ui);
}
static de_entity bound(bind_fx *f, const char *field, const char *property, de_ui_binding_mode mode,
                       de_component target) {
    de_entity e = ui_node(&f->ui, DE_ENTITY_NULL);
    if (target.id)
        (void)de_entity_add(f->ui.world, e, target);
    DeUiBinding *binding = de_entity_add(f->ui.world, e, f->ui.ui->binding->component);
    binding->source = f->source;
    snprintf(binding->path, sizeof binding->path, "BindData.%s", field);
    snprintf(binding->property, sizeof binding->property, "%s", property);
    binding->mode = mode;
    return e;
}
static const DeUiText *label(bind_fx *f, de_entity e) {
    return de_entity_get_const(f->ui.world, e, f->ui.ui->ids.text);
}
TEST_F(ui_binding, visual_bindings_override_default_state_and_theme_without_idle_writes, bind_fx) {
    de_ui *ui = f->ui.ui;
    ASSERT_GT(ui->style->builtin_count, 0u);
    const char *sheet = "[vars]\naccent='#ff0000'\n[theme.dark.vars]\naccent='#00ff00'\n"
                        "[style.text]\ntext_color='$accent'\n"
                        "[style.\"text:hover\"]\ntext_color='#ffff00'\n"
                        "[style.button]\nopacity=0.25\n"
                        "[style.\"button:hover\"]\nopacity=0.1";
    ASSERT_EQ(de_ui__load_stylesheet_source(ui, sheet, strlen(sheet), false), DE_OK);
    de_entity text = bound(f, "tint", "tint", DE_UI_BIND_ONE_WAY, ui->ids.text);
    de_entity button = bound(f, "precise", "opacity", DE_UI_BIND_ONE_WAY, ui->widgets->button);
    for (u32 step = 0; step < 4; ++step) {
        if (step == 1) {
            ((DeUiInteractionState *)de_entity_add(f->ui.world, text, ui->ids.state))->hovered =
                true;
            ((DeUiInteractionState *)de_entity_add(f->ui.world, button, ui->ids.state))->hovered =
                true;
        } else if (step == 2)
            ASSERT_EQ(de_ui_set_theme(ui, "dark"), DE_OK);
        else if (step == 3) {
            bind_data *data = de_entity_get(f->ui.world, f->source, f->data);
            data->precise = .375;
            data->tint = (de_vec4){.7f, .6f, .5f, 1};
        }
        ui_frame(&f->ui);
        const bind_data *data = de_entity_get_const(f->ui.world, f->source, f->data);
        const DeUiStyle *text_style = de_entity_get_const(f->ui.world, text, ui->ids.style);
        const DeUiStyle *button_style = de_entity_get_const(f->ui.world, button, ui->ids.style);
        ASSERT_TRUE(text_style && button_style);
        EXPECT_TRUE(memcmp(&text_style->text_color, &data->tint, sizeof data->tint) == 0);
        EXPECT_FLOAT_EQ(button_style->opacity, data->precise, .00001f);
    }
    /* Let unrelated default background/border transitions settle before the idle guard. */
    for (u32 i = 0; i < 12; ++i)
        ui_frame(&f->ui);
    u64 before = 0, after = 0;
    for (u32 tag = 0; tag < DE_MEM_TAG_BUILTIN_COUNT_; ++tag)
        before += de_mem_stats_for_tag(tag).alloc_count;
    for (u32 i = 0; i < 10; ++i) {
        ui_frame(&f->ui);
        EXPECT_EQ(ui->binding->writes, 0u);
        EXPECT_EQ(de_ui_get_layout_stats(ui).nodes_laid_out, 0u);
        EXPECT_EQ(de_ui_get_style_stats(ui).entities_restyled, 0u);
    }
    for (u32 tag = 0; tag < DE_MEM_TAG_BUILTIN_COUNT_; ++tag)
        after += de_mem_stats_for_tag(tag).alloc_count;
    EXPECT_EQ(after, before);
    de_entity_remove(f->ui.world, button, ui->binding->component);
    for (u32 i = 0; i < 12; ++i)
        ui_frame(&f->ui);
    const DeUiStyle *released = de_entity_get_const(f->ui.world, button, ui->ids.style);
    EXPECT_FLOAT_EQ(released->opacity, .1f, .00001f);
}
TEST_F(ui_binding, progress_updates_same_frame_and_unchanged_source_does_not_relayout, bind_fx) {
    de_entity e = bound(f, "current", "value", DE_UI_BIND_ONE_WAY, f->ui.ui->widgets->progress);
    for (u32 i = 0; i < 4; ++i)
        ui_frame(&f->ui);
    const DeUiProgress *progress = de_entity_get_const(f->ui.world, e, f->ui.ui->widgets->progress);
    EXPECT_FLOAT_EQ(progress->value, .42, .00001);
    ui_frame(&f->ui);
    EXPECT_EQ(f->ui.ui->binding->writes, 0u);
    EXPECT_EQ(de_ui_get_layout_stats(f->ui.ui).nodes_laid_out, 0u);
    ((bind_data *)de_entity_get(f->ui.world, f->source, f->data))->current = .8f;
    ui_frame(&f->ui);
    progress = de_entity_get_const(f->ui.world, e, f->ui.ui->widgets->progress);
    EXPECT_FLOAT_EQ(progress->value, .8, .00001);
    EXPECT_EQ(f->ui.ui->binding->writes, 1u);
}
TEST_F(ui_binding, attaching_opacity_binding_cancels_an_active_stylesheet_tween, bind_fx) {
    de_ui *ui = f->ui.ui;
    const char *sheet = "[style.button]\nopacity=1\ntransition=1\n"
                        "[style.\"button:hover\"]\nopacity=0";
    ASSERT_EQ(de_ui__load_stylesheet_source(ui, sheet, strlen(sheet), false), DE_OK);
    de_entity e = ui_node(&f->ui, DE_ENTITY_NULL);
    de_entity_add(f->ui.world, e, ui->widgets->button);
    ui_frame(&f->ui);
    ((DeUiInteractionState *)de_entity_get(f->ui.world, e, ui->ids.state))->hovered = true;
    ui_frame(&f->ui);
    EXPECT_GT(de_ui_get_style_stats(ui).active_tweens, 0u);
    DeUiBinding *binding = de_entity_add(f->ui.world, e, ui->binding->component);
    binding->source = f->source;
    memcpy(binding->path, "BindData.precise", sizeof("BindData.precise"));
    memcpy(binding->property, "opacity", sizeof("opacity"));
    for (u32 i = 0; i < 5; ++i) {
        ui_frame(&f->ui);
        const DeUiStyle *style = de_entity_get_const(f->ui.world, e, ui->ids.style);
        EXPECT_FLOAT_EQ(style->opacity, .625f, .00001f);
    }
}
TEST_F(ui_binding, nested_and_both_array_path_spellings_format_numeric_text, bind_fx) {
    de_entity a =
        bound(f, "inventory.slots.2.count", "text", DE_UI_BIND_ONE_WAY, f->ui.ui->ids.text);
    DeUiBinding *binding = de_entity_get(f->ui.world, a, f->ui.ui->binding->component);
    snprintf(binding->format, sizeof binding->format, "%s", "{value, number, integer} / 100");
    de_entity b =
        bound(f, "inventory.slots[2].count", "text", DE_UI_BIND_ONE_WAY, f->ui.ui->ids.text);
    ui_frame(&f->ui);
    EXPECT_TRUE(de_str_eq(label(f, a)->literal, DE_STR("42 / 100")));
    EXPECT_TRUE(de_str_eq(label(f, b)->literal, DE_STR("42")));
    EXPECT_EQ(f->ui.ui->binding->warnings, 0u);
}
TEST_F(ui_binding, every_numeric_kind_to_text_and_integer_extremes, bind_fx) {
    const char *fields[] = {"flag",      "signed8",    "signed16",   "signed32",   "signed64",
                            "unsigned8", "unsigned16", "unsigned32", "unsigned64", "precise"};
    const char *expected[] = {"true",
                              "-8",
                              "-160",
                              "-3200",
                              "-9223372036854775808",
                              "8",
                              "160",
                              "3200",
                              "18446744073709551615",
                              "0.625"};
    de_entity e[10];
    for (u32 i = 0; i < 10; ++i)
        e[i] = bound(f, fields[i], "text", DE_UI_BIND_ONE_WAY, f->ui.ui->ids.text);
    ui_frame(&f->ui);
    for (u32 i = 0; i < 10; ++i)
        EXPECT_TRUE(de_str_eq(label(f, e[i])->literal, de_str_from_cstr(expected[i])));
}
TEST_F(ui_binding, string_text_is_ui_owned_and_all_scalar_visual_properties, bind_fx) {
    de_ui *ui = f->ui.ui;
    de_entity text = bound(f, "title", "text", DE_UI_BIND_ONE_WAY, ui->ids.text);
    de_entity toggle = bound(f, "signed16", "on", DE_UI_BIND_ONE_WAY, ui->widgets->toggle);
    de_entity hidden = bound(f, "flag", "visible", DE_UI_BIND_ONE_WAY, (de_component){0});
    de_entity disabled = bound(f, "flag", "disabled", DE_UI_BIND_ONE_WAY, ui->widgets->button);
    de_entity opacity = bound(f, "precise", "opacity", DE_UI_BIND_ONE_WAY, (de_component){0});
    de_entity tint = bound(f, "tint", "tint", DE_UI_BIND_ONE_WAY, ui->ids.image);
    ui_frame(&f->ui);
    EXPECT_TRUE(de_str_eq(label(f, text)->literal, DE_STR("Captain")));
    const bind_data *data = de_entity_get_const(f->ui.world, f->source, f->data);
    EXPECT_TRUE(label(f, text)->literal.ptr != data->title);
    EXPECT_TRUE(
        ((const DeUiToggle *)de_entity_get_const(f->ui.world, toggle, ui->widgets->toggle))->on);
    EXPECT_EQ(ui_node_edit(&f->ui, hidden)->display, DE_UI_FLEX);
    EXPECT_TRUE(
        ((const DeUiButton *)de_entity_get_const(f->ui.world, disabled, ui->widgets->button))
            ->disabled);
    EXPECT_TRUE(
        ((const DeUiInteractionState *)de_entity_get_const(f->ui.world, disabled, ui->ids.state))
            ->disabled);
    EXPECT_FLOAT_EQ(
        ((const DeUiStyle *)de_entity_get_const(f->ui.world, opacity, ui->ids.style))->opacity,
        .625, .00001);
    const DeUiImage *image = de_entity_get_const(f->ui.world, tint, ui->ids.image);
    EXPECT_FLOAT_EQ(image->tint.z, .3, .00001);
    ((bind_data *)de_entity_get(f->ui.world, f->source, f->data))->flag = false;
    ui_frame(&f->ui);
    EXPECT_EQ(ui_node_edit(&f->ui, hidden)->display, DE_UI_NONE);
    EXPECT_FALSE(
        ((const DeUiButton *)de_entity_get_const(f->ui.world, disabled, ui->widgets->button))
            ->disabled);
}
static void count_changed(const de_query_chunk *chunk, void *user) {
    ((bind_fx *)user)->changed += chunk->count;
}
static void changed(de_system_ctx *ctx) {
    bind_fx *f = ctx->user;
    f->changed = 0;
    u32 count = de_query_count(ctx->world, ctx->queries[0]);
    de_query_each(ctx->world, ctx->queries[0], count_changed, f);
    EXPECT_EQ(count, f->changed);
}
TEST_F(ui_binding, actual_slider_toggle_changed_writeback_and_readonly_marks_next_update, bind_fx) {
    de_ui *ui = f->ui.ui;
    de_entity slider = bound(f, "current", "value", DE_UI_BIND_TWO_WAY, ui->widgets->slider);
    de_entity readonly =
        bound(f, "readonly_value", "value", DE_UI_BIND_TWO_WAY, ui->widgets->slider);
    de_entity toggle = bound(f, "flag", "on", DE_UI_BIND_TWO_WAY, ui->widgets->toggle);
    de_query_term term = {f->data, DE_READ};
    de_query_filter filter = {f->data, DE_CHANGED};
    de_query_desc query = {.terms = &term, .term_count = 1, .filters = &filter, .filter_count = 1};
    ASSERT_TRUE(de_system_add(f->ui.world, &(de_system_desc){.name = "binding.changed.observe",
                                                             .schedule = DE_SCHEDULE_UPDATE,
                                                             .fn = changed,
                                                             .user = f,
                                                             .queries = &query,
                                                             .query_count = 1})
                    .id);
    ui_frame(&f->ui);
    ASSERT_EQ(de_world_run_schedule(f->ui.world, DE_SCHEDULE_UPDATE, .016f, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->ui.world, DE_SCHEDULE_UPDATE, .016f, 0), DE_OK);
    EXPECT_EQ(f->changed, 0u);
    ui->widgets->table.entries[DE_UI_WIDGET_SLIDER].increment(ui, slider);
    ui->widgets->table.entries[DE_UI_WIDGET_SLIDER].increment(ui, readonly);
    ui->widgets->table.entries[DE_UI_WIDGET_TOGGLE].submit(ui, toggle);
    ui_frame(&f->ui);
    const bind_data *data = de_entity_get_const(f->ui.world, f->source, f->data);
    EXPECT_FLOAT_EQ(data->current, .5, .00001);
    EXPECT_FLOAT_EQ(data->readonly_value, .2, .00001);
    EXPECT_FALSE(data->flag);
    ASSERT_EQ(de_world_run_schedule(f->ui.world, DE_SCHEDULE_UPDATE, .016f, 0), DE_OK);
    EXPECT_EQ(f->changed, 1u);
    ASSERT_EQ(de_world_run_schedule(f->ui.world, DE_SCHEDULE_UPDATE, .016f, 0), DE_OK);
    EXPECT_EQ(f->changed, 0u);
}
TEST_F(ui_binding, registry_revision_moves_offset_and_retries_invalid_cache_once, bind_fx) {
    de_entity good = bound(f, "current", "value", DE_UI_BIND_ONE_WAY, f->ui.ui->widgets->progress);
    de_entity bad = bound(f, "future", "text", DE_UI_BIND_ONE_WAY, f->ui.ui->ids.text);
    ui_frame(&f->ui);
    EXPECT_EQ(f->ui.ui->binding->warnings, 1u);
    for (u32 i = 0; i < 10; ++i)
        ui_frame(&f->ui);
    EXPECT_EQ(f->ui.ui->binding->warnings, 1u);
    de_field_info fields[sizeof data_fields / sizeof data_fields[0]];
    memcpy(fields, data_fields, sizeof fields);
    fields[0].offset = offsetof(bind_data, moved);
    fields[1].name = "future";
    fields[1].offset = offsetof(bind_data, current);
    de_type_id type;
    ASSERT_EQ(de_type_register_ex(f->ui.types,
                                  &(de_type_desc){.name = "BindData",
                                                  .size = sizeof(bind_data),
                                                  .align = _Alignof(bind_data),
                                                  .version = 2,
                                                  .is_pod = true,
                                                  .fields = fields,
                                                  .field_count = sizeof fields / sizeof fields[0]},
                                  &type, NULL),
              DE_OK);
    ui_frame(&f->ui);
    EXPECT_FLOAT_EQ(
        ((const DeUiProgress *)de_entity_get_const(f->ui.world, good, f->ui.ui->widgets->progress))
            ->value,
        .75, .00001);
    EXPECT_TRUE(de_str_eq(label(f, bad)->literal, DE_STR("0.419999986886978")));
    EXPECT_EQ(f->ui.ui->binding->warnings, 1u);
    /* Metadata-only publication invalidates resolved and errored paths too. */
    fields[0].flags |= DE_FIELD_READONLY;
    ASSERT_EQ(de_type_register_ex(f->ui.types,
                                  &(de_type_desc){.name = "BindData",
                                                  .size = sizeof(bind_data),
                                                  .align = _Alignof(bind_data),
                                                  .version = 2,
                                                  .is_pod = true,
                                                  .fields = fields,
                                                  .field_count = sizeof fields / sizeof fields[0]},
                                  &type, NULL),
              DE_OK);
    u32 resolved = f->ui.ui->binding->resolved;
    ui_frame(&f->ui);
    EXPECT_EQ(f->ui.ui->binding->resolved, resolved + 2);
}
TEST_F(ui_binding, invalid_kind_bad_format_despawn_idle_and_descriptor_repair, bind_fx) {
    de_ui *ui = f->ui.ui;
    de_entity bad = bound(f, "title", "value", DE_UI_BIND_ONE_WAY, ui->widgets->progress);
    de_entity fmt = bound(f, "current", "text", DE_UI_BIND_ONE_WAY, ui->ids.text);
    DeUiBinding *binding = de_entity_get(f->ui.world, fmt, ui->binding->component);
    memcpy(binding->format, "{value, unknown}", 17);
    de_entity good = bound(f, "title", "text", DE_UI_BIND_ONE_WAY, ui->ids.text);
    ui_frame(&f->ui);
    EXPECT_EQ(ui->binding->warnings, 2u);
    for (u32 i = 0; i < 10; ++i)
        ui_frame(&f->ui);
    EXPECT_EQ(ui->binding->warnings, 2u);
    binding = de_entity_get(f->ui.world, bad, ui->binding->component);
    memcpy(binding->path, "BindData.current", 17);
    ui_frame(&f->ui);
    EXPECT_FLOAT_EQ(
        ((const DeUiProgress *)de_entity_get_const(f->ui.world, bad, ui->widgets->progress))->value,
        .42, .00001);
    const char *owned = label(f, good)->literal.ptr;
    de_entity_despawn(f->ui.world, f->source);
    ui_frame(&f->ui);
    EXPECT_STREQ(owned, "Captain");
    EXPECT_EQ(ui->binding->warnings, 2u);
}
TEST_F(ui_binding, thousand_idle_bindings_under_point_two_ms_no_heap_and_no_layout, bind_fx) {
    de_ui *ui = f->ui.ui;
    for (u32 i = 0; i < 1000; ++i)
        (void)bound(f, "current", "value", DE_UI_BIND_ONE_WAY, ui->widgets->progress);
    for (u32 i = 0; i < 8; ++i)
        ui_frame(&f->ui);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    f64 total = 0;
    for (u32 i = 0; i < 100; ++i) {
        ui_frame(&f->ui);
        total += ui->binding->update_ms;
    }
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    printf("1000 idle bindings %.4fms mean\n", total / 100);
    EXPECT_TRUE(total / 100 <= .2);
    EXPECT_EQ(ui->binding->writes, 0u);
    EXPECT_EQ(de_ui_get_layout_stats(ui).nodes_laid_out, 0u);
}
#if !defined(_WIN32)
#include <stdlib.h>
#include <unistd.h>
static void binding_locale_wait(de_asset_system *assets, de_text_system *text, u64 generation) {
    u64 deadline = de_time_now_ns() + 10000000000ULL;
    do {
        de_asset_system_update(assets, NULL);
        de_text_update(text);
        if (de_loc_generation(text) >= generation)
            return;
        de_sleep_ns(1000000);
    } while (de_time_now_ns() < deadline);
    ASSERT_TRUE(de_loc_generation(text) >= generation);
}
TEST_F(ui_binding, real_localized_name_refreshes_on_ready_locale_with_unchanged_source_bytes,
       bind_fx) {
    char directory[] = "/tmp/deng-binding-locale-XXXXXX";
    ASSERT_TRUE(mkdtemp(directory));
    ASSERT_EQ(de_fs_mount("bindlocale", directory, DE_MOUNT_WRITABLE, 0), DE_OK);
    ASSERT_EQ(de_dir_create("bindlocale:/strings/main"), DE_OK);
    const char *manifest = "[assets]\n'strings/main'={type='string_table',source='strings/main'}\n";
    const char *en = "[strings]\nplay='Play'\n", *pt = "[strings]\nplay='Jogar'\n";
    ASSERT_EQ(de_file_write_all_atomic("bindlocale:/assets.toml", manifest, strlen(manifest)),
              DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("bindlocale:/strings/main/en.toml", en, strlen(en)), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("bindlocale:/strings/main/pt.toml", pt, strlen(pt)), DE_OK);
    de_asset_system *assets = NULL;
    de_text_system *text = NULL;
    de_asset_system_desc asset_desc = {0};
#if !DE_HAS_COOKER && defined(DE_UI_TEST_PACKED_ASSETS)
    asset_desc.mode = DE_ASSET_MODE_PACKED;
#endif
    ASSERT_EQ(de_asset_system_create(&asset_desc, &assets), DE_OK);
    ASSERT_EQ(de_text_create(NULL, assets, &text), DE_OK);
    u64 generation = de_loc_generation(text);
    ASSERT_EQ(de_asset_manifest_load(assets, "bindlocale:/assets.toml"), DE_OK);
#if !DE_HAS_COOKER && defined(DE_UI_TEST_PACKED_ASSETS)
    char pack[256] = {0};
    text_fixture_pack(assets, "bindlocale:/", pack);
#endif
    binding_locale_wait(assets, text, generation + 1);
    f->ui.ui->desc.text = text;
    de_entity e = bound(f, "label", "text", DE_UI_BIND_ONE_WAY, f->ui.ui->ids.text);
    ui_frame(&f->ui);
    EXPECT_TRUE(de_str_eq(label(f, e)->literal, DE_STR("Play")));
    generation = de_loc_generation(text);
    ASSERT_EQ(de_loc_set_locale(text, "pt"), DE_OK);
    binding_locale_wait(assets, text, generation + 1);
    ui_frame(&f->ui);
    EXPECT_TRUE(de_str_eq(label(f, e)->literal, DE_STR("Jogar")));
    EXPECT_EQ(f->ui.ui->binding->warnings, 0u);
    f->ui.ui->desc.text = NULL;
    de_text_destroy(text);
    de_asset_system_destroy(assets);
#if !DE_HAS_COOKER && defined(DE_UI_TEST_PACKED_ASSETS)
    text_fixture_pack_cleanup(pack);
#endif
    ASSERT_EQ(de_file_delete("bindlocale:/strings/main/en.toml"), DE_OK);
    ASSERT_EQ(de_file_delete("bindlocale:/strings/main/pt.toml"), DE_OK);
    ASSERT_EQ(de_file_delete("bindlocale:/strings/main"), DE_OK);
    ASSERT_EQ(de_file_delete("bindlocale:/strings"), DE_OK);
    ASSERT_EQ(de_file_delete("bindlocale:/assets.toml"), DE_OK);
    ASSERT_EQ(de_fs_unmount("bindlocale", directory), DE_OK);
    EXPECT_EQ(rmdir(directory), 0);
}
#endif
