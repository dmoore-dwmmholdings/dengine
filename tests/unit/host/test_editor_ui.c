/* DENG-778: ui.layout and the ui.editor capability (DIDE protocol 1.14), through the real
 * handlers and the generated protocol readers and writers. The UI's live viewport is 400x100 with
 * reference height 100 (scale 1), as in tests/unit/ui. */
#include "de_test.h"
#if !defined(DE_SHIPPING) && DE_HAS_UI
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include "ui/ui_internal.h"
#include <dengine/de_ecs.h>
#include <dengine/de_ui.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STORAGE  (1u << 20)
#define RESPONSE (64u << 10)

void de__ed_session_shutdown(void);

typedef struct ui_ed_fx {
    de_engine engine;
    de__host host;
    de__editor ed;
    unsigned char *storage;
    char *response;
    char note[32768];
    char params[512];
    mpack_tree_t tree;
    bool parsed;
} ui_ed_fx;

static void ui_ed_fx_setup(ui_ed_fx *f) {
    memset(f, 0, sizeof *f);
    ASSERT_EQ(de_log_init(&(de_log_desc){.no_console_sink = true}), DE_OK);
    ASSERT_EQ(de_type_registry_create(NULL, &f->engine.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->engine.types}, &f->engine.world), DE_OK);
    (void)de_builtin_components(f->engine.world);
    ASSERT_EQ(
        de_ui_create(&(de_ui_desc){.reference_height = 100, .max_nodes = 1000}, &f->engine.ui),
        DE_OK);
    de_ui_set_viewport(f->engine.ui, 400, 100, (de_window_insets){0});
    de_ui_attach(f->engine.ui, f->engine.world);
    f->storage = (unsigned char *)malloc(STORAGE);
    f->response = (char *)malloc(RESPONSE);
    ASSERT_TRUE(f->storage != NULL && f->response != NULL);
    f->ed.e = &f->engine;
    f->ed.h = &f->host;
    f->ed.mode = DE_EP_WORLD_MODE_EDIT;
    f->ed.arena = f->storage;
    f->ed.arena_cap = STORAGE;
    f->ed.note = f->note;
    f->ed.note_cap = sizeof f->note;
    de__ed_edit_register(&f->ed);
    de__ed_ui_register(&f->ed);
}
static void ui_ed_fx_teardown(ui_ed_fx *f) {
    if (f->parsed)
        mpack_tree_destroy(&f->tree);
    for (u32 i = 0; i < f->ed.reset_count; ++i)
        f->ed.resets[i](&f->ed);
    de__ed_session_shutdown();
    if (f->engine.ui)
        de_ui_destroy(f->engine.ui);
    de_world_destroy(f->engine.world);
    de_type_registry_destroy(f->engine.types);
    free(f->storage);
    free(f->response);
    de_log_shutdown();
}

/* Calls `m` with the `size` bytes in f->params and returns the parsed reply. */
static mpack_node_t call(ui_ed_fx *f, de_ep_method m, size_t size) {
    const de_ep_request req = {
        .id = 778, .method = de_ep_cstr(de_ep_method_name(m)), .params = {f->params, size}};
    ASSERT_TRUE(f->ed.handlers[m] != NULL);
    de_ep_arena a = {f->storage, STORAGE, 0};
    mpack_writer_t w;
    mpack_writer_init(&w, f->response, RESPONSE);
    f->ed.handlers[m](&f->ed, &req, &a, &w);
    const size_t used = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    if (f->parsed)
        mpack_tree_destroy(&f->tree);
    mpack_tree_init_data(&f->tree, f->response, used);
    mpack_tree_parse(&f->tree);
    f->parsed = true;
    ASSERT_EQ(mpack_tree_error(&f->tree), mpack_ok);
    mpack_node_t root = mpack_tree_root(&f->tree);
    ASSERT_EQ(mpack_node_u64(mpack_node_map_cstr(root, "id")), 778u);
    return root;
}
static mpack_node_t layout(ui_ed_fx *f, const de_ep_ui_layout_params *p) {
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, sizeof f->params);
    de_ep_write_ui_layout_params(&w, p);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    return call(f, DE_EP_METHOD_UI_LAYOUT, size);
}
/* The error code, or 0 for a result. */
static int code_of(mpack_node_t root) {
    if (!mpack_node_map_contains_cstr(root, "error")) {
        EXPECT_TRUE(mpack_node_map_contains_cstr(root, "result"));
        return 0;
    }
    return (int)mpack_node_i32(mpack_node_map_cstr(mpack_node_map_cstr(root, "error"), "code"));
}
/* The decoded ui.layout result, through the generated reader. */
static de_ep_ui_layout result_of(ui_ed_fx *f, mpack_node_t root) {
    ASSERT_EQ(code_of(root), 0);
    (void)f;
    mpack_node_t res = mpack_node_map_cstr(root, "result");
    de_ep_ui_layout out = {0};
    out.scale = mpack_node_float(mpack_node_map_cstr(res, "scale"));
    mpack_node_t rects = mpack_node_map_cstr(res, "rects");
    static de_ep_ui_layout_rect items[64];
    static float values[64][4];
    out.rects.count = (u32)mpack_node_array_length(rects);
    ASSERT_TRUE(out.rects.count <= 64);
    out.rects.items = items;
    for (u32 i = 0; i < out.rects.count; ++i) {
        mpack_node_t r = mpack_node_array_at(rects, i);
        items[i].entity = mpack_node_u64(mpack_node_map_cstr(r, "entity"));
        mpack_node_t v = mpack_node_map_cstr(r, "rect");
        ASSERT_EQ(mpack_node_array_length(v), 4u);
        for (u32 k = 0; k < 4; ++k)
            values[i][k] = mpack_node_float(mpack_node_array_at(v, k));
        items[i].rect.items = values[i];
        items[i].rect.count = 4;
    }
    return out;
}
static void expect_rect4(const de_ep_ui_layout_rect *r, de_entity e, f32 x, f32 y, f32 w, f32 h) {
    EXPECT_EQ(r->entity, e.bits);
    EXPECT_FLOAT_EQ(r->rect.items[0], x, .5f);
    EXPECT_FLOAT_EQ(r->rect.items[1], y, .5f);
    EXPECT_FLOAT_EQ(r->rect.items[2], w, .5f);
    EXPECT_FLOAT_EQ(r->rect.items[3], h, .5f);
}

static de_entity node(ui_ed_fx *f, de_entity parent) {
    de_entity e;
    ASSERT_EQ(de_entity_spawn_with(f->engine.world, &f->engine.ui->ids.node, 1, &e), DE_OK);
    if (parent.bits)
        ASSERT_EQ(de_entity_set_parent(f->engine.world, e, parent), DE_OK);
    return e;
}
static DeUiNode *node_edit(ui_ed_fx *f, de_entity e) {
    return de_entity_get(f->engine.world, e, f->engine.ui->ids.node);
}
static de_entity sized(ui_ed_fx *f, de_entity parent, f32 w, f32 h) {
    const de_entity e = node(f, parent);
    DeUiNode *n = node_edit(f, e);
    n->width = (de_ui_len){DE_UI_PX, w};
    n->height = (de_ui_len){DE_UI_PX, h};
    return e;
}
static void frame(ui_ed_fx *f) {
    ASSERT_EQ(de_world_run_schedule(f->engine.world, DE_SCHEDULE_POST_UPDATE, 1.f / 60, 0), DE_OK);
}
static de_vec4 live(ui_ed_fx *f, de_entity e) {
    const DeUiNode *n = de_entity_get_const(f->engine.world, e, f->engine.ui->ids.node);
    ASSERT_TRUE(n != NULL);
    return n->computed_rect;
}

TEST_F(editor_ui, advertises_ui_editor_and_registers_ui_layout, ui_ed_fx) {
    bool found = false;
    for (u32 i = 0; i < f->ed.cap_count; ++i)
        found = found || strcmp(f->ed.caps[i], "ui.editor") == 0;
    EXPECT_TRUE(found);
    EXPECT_TRUE(f->ed.handlers[DE_EP_METHOD_UI_LAYOUT] != NULL);
    EXPECT_STREQ(DE_EP_PROTOCOL_VERSION, "1.14");
}

TEST_F(editor_ui, layout_reports_pre_order_rects_at_the_preview_viewport, ui_ed_fx) {
    const de_entity root = node(f, (de_entity){0});
    node_edit(f, root)->direction = DE_UI_ROW;
    const de_entity a = sized(f, root, 50, 20), b = sized(f, root, 30, 10);
    node_edit(f, a)->margin[3] = (de_ui_len){DE_UI_PX, 5};
    frame(f);
    /* 800x200 is scale 2; uiScale 1.5 makes it 3. */
    const de_ep_ui_layout r = result_of(f, layout(f, &(de_ep_ui_layout_params){.root = root.bits,
                                                                               .width = 800,
                                                                               .height = 200,
                                                                               .has_ui_scale = true,
                                                                               .ui_scale = 1.5f}));
    EXPECT_FLOAT_EQ(r.scale, 3, 1e-5f);
    ASSERT_EQ(r.rects.count, 3u);
    expect_rect4(&r.rects.items[0], root, 0, 0, 800, 200);
    expect_rect4(&r.rects.items[1], a, 15, 0, 150, 60);
    expect_rect4(&r.rects.items[2], b, 165, 0, 90, 30);
    /* The live rects are unchanged. */
    const de_vec4 la = live(f, a);
    EXPECT_FLOAT_EQ(la.x, 5, .5f);
    EXPECT_FLOAT_EQ(la.z, 50, .5f);
    /* Without uiScale the live ui.scale (1) applies: scale 2. */
    const de_ep_ui_layout d = result_of(
        f, layout(f, &(de_ep_ui_layout_params){.root = root.bits, .width = 800, .height = 200}));
    EXPECT_FLOAT_EQ(d.scale, 2, 1e-5f);
    expect_rect4(&d.rects.items[1], a, 10, 0, 100, 40);
}

TEST_F(editor_ui, layout_applies_safe_insets_and_pixel_art, ui_ed_fx) {
    const de_entity root = node(f, (de_entity){0});
    node_edit(f, root)->safe_area = true;
    const de_entity panel = node(f, root);
    node_edit(f, panel)->grow = 1;
    const de_entity leaf = sized(f, panel, 10, 10);
    const de_ep_ui_layout s =
        result_of(f, layout(f, &(de_ep_ui_layout_params){.root = root.bits,
                                                         .width = 300,
                                                         .height = 100,
                                                         .has_ui_scale = true,
                                                         .ui_scale = 1,
                                                         .has_safe = true,
                                                         .safe = {20, 10, 30, 15}}));
    ASSERT_EQ(s.rects.count, 3u);
    expect_rect4(&s.rects.items[0], root, 20, 10, 250, 75);
    expect_rect4(&s.rects.items[2], leaf, 20, 10, 10, 10);
    const de_ep_ui_layout p =
        result_of(f, layout(f, &(de_ep_ui_layout_params){.root = root.bits,
                                                         .width = 320,
                                                         .height = 180,
                                                         .has_ui_scale = true,
                                                         .ui_scale = 2.4f,
                                                         .has_pixel_art = true,
                                                         .pixel_art = true}));
    EXPECT_FLOAT_EQ(p.scale, 2, 0);
    expect_rect4(&p.rects.items[2], leaf, 0, 0, 20, 20);
    /* A subtree root reports only its subtree. */
    const de_ep_ui_layout t = result_of(
        f, layout(f, &(de_ep_ui_layout_params){.root = panel.bits, .width = 400, .height = 100}));
    ASSERT_EQ(t.rects.count, 2u);
    EXPECT_EQ(t.rects.items[0].entity, panel.bits);
    EXPECT_EQ(t.rects.items[1].entity, leaf.bits);
}

/* The DIDE 1.14 conformance flow: an indexed reparent (edit.apply) reorders the siblings, and
 * ui.layout shows the new order; undo restores it. */
TEST_F(editor_ui, indexed_reparent_reorders_the_layout, ui_ed_fx) {
    const de_entity root = node(f, (de_entity){0});
    node_edit(f, root)->direction = DE_UI_ROW;
    const de_entity a = sized(f, root, 10, 10), b = sized(f, root, 20, 10),
                    c = sized(f, root, 30, 10);
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, sizeof f->params);
    de_ep_write_edit_begin_params(
        &w, &(de_ep_edit_begin_params){.label = de_ep_cstr("ui"), .scene = 1});
    size_t n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_node_t r = call(f, DE_EP_METHOD_EDIT_BEGIN, n);
    ASSERT_EQ(code_of(r), 0);
    const u32 tx = mpack_node_u32(mpack_node_map_cstr(mpack_node_map_cstr(r, "result"), "tx"));
    de_ep_edit_op op = {.op = de_ep_cstr("reparent"),
                        .has_entity = true,
                        .entity = c.bits,
                        .has_parent = true,
                        .parent = root.bits,
                        .has_keep_world = true,
                        .keep_world = false,
                        .has_index = true,
                        .index = 0};
    mpack_writer_init(&w, f->params, sizeof f->params);
    de_ep_write_edit_apply_params(&w, &(de_ep_edit_apply_params){.tx = tx, .ops = {&op, 1}});
    n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    ASSERT_EQ(code_of(call(f, DE_EP_METHOD_EDIT_APPLY, n)), 0);
    mpack_writer_init(&w, f->params, sizeof f->params);
    de_ep_write_tx_params(&w, &(de_ep_tx_params){.tx = tx});
    n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    ASSERT_EQ(code_of(call(f, DE_EP_METHOD_EDIT_COMMIT, n)), 0);

    const de_ep_ui_layout_params q = {.root = root.bits, .width = 400, .height = 100};
    de_ep_ui_layout l = result_of(f, layout(f, &q));
    ASSERT_EQ(l.rects.count, 4u);
    expect_rect4(&l.rects.items[1], c, 0, 0, 30, 10);
    expect_rect4(&l.rects.items[2], a, 30, 0, 10, 10);
    expect_rect4(&l.rects.items[3], b, 40, 0, 20, 10);

    mpack_writer_init(&w, f->params, sizeof f->params);
    de_ep_write_tx_params(&w, &(de_ep_tx_params){.tx = tx});
    n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    ASSERT_EQ(code_of(call(f, DE_EP_METHOD_EDIT_UNDO, n)), 0);
    l = result_of(f, layout(f, &q));
    ASSERT_EQ(l.rects.count, 4u);
    expect_rect4(&l.rects.items[1], a, 0, 0, 10, 10);
    expect_rect4(&l.rects.items[2], b, 10, 0, 20, 10);
    expect_rect4(&l.rects.items[3], c, 30, 0, 30, 10);
}

/* The DIDE 1.14 conformance error cases. */
TEST_F(editor_ui, layout_errors_match_the_contract, ui_ed_fx) {
    const de_entity root = node(f, (de_entity){0});
    const de_entity plain = de_entity_spawn(f->engine.world);
    EXPECT_EQ(
        code_of(layout(
            f, &(de_ep_ui_layout_params){.root = UINT64_MAX - 1, .width = 400, .height = 100})),
        DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(code_of(layout(
                  f, &(de_ep_ui_layout_params){.root = plain.bits, .width = 400, .height = 100})),
              DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(
        code_of(layout(f, &(de_ep_ui_layout_params){.root = root.bits, .width = 0, .height = 100})),
        DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(
        code_of(layout(f, &(de_ep_ui_layout_params){.root = root.bits, .width = 400, .height = 0})),
        DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(code_of(layout(f, &(de_ep_ui_layout_params){.root = root.bits,
                                                          .width = 400,
                                                          .height = 100,
                                                          .has_ui_scale = true,
                                                          .ui_scale = INFINITY})),
              DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(code_of(layout(f, &(de_ep_ui_layout_params){.root = root.bits,
                                                          .width = 400,
                                                          .height = 100,
                                                          .has_safe = true,
                                                          .safe = {NAN, 0, 0, 0}})),
              DE_EP_ERR_INVALID_PARAMS);
    /* Malformed params. */
    f->params[0] = (char)0xc1; /* never used in msgpack */
    EXPECT_EQ(code_of(call(f, DE_EP_METHOD_UI_LAYOUT, 1)), DE_EP_ERR_INVALID_PARAMS);
    /* A game without a UI: no node can be found. */
    de_ui_destroy(f->engine.ui);
    f->engine.ui = NULL;
    EXPECT_EQ(code_of(layout(
                  f, &(de_ep_ui_layout_params){.root = root.bits, .width = 400, .height = 100})),
              DE_EP_ERR_NOT_FOUND);
}
#endif
