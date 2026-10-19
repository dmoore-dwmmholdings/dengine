#include "ui_test_util.h"
#include "ui/widgets/advanced_internal.h"
#include "../input/input_test_util.h"
typedef struct edit_fx {
    ui_fx base;
    de_input *input;
    de_entity editor;
} edit_fx;
static void edit_fx_setup(edit_fx *f) {
    ui_fx_setup(&f->base);
    f->input = input_test_create(NULL);
    ASSERT_TRUE(f->input);
    de_ui_attach(f->base.ui, NULL);
    f->base.ui->desc.input = f->input;
    de_ui_attach(f->base.ui, f->base.world);
    de_entity root = ui_node(&f->base, (de_entity){0});
    f->editor = ui_node(&f->base, root);
    DeUiNode *n = ui_node_edit(&f->base, f->editor);
    n->width = (de_ui_len){DE_UI_PX, 200};
    n->height = (de_ui_len){DE_UI_PX, 40};
    de_entity_add(f->base.world, f->editor, f->base.ui->advanced->input);
    ui_frame(&f->base);
    de_ui__input_submit(f->base.ui, f->editor);
}
static void edit_fx_teardown(edit_fx *f) {
    ui_fx_teardown(&f->base);
    de_input_destroy(f->input);
}
static void edit_frame(edit_fx *f) {
    if (f->base.ui->desc.text)
        de_text_update(f->base.ui->desc.text);
    input_test_frame(f->input);
    ASSERT_EQ(de_world_run_schedule(f->base.world, DE_SCHEDULE_PRE_UPDATE, INPUT_TEST_DT, 0),
              DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->base.world, DE_SCHEDULE_FIXED_UPDATE, INPUT_TEST_DT, 0),
              DE_OK);
    ui_frame(&f->base);
}
static DeUiTextInput *editing(edit_fx *f) {
    return de_entity_get(f->base.world, f->editor, f->base.ui->advanced->input);
}
TEST_F(ui_text_input, injected_japanese_preedit_then_commit_no_buffer_mutation, edit_fx) {
    input_test_text_edit(f->input, "にほん", 2, 1);
    edit_frame(f);
    DeUiTextInput *in = editing(f);
    EXPECT_EQ(in->length, 0u);
    EXPECT_EQ(in->buffer[0], 0);
    const de_ui_editor *state =
        de_entity_get_const(f->base.world, f->editor, f->base.ui->advanced->editor);
    EXPECT_TRUE(strstr(state->display, "にほん") != NULL);
    EXPECT_TRUE(state->preedit_end > state->preedit_start);
    input_test_text_edit(f->input, "", 0, 0);
    input_test_text(f->input, "日本");
    edit_frame(f);
    in = editing(f);
    EXPECT_EQ(in->length, 6u);
    EXPECT_TRUE(strcmp(in->buffer, "日本") == 0);
    state = de_entity_get_const(f->base.world, f->editor, f->base.ui->advanced->editor);
    EXPECT_EQ(state->preedit_start, state->preedit_end);
}
TEST(ui_text_input, grapheme_selection_word_navigation_and_255_byte_cap) {
    DeUiTextInput in = {0};
    de_ui_editor state = {0};
    const char *utf = "a\xcc\x81👨‍👩‍👧‍👦b";
    ASSERT_TRUE(de_ui__input_replace(&in, &state, (de_str){utf, strlen(utf)}));
    EXPECT_EQ(in.length, 29u);
    de_ui__input_move(&in, &state, -1, false, false);
    EXPECT_EQ(in.caret, 28u);
    de_ui__input_move(&in, &state, -1, true, false);
    EXPECT_EQ(in.caret, 3u);
    EXPECT_EQ(state.anchor, 28u);
    ASSERT_TRUE(de_ui__input_replace(&in, &state, (de_str){"", 0}));
    EXPECT_TRUE(strcmp(in.buffer, "a\xcc\x81"
                                  "b") == 0);
    memset(&in, 0, sizeof in);
    memset(&state, 0, sizeof state);
    char ascii[254];
    memset(ascii, 'x', 253);
    ascii[253] = 0;
    ASSERT_TRUE(de_ui__input_replace(&in, &state, (de_str){ascii, 253}));
    EXPECT_FALSE(de_ui__input_replace(&in, &state, (de_str){"日", 3}));
    EXPECT_EQ(in.length, 253u);
    ASSERT_TRUE(de_ui__input_replace(&in, &state, (de_str){"ab", 2}));
    EXPECT_EQ(in.length, 255u);
    EXPECT_EQ(in.buffer[255], 0);
    memset(&in, 0, sizeof in);
    memset(&state, 0, sizeof state);
    de_ui__input_replace(&in, &state, (de_str){"one two three", 13});
    de_ui__input_move(&in, &state, -1, false, true);
    EXPECT_EQ(in.caret, 8u);
    de_ui__input_move(&in, &state, -1, true, true);
    EXPECT_EQ(in.caret, 4u);
    EXPECT_EQ(state.anchor, 8u);
}
static char clip[256];
static de_result clip_set(const char *s) {
    strncpy(clip, s, 255);
    clip[255] = 0;
    return DE_OK;
}
static size_t clip_get(char *out, size_t cap) {
    size_t n = strlen(clip);
    if (cap) {
        size_t c = n < cap - 1 ? n : cap - 1;
        memcpy(out, clip, c);
        out[c] = 0;
    }
    return n;
}
TEST_F(ui_text_input, injected_select_copy_cut_paste_password_and_multiline_submit, edit_fx) {
    f->base.ui->advanced->clipboard_set = clip_set;
    f->base.ui->advanced->clipboard_get = clip_get;
    input_test_text(f->input, "hello");
    edit_frame(f);
    input_test_key(f->input, DE_KEY_LCTRL, true);
    input_test_key(f->input, DE_KEY_A, true);
    edit_frame(f);
    input_test_key(f->input, DE_KEY_A, false);
    input_test_key(f->input, DE_KEY_C, true);
    edit_frame(f);
    EXPECT_TRUE(strcmp(clip, "hello") == 0);
    input_test_key(f->input, DE_KEY_C, false);
    input_test_key(f->input, DE_KEY_X, true);
    edit_frame(f);
    EXPECT_EQ(editing(f)->length, 0u);
    input_test_key(f->input, DE_KEY_X, false);
    input_test_key(f->input, DE_KEY_V, true);
    edit_frame(f);
    EXPECT_TRUE(strcmp(editing(f)->buffer, "hello") == 0);
    input_test_key(f->input, DE_KEY_V, false);
    input_test_key(f->input, DE_KEY_LCTRL, false);
    editing(f)->password = true;
    edit_frame(f);
    const de_ui_editor *state =
        de_entity_get_const(f->base.world, f->editor, f->base.ui->advanced->editor);
    EXPECT_TRUE(strstr(state->display, "hello") == NULL);
    EXPECT_EQ(strlen(state->display), 15u);
    de_event_reader reader = de_event_reader_create(f->base.world, f->base.ui->event);
    input_test_key(f->input, DE_KEY_RETURN, true);
    edit_frame(f);
    const de_ui_event *ev;
    u32 submits = 0;
    while ((ev = de_event_next(f->base.world, &reader)))
        if (ev->kind == DE_UI_EV_SUBMIT)
            ++submits;
    EXPECT_EQ(submits, 1u);
    input_test_key(f->input, DE_KEY_RETURN, false);
    edit_frame(f);
    editing(f)->multiline = true;
    input_test_key(f->input, DE_KEY_RETURN, true);
    edit_frame(f);
    EXPECT_TRUE(strchr(editing(f)->buffer, '\n') != NULL);
}

TEST_F(ui_text_input, keyboard_editor_context_blocks_navigation_submit_and_cancel_once, edit_fx) {
    ui_fx *b = &f->base;
    de_entity button = ui_node(b, (de_entity){0});
    de_entity_add(b->world, button, b->ui->widgets->button);
    de_entity_add(b->world, button, b->ui->widgets->focusable);
    ui_frame(b);
    input_test_text(f->input, "abcd");
    edit_frame(f);
    EXPECT_EQ(de_ui_focused(b->ui).bits, f->editor.bits);
    ASSERT_TRUE(b->ui->advanced->edit_map.id);
    input_test_key(f->input, DE_KEY_LEFT, true);
    edit_frame(f);
    de_action_map ui_map = de_action_map_find(f->input, "ui");
    ASSERT_TRUE(ui_map.id);
    EXPECT_FLOAT_EQ(de_action_value2(f->input, de_action_find(f->input, ui_map, "navigate")).x, 0,
                    0);
    EXPECT_EQ(editing(f)->caret, 3u);
    EXPECT_EQ(de_ui_focused(b->ui).bits, f->editor.bits);
    input_test_key(f->input, DE_KEY_LEFT, false);
    input_test_key(f->input, DE_KEY_RIGHT, true);
    edit_frame(f);
    EXPECT_EQ(editing(f)->caret, 4u);
    EXPECT_EQ(de_ui_focused(b->ui).bits, f->editor.bits);
    input_test_key(f->input, DE_KEY_RIGHT, false);
    input_test_key(f->input, DE_KEY_SPACE, true);
    input_test_text(f->input, " ");
    de_event_reader reader = de_event_reader_create(b->world, b->ui->event);
    edit_frame(f);
    EXPECT_EQ(editing(f)->length, 5u);
    const de_ui_event *event;
    u32 submits = 0;
    while ((event = de_event_next(b->world, &reader)))
        if (event->kind == DE_UI_EV_SUBMIT || event->kind == DE_UI_EV_CLICK)
            ++submits;
    EXPECT_EQ(submits, 0u);
    input_test_key(f->input, DE_KEY_SPACE, false);
    input_test_key(f->input, DE_KEY_RETURN, true);
    edit_frame(f);
    submits = 0;
    while ((event = de_event_next(b->world, &reader)))
        if (event->kind == DE_UI_EV_SUBMIT)
            ++submits;
    EXPECT_EQ(submits, 1u);
    input_test_key(f->input, DE_KEY_RETURN, false);
    edit_frame(f);
    editing(f)->multiline = true;
    input_test_key(f->input, DE_KEY_RETURN, true);
    edit_frame(f);
    submits = 0;
    while ((event = de_event_next(b->world, &reader)))
        if (event->kind == DE_UI_EV_SUBMIT)
            ++submits;
    EXPECT_EQ(submits, 0u);
    EXPECT_TRUE(strchr(editing(f)->buffer, '\n') != NULL);
    input_test_key(f->input, DE_KEY_RETURN, false);
    input_test_key(f->input, DE_KEY_ESCAPE, true);
    edit_frame(f);
    EXPECT_EQ(b->ui->advanced->active_input.bits, 0u);
    EXPECT_EQ(de_ui_focused(b->ui).bits, 0u);
    edit_frame(f);
    EXPECT_EQ(b->ui->advanced->active_input.bits, 0u);
    input_test_key(f->input, DE_KEY_ESCAPE, false);
    edit_frame(f);
    EXPECT_EQ(b->ui->advanced->active_input.bits, 0u);
}
#if defined(DE_UI_TEST_FONTS)
#include "../text/runtime_test_util.h"
TEST_F(ui_text_input, public_rtl_hit_test_mouse_drag_double_word_and_horizontal_scroll, edit_fx) {
    runtime_fx fonts = {0};
    runtime_fx_setup(&fonts);
    ui_fx *b = &f->base;
    b->ui->desc.text = fonts.t;
    de_font hebrew = de_asset_font(fonts.assets, de_asset_id_from("font/hebrew"));
    de_load_handle load = de_asset_load(fonts.assets, de_asset_id_from("font/hebrew"), 0);
    ASSERT_EQ(de_asset_wait(fonts.assets, load, 60ULL * 1000000000), (int)DE_LOAD_READY);
    de_text_update(fonts.t);
    DeUiText *text = de_entity_get(b->world, f->editor, b->ui->ids.text);
    text->style =
        (de_text_style){.font = hebrew, .size_px = 20, .line_height = 1, .color = {1, 1, 1, 1}};
    DeUiTextInput *in = editing(f);
    memcpy(in->buffer, "שלום", 9);
    in->length = in->caret = 8;
    de_ui_editor *state = de_entity_get(b->world, f->editor, b->ui->advanced->editor);
    state->anchor = 8;
    ui_frame(b);
    de_text_layout *layout = NULL;
    de_text_style style = text->style;
    ASSERT_EQ(de_text_layout_build(fonts.t,
                                   &(de_text_layout_desc){.utf8 = {"שלום", 8}, .style = &style},
                                   NULL, &layout),
              DE_OK);
    u32 expected = de_text_layout_hit_test(layout, (de_vec2){0, 10});
    de_text_layout_free(layout);
    de__input_event ev = input_test_event(DE_RAW_MOUSE_BUTTON);
    ev.mouse_button.button = DE_MOUSE_LEFT;
    ev.mouse_button.down = true;
    ev.mouse_button.x = 4;
    ev.mouse_button.y = 14;
    de__input_inject(f->input, &ev);
    edit_frame(f);
    EXPECT_EQ(editing(f)->caret, expected);
    ev.mouse_button.down = false;
    de__input_inject(f->input, &ev);
    edit_frame(f);
    text = de_entity_get(b->world, f->editor, b->ui->ids.text);
    text->style.font = fonts.latin;
    in = editing(f);
    memcpy(in->buffer, "one two", 8);
    in->length = in->caret = 7;
    state = de_entity_get(b->world, f->editor, b->ui->advanced->editor);
    state->anchor = 7;
    state->last_click = -10;
    ui_frame(b);
    ev.mouse_button.down = true;
    ev.mouse_button.x = 7;
    ev.mouse_button.y = 14;
    de__input_inject(f->input, &ev);
    edit_frame(f);
    input_test_mouse_move(f->input, 80, 14, 73, 0);
    edit_frame(f);
    in = editing(f);
    state = de_entity_get(b->world, f->editor, b->ui->advanced->editor);
    EXPECT_TRUE(in->caret != state->anchor);
    ev.mouse_button.down = false;
    ev.mouse_button.x = 7;
    de__input_inject(f->input, &ev);
    edit_frame(f);
    ev.mouse_button.down = true;
    de__input_inject(f->input, &ev);
    edit_frame(f);
    in = editing(f);
    state = de_entity_get(b->world, f->editor, b->ui->advanced->editor);
    EXPECT_EQ(state->anchor, 0u);
    EXPECT_EQ(in->caret, 3u);
    ev.mouse_button.down = false;
    de__input_inject(f->input, &ev);
    edit_frame(f);
    in = editing(f);
    memset(in->buffer, 'W', 100);
    in->buffer[100] = 0;
    in->length = in->caret = 100;
    state = de_entity_get(b->world, f->editor, b->ui->advanced->editor);
    state->anchor = 100;
    edit_frame(f);
    state = de_entity_get(b->world, f->editor, b->ui->advanced->editor);
    EXPECT_TRUE(state->scroll_x > 0);
    for (u32 i = 0; i < 16; ++i)
        edit_frame(f);
    u64 before = 0, after = 0;
    for (u32 i = 0; i < DE_MEM_TAG_BUILTIN_COUNT_; ++i)
        before += de_mem_stats_for_tag(i).alloc_count;
    for (u32 i = 0; i < 100; ++i)
        edit_frame(f);
    for (u32 i = 0; i < DE_MEM_TAG_BUILTIN_COUNT_; ++i)
        after += de_mem_stats_for_tag(i).alloc_count;
    EXPECT_EQ(after, before);
    in = editing(f);
    strcpy(in->buffer, "secret123");
    in->length = in->caret = 9;
    in->password = true;
    for (u32 i = 0; i < 32; ++i)
        edit_frame(f);
    state = de_entity_get(b->world, f->editor, b->ui->advanced->editor);
    de_text_style password_style =
        ((const DeUiText *)de_entity_get_const(b->world, f->editor, b->ui->ids.text))->style;
    de_text_layout *password_layout = NULL;
    ASSERT_EQ(de_text_layout_build(
                  fonts.t,
                  &(de_text_layout_desc){.utf8 = {state->display, strlen(state->display)},
                                         .style = &password_style},
                  NULL, &password_layout),
              DE_OK);
    de_glyph_quad password_quads[16];
    EXPECT_EQ(de_text_layout_quads(password_layout, password_quads, 16), 9u);
    de_text_layout_free(password_layout);
    b->ui->desc.text = NULL;
    de_ui_attach(b->ui, NULL);
    de_asset_release(fonts.assets, load);
    de_text_destroy(fonts.t);
    fonts.t = NULL;
    runtime_fx_teardown(&fonts);
}
#endif
TEST_F(ui_text_input, escape_editor_does_not_close_parent_modal, edit_fx) {
    ui_fx *b = &f->base;
    de_entity root = de_entity_parent(b->world, f->editor), modal = ui_node(b, root);
    DeUiNode *node = ui_node_edit(b, modal);
    node->width = (de_ui_len){DE_UI_PX, 300};
    node->height = (de_ui_len){DE_UI_PX, 80};
    DeUiPopup *popup = de_entity_add(b->world, modal, b->ui->ids.popup);
    *popup = (DeUiPopup){.anchor = root, .placement = DE_UI_POPUP_OVERLAY, .modal = true};
    ASSERT_EQ(de_entity_set_parent(b->world, f->editor, modal), DE_OK);
    ui_frame(b);
    de_ui__input_submit(b->ui, f->editor);
    edit_frame(f);
    input_test_key(f->input, DE_KEY_ESCAPE, true);
    edit_frame(f);
    EXPECT_EQ(b->ui->advanced->active_input.bits, 0u);
    EXPECT_TRUE(ui_node_edit(b, modal)->display != DE_UI_NONE);
    input_test_key(f->input, DE_KEY_ESCAPE, false);
    edit_frame(f);
    EXPECT_EQ(b->ui->advanced->active_input.bits, 0u);
    EXPECT_TRUE(ui_node_edit(b, modal)->display != DE_UI_NONE);
}
typedef struct editor_source {
    char text[256], locked[256];
} editor_source;
static void editor_count_chunk(const de_query_chunk *chunk, void *user) {
    *(u32 *)user += chunk->count;
}
static void editor_observe(de_system_ctx *ctx) {
    u32 *count = ctx->user;
    *count = 0;
    de_query_each(ctx->world, ctx->queries[0], editor_count_chunk, count);
}
TEST_F(ui_text_input, actual_committed_binding_refresh_writeback_preedit_and_readonly, edit_fx) {
    ui_fx *b = &f->base;
    static const de_field_info fields[] = {
        DE_FIELD(editor_source, text, DE_FK_STRING, DE_FIELD_DEFAULT),
        DE_FIELD(editor_source, locked, DE_FK_STRING, DE_FIELD_DEFAULT | DE_FIELD_READONLY)};
    de_type_id type;
    ASSERT_EQ(de_type_register_ex(b->types,
                                  &(de_type_desc){.name = "EditorSource",
                                                  .size = sizeof(editor_source),
                                                  .align = _Alignof(editor_source),
                                                  .version = 1,
                                                  .is_pod = true,
                                                  .fields = fields,
                                                  .field_count = 2},
                                  &type, NULL),
              DE_OK);
    de_component component = de_component_register(b->world, type, DE_COMP_SNAPSHOT);
    de_entity source;
    ASSERT_EQ(de_entity_spawn_with(b->world, &component, 1, &source), DE_OK);
    editor_source *data = de_entity_get(b->world, source, component);
    strcpy(data->text, "Captain");
    strcpy(data->locked, "Locked");
    de_component binding_id = de_component_find(b->world, "DeUiBinding");
    ASSERT_TRUE(binding_id.id);
    DeUiBinding *binding = de_entity_add(b->world, f->editor, binding_id);
    binding->source = source;
    strcpy(binding->path, "EditorSource.text");
    strcpy(binding->property, "text");
    binding->mode = DE_UI_BIND_TWO_WAY;
    ui_frame(b);
    EXPECT_TRUE(strcmp(editing(f)->buffer, "Captain") == 0);
    const DeUiText *display = de_entity_get_const(b->world, f->editor, b->ui->ids.text);
    EXPECT_EQ(display->literal.len, 7u);
    EXPECT_TRUE(memcmp(display->literal.ptr, "Captain", 7) == 0);
    data = de_entity_get(b->world, source, component);
    strcpy(data->text, "Pilot");
    ui_frame(b);
    EXPECT_TRUE(strcmp(editing(f)->buffer, "Pilot") == 0);
    display = de_entity_get_const(b->world, f->editor, b->ui->ids.text);
    EXPECT_EQ(display->literal.len, 5u);
    u32 changes = 0;
    de_query_term term = {component, DE_READ};
    de_query_filter filter = {component, DE_CHANGED};
    de_query_desc query = {.terms = &term, .term_count = 1, .filters = &filter, .filter_count = 1};
    ASSERT_TRUE(de_system_add(b->world, &(de_system_desc){.name = "editor.source.observe",
                                                          .schedule = DE_SCHEDULE_UPDATE,
                                                          .fn = editor_observe,
                                                          .user = &changes,
                                                          .queries = &query,
                                                          .query_count = 1})
                    .id);
    ASSERT_EQ(de_world_run_schedule(b->world, DE_SCHEDULE_UPDATE, .016f, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(b->world, DE_SCHEDULE_UPDATE, .016f, 0), DE_OK);
    EXPECT_EQ(changes, 0u);
    editing(f)->caret = editing(f)->length;
    de_ui_editor *state = de_entity_get(b->world, f->editor, b->ui->advanced->editor);
    state->anchor = editing(f)->caret;
    input_test_text_edit(f->input, "にほん", 2, 1);
    edit_frame(f);
    const editor_source *read = de_entity_get_const(b->world, source, component);
    EXPECT_TRUE(strcmp(read->text, "Pilot") == 0);
    ASSERT_EQ(de_world_run_schedule(b->world, DE_SCHEDULE_UPDATE, .016f, 0), DE_OK);
    EXPECT_EQ(changes, 0u);
    input_test_text_edit(f->input, "", 0, 0);
    input_test_text(f->input, "!");
    edit_frame(f);
    read = de_entity_get_const(b->world, source, component);
    EXPECT_TRUE(strcmp(read->text, "Pilot!") == 0);
    ASSERT_EQ(de_world_run_schedule(b->world, DE_SCHEDULE_UPDATE, .016f, 0), DE_OK);
    EXPECT_EQ(changes, 1u);
    binding = de_entity_get(b->world, f->editor, binding_id);
    strcpy(binding->path, "EditorSource.locked");
    ui_frame(b);
    EXPECT_TRUE(strcmp(editing(f)->buffer, "Locked") == 0);
    editing(f)->caret = editing(f)->length;
    state = de_entity_get(b->world, f->editor, b->ui->advanced->editor);
    state->anchor = editing(f)->caret;
    input_test_text(f->input, "?");
    edit_frame(f);
    read = de_entity_get_const(b->world, source, component);
    EXPECT_TRUE(strcmp(read->locked, "Locked") == 0);
}
#if defined(DE_UI_TEST_FONTS)
TEST_F(ui_text_input, runtime_password_page_metadata_required_for_retained_extract, edit_fx) {
    runtime_fx fonts = {0};
    runtime_fx_setup(&fonts);
    ui_fx *b = &f->base;
    b->ui->desc.text = fonts.t;
    DeUiText *text = de_entity_get(b->world, f->editor, b->ui->ids.text);
    text->style = (de_text_style){
        .font = fonts.latin, .size_px = 20, .line_height = 1, .color = {1, 1, 1, 1}};
    DeUiTextInput *in = editing(f);
    strcpy(in->buffer, "secret123");
    in->length = in->caret = 9;
    in->password = true;
    for (u32 i = 0; i < 32; ++i)
        edit_frame(f);
    const de_ui_editor *state = de_entity_get_const(b->world, f->editor, b->ui->advanced->editor);
    de_text_style style = text->style;
    de_text_layout *layout = NULL;
    ASSERT_EQ(
        de_text_layout_build(fonts.t,
                             &(de_text_layout_desc){
                                 .utf8 = {state->display, strlen(state->display)}, .style = &style},
                             NULL, &layout),
        DE_OK);
    de_glyph_quad quads[16];
    EXPECT_EQ(de_text_layout_quads(layout, quads, 16), 9u);
    de_text_page_info info = {0};
    EXPECT_EQ(de_text_get_page_info(fonts.t, quads[0].font, quads[0].atlas_page, &info), DE_OK);
    de_text_layout_free(layout);
    b->ui->desc.text = NULL;
    de_ui_attach(b->ui, NULL);
    de_text_destroy(fonts.t);
    fonts.t = NULL;
    runtime_fx_teardown(&fonts);
}
#endif
