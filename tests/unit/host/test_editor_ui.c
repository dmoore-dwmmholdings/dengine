/* DENG-778: ui.layout and the ui.editor capability (DIDE protocol 1.14), through the real
 * handlers and the generated protocol readers and writers. DENG-785 (protocol 1.15): the UI pick
 * stack, ui.nodeCorners, the theme methods and the locale methods. The UI's live viewport is 400x100 with
 * reference height 100 (scale 1), as in tests/unit/ui. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "de_test.h"
#if !defined(DE_SHIPPING) && DE_HAS_UI
#include "../ui/style_test_util.h"
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include "ui/ui_internal.h"
#include <dengine/de_ecs.h>
#include <dengine/de_text.h>
#include <dengine/de_ui.h>
#if defined(DE_UI_TEST_FONTS)
#include "../text/runtime_test_util.h"
#endif
#include <dengine/de_debug.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

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
    char params[4096];
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
    f->ed.proto_major = 1; /* as hello agrees with a 1.17 IDE */
    f->ed.proto_minor = 18;
    de__ed_edit_register(&f->ed);
    de__ed_ui_register(&f->ed);
}
static void ui_ed_fx_teardown(ui_ed_fx *f) {
    if (f->parsed)
        mpack_tree_destroy(&f->tree);
    for (u32 i = 0; i < f->ed.reset_count; ++i)
        f->ed.resets[i](&f->ed);
    de__ed_session_shutdown();
    de__ed_ui_shutdown(&f->ed);
    if (f->engine.text)
        de_text_destroy(f->engine.text);
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
    EXPECT_STREQ(DE_EP_PROTOCOL_VERSION, "1.24"); /* pinned dide 406153b1 (DENG-828) */
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
    f->params[0] = '\xc1'; /* never used in msgpack */
    EXPECT_EQ(code_of(call(f, DE_EP_METHOD_UI_LAYOUT, 1)), DE_EP_ERR_INVALID_PARAMS);
    /* A game without a UI: no node can be found. */
    de_ui_destroy(f->engine.ui);
    f->engine.ui = NULL;
    EXPECT_EQ(code_of(layout(
                  f, &(de_ep_ui_layout_params){.root = root.bits, .width = 400, .height = 100})),
              DE_EP_ERR_NOT_FOUND);
}

/* ---- DENG-785: protocol 1.15 ---- */
static const de_ep_method k_115[] = {DE_EP_METHOD_UI_NODE_CORNERS,  DE_EP_METHOD_UI_THEMES,
                                     DE_EP_METHOD_UI_THEME_VARS,    DE_EP_METHOD_UI_PREVIEW_THEME,
                                     DE_EP_METHOD_LOC_LOCALES,      DE_EP_METHOD_LOC_STRINGS,
                                     DE_EP_METHOD_LOC_PREVIEW_LOCALE};
/* Calls `m` with an empty params map. */
static mpack_node_t call_empty(ui_ed_fx *f, de_ep_method m) {
    f->params[0] = '\x80';
    return call(f, m, 1);
}
/* Calls `m` with {key: value}; value NULL sends an explicit nil. */
static mpack_node_t call_str(ui_ed_fx *f, de_ep_method m, const char *key, const char *value) {
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, sizeof f->params);
    mpack_start_map(&w, 1);
    mpack_write_cstr(&w, key);
    if (value)
        mpack_write_cstr(&w, value);
    else
        mpack_write_nil(&w);
    mpack_finish_map(&w);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    return call(f, m, size);
}
static mpack_node_t corners(ui_ed_fx *f, u64 entity) {
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, sizeof f->params);
    de_ep_write_ui_node_corners_params(&w, &(de_ep_ui_node_corners_params){.entity = entity});
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    return call(f, DE_EP_METHOD_UI_NODE_CORNERS, size);
}
static mpack_node_t res(mpack_node_t root) {
    ASSERT_EQ(code_of(root), 0);
    return mpack_node_map_cstr(root, "result");
}
static bool str_is(mpack_node_t n, const char *s) {
    return mpack_node_type(n) == mpack_type_str && mpack_node_strlen(n) == strlen(s) &&
           memcmp(mpack_node_str(n), s, strlen(s)) == 0;
}
static mpack_node_t find_var(mpack_node_t vars, const char *name) {
    for (size_t i = 0; i < mpack_node_array_length(vars); ++i) {
        mpack_node_t v = mpack_node_array_at(vars, i);
        if (str_is(mpack_node_map_cstr(v, "name"), name))
            return v;
    }
    ASSERT_TRUE(!"variable not found");
    return mpack_node_array_at(vars, 0);
}
static de_entity boxed(ui_ed_fx *f, de_entity parent, f32 x, f32 y, f32 w, f32 h) {
    de_entity e = node(f, parent);
    *(DeUiAnchor *)de_entity_add(f->engine.world, e, f->engine.ui->ids.anchor) =
        (DeUiAnchor){.offset_min = {x, y}, .offset_max = {x + w, y + h}};
    return e;
}
static de_entity panel_at(ui_ed_fx *f, de_dvec3 position) {
    de_entity root = node(f, (de_entity){0});
    de_component panel = de_component_find(f->engine.world, "DeUiWorldPanel");
    ASSERT_TRUE(panel.id);
    *(DeUiWorldPanel *)de_entity_add(f->engine.world, root, panel) =
        (DeUiWorldPanel){.size_m = {2, 1}, .pixels_per_meter = 100, .depth_test = true};
    de_transform t = de_transform_identity();
    t.position = position;
    *(DeTransform *)de_entity_add(f->engine.world, root, f->engine.ui->hierarchy.transform) = t;
    *(DeGlobalTransform *)de_entity_add(f->engine.world, root,
                                        f->engine.ui->hierarchy.global_transform) = t;
    return root;
}

TEST_F(editor_ui, registers_the_1_15_methods_and_gates_them_on_the_agreed_version, ui_ed_fx) {
    for (u32 i = 0; i < (u32)(sizeof k_115 / sizeof k_115[0]); ++i)
        EXPECT_TRUE(f->ed.handlers[k_115[i]] != NULL);
    /* An IDE that agreed 1.14: every 1.15 method is unsupported, and there is no pick stack. */
    f->ed.proto_minor = 14;
    for (u32 i = 0; i < (u32)(sizeof k_115 / sizeof k_115[0]); ++i)
        EXPECT_EQ(code_of(call_empty(f, k_115[i])), DE_EP_ERR_UNSUPPORTED);
    const de_entity root = boxed(f, (de_entity){0}, 0, 0, 100, 50);
    frame(f);
    de__ed_ui_hit hits[DE__ED_UI_STACK];
    EXPECT_EQ(de__ed_ui_pick(&f->ed, 10, 10, NULL, NULL, hits, DE__ED_UI_STACK), 0u);
    f->ed.proto_minor = 15;
    ASSERT_EQ(de__ed_ui_pick(&f->ed, 10, 10, NULL, NULL, hits, DE__ED_UI_STACK), 1u);
    EXPECT_EQ(hits[0].entity, root.bits);
    EXPECT_TRUE(de__ed_protocol_at_least(&f->ed, 1, 15));
    EXPECT_FALSE(de__ed_protocol_at_least(&f->ed, 1, 16));
    EXPECT_FALSE(de__ed_protocol_at_least(&f->ed, 2, 0));
}

TEST_F(editor_ui, themes_list_vars_and_preview, ui_ed_fx) {
    ASSERT_EQ(style_install(f->engine.ui, "[vars]\na='#fff'\nb=4\n[theme.dark.vars]\na='#000'\n"
                                          "[theme.contrast.vars]\nc=1\n"),
              DE_OK);
    mpack_node_t r = res(call_empty(f, DE_EP_METHOD_UI_THEMES));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "active"), ""));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "selected"), ""));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "previewing")));
    mpack_node_t themes = mpack_node_map_cstr(r, "themes");
    ASSERT_EQ(mpack_node_array_length(themes), 2u);
    EXPECT_TRUE(str_is(mpack_node_map_cstr(mpack_node_array_at(themes, 0), "name"), "contrast"));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(mpack_node_array_at(themes, 1), "name"), "dark"));
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(mpack_node_array_at(themes, 1), "sheets")), 1u);

    /* Variables: the overlay's values for "dark", the base ones for "" and for the active
     * theme (absent). A sheet loaded from source text has no path: "sheet:<index>" (DENG-786). */
    mpack_node_t v = find_var(mpack_node_map_cstr(res(call_str(f, DE_EP_METHOD_UI_THEME_VARS,
                                                               "theme", "dark")),
                                                  "vars"),
                              "a");
    EXPECT_TRUE(str_is(mpack_node_map_cstr(v, "value"), "\"#000\""));
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(v, "themed")));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(v, "truncated")));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(v, "sheet"), "sheet:0"));
    v = find_var(mpack_node_map_cstr(res(call_str(f, DE_EP_METHOD_UI_THEME_VARS, "theme", "")),
                                     "vars"),
                 "a");
    EXPECT_TRUE(str_is(mpack_node_map_cstr(v, "value"), "\"#fff\""));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(v, "themed")));
    mpack_node_t vars = mpack_node_map_cstr(res(call_empty(f, DE_EP_METHOD_UI_THEME_VARS)), "vars");
    EXPECT_EQ(mpack_node_array_length(vars), 2u);
    v = find_var(vars, "b");
    EXPECT_TRUE(str_is(mpack_node_map_cstr(v, "value"), "4"));
    EXPECT_EQ(code_of(call_str(f, DE_EP_METHOD_UI_THEME_VARS, "theme", "undeclared")),
              DE_EP_ERR_NOT_FOUND);
    char long_name[80];
    memset(long_name, 'x', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = 0;
    EXPECT_EQ(code_of(call_str(f, DE_EP_METHOD_UI_THEME_VARS, "theme", long_name)),
              DE_EP_ERR_NOT_FOUND);

    /* Preview: the reply is the new ui.themes state; absent or nil ends it. */
    r = res(call_str(f, DE_EP_METHOD_UI_PREVIEW_THEME, "name", "dark"));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "active"), "dark"));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "selected"), ""));
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(r, "previewing")));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "themes")), 2u);
    EXPECT_EQ(code_of(call_str(f, DE_EP_METHOD_UI_PREVIEW_THEME, "name", "undeclared")),
              DE_EP_ERR_NOT_FOUND);
    char too_long[65];
    memset(too_long, 'y', 64);
    too_long[64] = 0;
    EXPECT_EQ(code_of(call_str(f, DE_EP_METHOD_UI_PREVIEW_THEME, "name", too_long)),
              DE_EP_ERR_INVALID_PARAMS);
    r = res(call_str(f, DE_EP_METHOD_UI_PREVIEW_THEME, "name", NULL)); /* explicit nil */
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "previewing")));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "active"), ""));
    ASSERT_EQ(code_of(call_str(f, DE_EP_METHOD_UI_PREVIEW_THEME, "name", "contrast")), 0);
    r = res(call_empty(f, DE_EP_METHOD_UI_PREVIEW_THEME)); /* absent */
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "previewing")));
    r = res(call_empty(f, DE_EP_METHOD_UI_PREVIEW_THEME)); /* no preview: still OK */
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "previewing")));

    /* A disconnect ends a preview the IDE left on. */
    ASSERT_EQ(code_of(call_str(f, DE_EP_METHOD_UI_PREVIEW_THEME, "name", "dark")), 0);
    for (u32 i = 0; i < f->ed.reset_count; ++i)
        f->ed.resets[i](&f->ed);
    de_ui_theme_state state;
    ASSERT_EQ(de_ui_get_theme_state(f->engine.ui, &state), DE_OK);
    EXPECT_FALSE(state.previewing);
    EXPECT_STREQ(state.active, "");
}

TEST_F(editor_ui, themes_without_a_ui_are_empty, ui_ed_fx) {
    de_ui_destroy(f->engine.ui);
    f->engine.ui = NULL;
    mpack_node_t r = res(call_empty(f, DE_EP_METHOD_UI_THEMES));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "themes")), 0u);
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "previewing")));
    EXPECT_EQ(mpack_node_array_length(
                  mpack_node_map_cstr(res(call_empty(f, DE_EP_METHOD_UI_THEME_VARS)), "vars")),
              0u);
    EXPECT_EQ(code_of(call_str(f, DE_EP_METHOD_UI_THEME_VARS, "theme", "")), 0);
    EXPECT_EQ(code_of(call_str(f, DE_EP_METHOD_UI_THEME_VARS, "theme", "dark")),
              DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(code_of(call_empty(f, DE_EP_METHOD_UI_PREVIEW_THEME)), 0);
    EXPECT_EQ(code_of(call_str(f, DE_EP_METHOD_UI_PREVIEW_THEME, "name", "dark")),
              DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(code_of(corners(f, 1)), DE_EP_ERR_NOT_FOUND);
    de__ed_ui_hit hits[4];
    EXPECT_EQ(de__ed_ui_pick(&f->ed, 1, 1, NULL, NULL, hits, 4), 0u);
    for (u32 i = 0; i < f->ed.reset_count; ++i) /* the reset hook copes without a UI */
        f->ed.resets[i](&f->ed);
}

TEST_F(editor_ui, locales_strings_and_preview, ui_ed_fx) {
    ASSERT_EQ(de_text_create(NULL, NULL, &f->engine.text), DE_OK);
    mpack_node_t r = res(call_empty(f, DE_EP_METHOD_LOC_LOCALES));
    de_loc_locale_state want;
    ASSERT_EQ(de_loc_get_locale_state(f->engine.text, &want), DE_OK);
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "active"), want.active));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "fallback"), want.fallback));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "preview"), ""));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "previewing")));
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(r, "generation")), want.generation);
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "locales")), 0u);
    r = res(call_empty(f, DE_EP_METHOD_LOC_STRINGS));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "strings")), 0u);
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "locale"), want.active));

    EXPECT_EQ(code_of(call_str(f, DE_EP_METHOD_LOC_PREVIEW_LOCALE, "locale", "not a locale")),
              DE_EP_ERR_INVALID_PARAMS);
    char long_tag[48];
    memset(long_tag, 'a', sizeof long_tag - 1);
    long_tag[sizeof long_tag - 1] = 0;
    EXPECT_EQ(code_of(call_str(f, DE_EP_METHOD_LOC_PREVIEW_LOCALE, "locale", long_tag)),
              DE_EP_ERR_INVALID_PARAMS);
    r = res(call_str(f, DE_EP_METHOD_LOC_PREVIEW_LOCALE, "locale", "pt-br"));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "preview"), "pt-BR"));
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(r, "previewing")));
    r = res(call_str(f, DE_EP_METHOD_LOC_PREVIEW_LOCALE, "locale", NULL)); /* explicit nil */
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "previewing")));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "preview"), ""));
    EXPECT_EQ(code_of(call_empty(f, DE_EP_METHOD_LOC_PREVIEW_LOCALE)), 0);

    ASSERT_EQ(code_of(call_str(f, DE_EP_METHOD_LOC_PREVIEW_LOCALE, "locale", "de")), 0);
    for (u32 i = 0; i < f->ed.reset_count; ++i)
        f->ed.resets[i](&f->ed);
    ASSERT_EQ(de_loc_get_locale_state(f->engine.text, &want), DE_OK);
    EXPECT_FALSE(want.previewing);
}

/* ---- DENG-820: protocol 1.21 loc.pseudo and ui.textOverflow ---- */
static mpack_node_t call_pseudo(ui_ed_fx *f, int enabled) { /* -1 absent, -2 nil, else a bool */
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, sizeof f->params);
    if (enabled == -2) {
        mpack_start_map(&w, 1);
        mpack_write_cstr(&w, "enabled");
        mpack_write_nil(&w);
        mpack_finish_map(&w);
    } else {
        de_ep_write_loc_pseudo_params(
            &w, &(de_ep_loc_pseudo_params){.has_enabled = enabled >= 0, .enabled = enabled > 0});
    }
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    return call(f, DE_EP_METHOD_LOC_PSEUDO, size);
}
#if !defined(_WIN32) /* only the non-Windows overflow tests call it */
static mpack_node_t call_overflow(ui_ed_fx *f, bool has_root, u64 root) {
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, sizeof f->params);
    de_ep_write_ui_text_overflow_params(
        &w, &(de_ep_ui_text_overflow_params){.has_root = has_root, .root = root});
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    return call(f, DE_EP_METHOD_UI_TEXT_OVERFLOW, size);
}
#endif
TEST_F(editor_ui, pseudo_and_overflow_are_registered_and_gated_on_protocol_1_21, ui_ed_fx) {
    EXPECT_TRUE(f->ed.handlers[DE_EP_METHOD_LOC_PSEUDO] != NULL);
    EXPECT_TRUE(f->ed.handlers[DE_EP_METHOD_UI_TEXT_OVERFLOW] != NULL);
    f->ed.proto_minor = 20;
    EXPECT_EQ(code_of(call_empty(f, DE_EP_METHOD_LOC_PSEUDO)), DE_EP_ERR_UNSUPPORTED);
    EXPECT_EQ(code_of(call_empty(f, DE_EP_METHOD_UI_TEXT_OVERFLOW)), DE_EP_ERR_UNSUPPORTED);
    f->ed.proto_minor = 21;
    EXPECT_EQ(code_of(call_empty(f, DE_EP_METHOD_LOC_PSEUDO)), 0);
    EXPECT_EQ(code_of(call_empty(f, DE_EP_METHOD_UI_TEXT_OVERFLOW)), 0);
}

TEST_F(editor_ui, loc_pseudo_reads_sets_and_reports_in_loc_locales, ui_ed_fx) {
    f->ed.proto_minor = 21;
    /* No text system: a read is empty and off; turning it on is not_found. */
    mpack_node_t r = res(call_pseudo(f, -1));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "pseudo")));
    EXPECT_EQ(code_of(call_pseudo(f, 1)), DE_EP_ERR_NOT_FOUND);

    ASSERT_EQ(de_text_create(NULL, NULL, &f->engine.text), DE_OK);
    r = res(call_pseudo(f, -1));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "pseudo")));
    r = res(call_empty(f, DE_EP_METHOD_LOC_LOCALES));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "pseudo")));
#if DE_NAME_LOOKUP
    de_loc_locale_state st;
    r = res(call_pseudo(f, 1)); /* the requested state at once; published follows a generation */
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(r, "pseudo")));
    ASSERT_EQ(de_loc_get_locale_state(f->engine.text, &st), DE_OK);
    EXPECT_TRUE(st.pseudo);
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(r, "generation")), st.generation);
    r = res(call_pseudo(f, -1)); /* only reads */
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(r, "pseudo")));
    r = res(call_pseudo(f, -2)); /* nil only reads too */
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(r, "pseudo")));
    r = res(call_empty(f, DE_EP_METHOD_LOC_LOCALES));
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(r, "pseudo")));
    r = res(call_str(f, DE_EP_METHOD_LOC_PREVIEW_LOCALE, "locale", NULL));
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(r, "pseudo")));
    for (u32 i = 0; i < f->ed.reset_count; ++i) /* a disconnect ends what the IDE turned on */
        f->ed.resets[i](&f->ed);
    ASSERT_EQ(de_loc_get_locale_state(f->engine.text, &st), DE_OK);
    EXPECT_FALSE(st.pseudo);
    r = res(call_pseudo(f, 0));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "pseudo")));
#endif
}


TEST_F(editor_ui, node_corners_on_a_world_panel, ui_ed_fx) {
    const de_entity panel = panel_at(f, (de_dvec3){100000, 0, 0});
    const de_entity inner = boxed(f, panel, 50, 25, 100, 50);
    const de_entity screen = node(f, (de_entity){0});
    frame(f);
    mpack_node_t r = res(corners(f, inner.bits));
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(r, "panel")), panel.bits);
    mpack_node_t c = mpack_node_map_cstr(r, "corners");
    ASSERT_EQ(mpack_node_array_length(c), 12u);
    const f64 want[4][3] = {{-.5, .25, 0}, {.5, .25, 0}, {.5, -.25, 0}, {-.5, -.25, 0}};
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ((f32)(mpack_node_double(mpack_node_array_at(c, i * 3)) - 100000),
                        (f32)want[i][0], 1e-4f);
        EXPECT_FLOAT_EQ((f32)mpack_node_double(mpack_node_array_at(c, i * 3 + 1)),
                        (f32)want[i][1], 1e-4f);
        EXPECT_FLOAT_EQ((f32)mpack_node_double(mpack_node_array_at(c, i * 3 + 2)),
                        (f32)want[i][2], 1e-4f);
    }
    EXPECT_EQ(code_of(corners(f, screen.bits)), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(code_of(corners(f, UINT64_MAX - 1)), DE_EP_ERR_NOT_FOUND);
}

TEST_F(editor_ui, ui_pick_stack_on_screen_and_through_a_ray, ui_ed_fx) {
    const de_entity root = node(f, (de_entity){0});
    const de_entity a = boxed(f, root, 10, 10, 100, 50);
    const de_entity b = boxed(f, root, 50, 20, 100, 50);
    const de_entity panel = panel_at(f, (de_dvec3){100000.5, 0, 0});
    const de_entity button = boxed(f, panel, 0, 0, 50, 50);
    frame(f);
    de__ed_ui_hit hits[DE__ED_UI_STACK];
    /* A game view: the screen UI at the viewport pixel, topmost first. */
    ASSERT_EQ(de__ed_ui_pick(&f->ed, 65, 35, NULL, NULL, hits, DE__ED_UI_STACK), 3u);
    EXPECT_EQ(hits[0].entity, b.bits);
    EXPECT_EQ(hits[1].entity, a.bits);
    EXPECT_EQ(hits[2].entity, root.bits);
    EXPECT_EQ(hits[0].root, root.bits);
    EXPECT_EQ(hits[0].depth, 1u);
    EXPECT_EQ(hits[2].depth, 0u);
    EXPECT_FLOAT_EQ(hits[0].rect[0], 50, 1e-3f);
    EXPECT_FLOAT_EQ(hits[0].rect[1], 20, 1e-3f);
    EXPECT_FLOAT_EQ(hits[0].rect[2], 100, 1e-3f);
    EXPECT_FLOAT_EQ(hits[0].rect[3], 50, 1e-3f);
    /* Capacity keeps the topmost entries. */
    EXPECT_EQ(de__ed_ui_pick(&f->ed, 65, 35, NULL, NULL, hits, 1), 1u);
    EXPECT_EQ(hits[0].entity, b.bits);
    /* A scene view: the world panel under the camera ray. */
    const f64 origin[3] = {100000 - 0.25, 0.25, 5};
    const f32 dir[3] = {0, 0, -1};
    ASSERT_EQ(de__ed_ui_pick(&f->ed, 0, 0, origin, dir, hits, DE__ED_UI_STACK), 2u);
    EXPECT_EQ(hits[0].entity, button.bits);
    EXPECT_EQ(hits[1].entity, panel.bits);
    EXPECT_EQ(hits[0].root, panel.bits);
    const f64 miss[3] = {0, 0, 5};
    EXPECT_EQ(de__ed_ui_pick(&f->ed, 0, 0, miss, dir, hits, DE__ED_UI_STACK), 0u);
    const f32 zero[3] = {0, 0, 0};
    EXPECT_EQ(de__ed_ui_pick(&f->ed, 0, 0, origin, zero, hits, DE__ED_UI_STACK), 0u);

    /* PickResult.uiStack through the generated writer: absent when empty. */
    de_ep_ui_pick_hit items[DE__ED_UI_STACK];
    const u32 n = de__ed_ui_pick(&f->ed, 65, 35, NULL, NULL, hits, DE__ED_UI_STACK);
    for (u32 pass = 0; pass < 2; ++pass) {
        de_ep_pick_result pr = {.has_entity = true, .entity = hits[0].entity};
        de__ed_pick_set_ui_stack(&pr, hits, pass ? 0 : n, items);
        mpack_writer_t w;
        mpack_writer_init(&w, f->response, RESPONSE);
        de_ep_write_response_pick(&w, 778, &pr);
        const size_t used = mpack_writer_buffer_used(&w);
        ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
        if (f->parsed)
            mpack_tree_destroy(&f->tree);
        mpack_tree_init_data(&f->tree, f->response, used);
        mpack_tree_parse(&f->tree);
        f->parsed = true;
        mpack_node_t r = res(mpack_tree_root(&f->tree));
        if (pass) {
            EXPECT_FALSE(mpack_node_map_contains_cstr(r, "uiStack"));
            continue;
        }
        mpack_node_t stack = mpack_node_map_cstr(r, "uiStack");
        ASSERT_EQ(mpack_node_array_length(stack), 3u);
        mpack_node_t top = mpack_node_array_at(stack, 0);
        EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(top, "entity")), b.bits);
        EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(top, "root")), root.bits);
        EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(top, "depth")), 1u);
        mpack_node_t rect = mpack_node_map_cstr(top, "rect");
        ASSERT_EQ(mpack_node_array_length(rect), 4u);
        EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(rect, 2)), 100, 1e-3f);
    }
}

/* ---- DENG-787: protocol 1.16, ui.stylesheets and ui.checkStylesheet ---- */
static mpack_node_t check_text(ui_ed_fx *f, const char *text) {
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, sizeof f->params);
    de_ep_write_ui_check_stylesheet_params(&w,
                                           &(de_ep_ui_check_stylesheet_params){.text = de_ep_cstr(text)});
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    return call(f, DE_EP_METHOD_UI_CHECK_STYLESHEET, size);
}
static bool has_cap(ui_ed_fx *f, const char *name) {
    for (u32 i = 0; i < f->ed.cap_count; ++i)
        if (strcmp(f->ed.caps[i], name) == 0)
            return true;
    return false;
}
static bool is_absent(mpack_node_t map, const char *key) {
    return mpack_node_is_missing(mpack_node_map_cstr_optional(map, key)) ||
           mpack_node_type(mpack_node_map_cstr_optional(map, key)) == mpack_type_nil;
}

TEST_F(editor_ui, advertises_ui_stylesheets_and_gates_the_1_16_methods, ui_ed_fx) {
    EXPECT_TRUE(has_cap(f, "ui.stylesheets"));
    EXPECT_TRUE(f->ed.handlers[DE_EP_METHOD_UI_STYLESHEETS] != NULL);
    EXPECT_TRUE(f->ed.handlers[DE_EP_METHOD_UI_CHECK_STYLESHEET] != NULL);
    f->ed.proto_minor = 15; /* an IDE that agreed 1.15 */
    EXPECT_EQ(code_of(call_empty(f, DE_EP_METHOD_UI_STYLESHEETS)), DE_EP_ERR_UNSUPPORTED);
    EXPECT_EQ(code_of(check_text(f, "")), DE_EP_ERR_UNSUPPORTED);
    f->ed.proto_minor = 16;
    EXPECT_TRUE(de__ed_protocol_at_least(&f->ed, 1, 16));
    EXPECT_EQ(code_of(call_empty(f, DE_EP_METHOD_UI_STYLESHEETS)), 0);
}

TEST_F(editor_ui, stylesheets_list_loaded_failed_and_rejected_reload, ui_ed_fx) {
    mpack_node_t r = res(call_empty(f, DE_EP_METHOD_UI_STYLESHEETS));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "suffix"), DE_UI_STYLESHEET_SUFFIX));
    const u32 builtin = (u32)mpack_node_array_length(mpack_node_map_cstr(r, "sheets"));
    ASSERT_TRUE(builtin >= 1u); /* the embedded widget theme */

    ASSERT_EQ(style_install(f->engine.ui, "[vars]\na=1\n"), DE_OK);
    ASSERT_EQ(style_install(f->engine.ui, "[vars]\nb=2\n"), DE_OK);
    de_ui_style_system *st = f->engine.ui->style;
    ASSERT_EQ(st->sheet_count, 2u);
    /* A rejected reload of the second sheet: loaded and an error, the old rules stay. */
    st->sheets[1].failed = true;
    st->sheets[1].error = (de_ui_sheet_error){.line = 4, .column = 9};
    snprintf(st->sheets[1].error.message, sizeof st->sheets[1].error.message, "bad value");
    /* A failed first load, with a column but no line: the column must not be sent. */
    st->failed[st->failed_count++] =
        (de_ui_style_failed_sheet){.error = {.line = 0, .column = 5}};
    snprintf(st->failed[0].error.message, sizeof st->failed[0].error.message, "cannot read");

    r = res(call_empty(f, DE_EP_METHOD_UI_STYLESHEETS));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "suffix"), ".uistyle.toml"));
    mpack_node_t sheets = mpack_node_map_cstr(r, "sheets");
    ASSERT_EQ(mpack_node_array_length(sheets), 3u);
    for (u32 i = 0; i < 3; ++i) {
        mpack_node_t s = mpack_node_array_at(sheets, i);
        EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(s, "index")), i);
        EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(s, "builtin")));
        EXPECT_TRUE(str_is(mpack_node_map_cstr(s, "path"), ""));
        EXPECT_TRUE(str_is(mpack_node_map_cstr(s, "asset"), ""));
        EXPECT_TRUE(str_is(mpack_node_map_cstr(s, "source"), ""));
    }
    mpack_node_t ok = mpack_node_array_at(sheets, 0);
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(ok, "loaded")));
    EXPECT_TRUE(is_absent(ok, "error"));
    mpack_node_t rej = mpack_node_array_at(sheets, 1);
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(rej, "loaded")));
    mpack_node_t e = mpack_node_map_cstr(rej, "error");
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(e, "line")), 4u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(e, "column")), 9u);
    EXPECT_TRUE(str_is(mpack_node_map_cstr(e, "message"), "bad value"));
    mpack_node_t failed = mpack_node_array_at(sheets, 2);
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(failed, "loaded")));
    e = mpack_node_map_cstr(failed, "error");
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(e, "line")), 0u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(e, "column")), 0u); /* never a column at line 0 */
    EXPECT_TRUE(str_is(mpack_node_map_cstr(e, "message"), "cannot read"));
}

TEST_F(editor_ui, stylesheets_without_a_ui_are_empty, ui_ed_fx) {
    de_ui_destroy(f->engine.ui);
    f->engine.ui = NULL;
    mpack_node_t r = res(call_empty(f, DE_EP_METHOD_UI_STYLESHEETS));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "suffix"), DE_UI_STYLESHEET_SUFFIX));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "sheets")), 0u);
}

TEST_F(editor_ui, check_stylesheet_reports_ok_and_positions, ui_ed_fx) {
    mpack_node_t r = res(check_text(f, ""));
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(r, "ok")));
    EXPECT_TRUE(is_absent(r, "error"));
    r = res(check_text(f, "[vars]\na=1\n[style.button]\nopacity=0.5\n"));
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(r, "ok")));
    EXPECT_TRUE(is_absent(r, "error"));
    r = res(check_text(f, "[style.button]\nopacity = 0.5\nradius = 'open\n"));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "ok")));
    mpack_node_t e = mpack_node_map_cstr(r, "error");
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(e, "line")), 3u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(e, "column")), 10u);
    EXPECT_TRUE(mpack_node_strlen(mpack_node_map_cstr(e, "message")) > 0);
    r = res(check_text(f, "[style.button]\nopacity='wrong'"));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "ok")));
    e = mpack_node_map_cstr(r, "error");
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(e, "line")), 2u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(e, "column")), 9u);
    /* Checking never loads: the sheet list is unchanged. */
    const size_t before = mpack_node_array_length(
        mpack_node_map_cstr(res(call_empty(f, DE_EP_METHOD_UI_STYLESHEETS)), "sheets"));
    (void)res(check_text(f, "[vars]\nz=1\n"));
    EXPECT_EQ(mpack_node_array_length(
                  mpack_node_map_cstr(res(call_empty(f, DE_EP_METHOD_UI_STYLESHEETS)), "sheets")),
              before);
    EXPECT_EQ(code_of(call_empty(f, DE_EP_METHOD_UI_CHECK_STYLESHEET)), DE_EP_ERR_INVALID_PARAMS);
}

#if !defined(_WIN32)
/* Sheets from the manifest: the asset id, the manifest path and the source relative to the asset
 * root, and error.file taken from that source. */
TEST_F(editor_ui, stylesheets_carry_manifest_path_source_and_error_file, ui_ed_fx) {
    char directory[] = "/tmp/deng-edui-sheets-XXXXXX";
    ASSERT_TRUE(mkdtemp(directory) != NULL);
    ASSERT_EQ(de_fs_mount("edsheets", directory, DE_MOUNT_WRITABLE, 0), DE_OK);
    static const char *const names[] = {"assets.toml", "hud.uistyle.toml", "broken.uistyle.toml"};
    static const char *const texts[] = {
        "[project]\nasset_root='.'\nversion=1\n[assets]\n"
        "'ui/hud'={type='ui_style',source='hud.uistyle.toml'}\n"
        "'ui/broken'={type='ui_style',source='broken.uistyle.toml'}\n",
        "[style.button]\nopacity=0.2\n", "[style.button]\n\nopacity = 'x'\n"};
    for (u32 i = 0; i < 3; ++i) {
        char path[64];
        snprintf(path, sizeof path, "edsheets:/%s", names[i]);
        ASSERT_EQ(de_file_write_all_atomic(path, texts[i], strlen(texts[i])), DE_OK);
    }
    de_ui_destroy(f->engine.ui);
    f->engine.ui = NULL;
    de_asset_system *assets = NULL;
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){0}, &assets), DE_OK);
    ASSERT_EQ(de_ui_create(&(de_ui_desc){.assets = assets, .max_nodes = 1000,
                                         .reference_height = 100},
                           &f->engine.ui),
              DE_OK);
    ASSERT_EQ(de_asset_manifest_load(assets, "edsheets:/assets.toml"), DE_OK);
    de_asset_id ids[] = {de_asset_id_from("ui/hud"), de_asset_id_from("ui/broken")};
    de_load_handle load = de_asset_load_many(assets, ids, 2, 0);
    de_asset_wait(assets, load, 30000000000ULL);
    ASSERT_EQ(de_ui_load_stylesheet(f->engine.ui, de_ui_stylesheet(f->engine.ui, ids[0])), DE_OK);
    EXPECT_TRUE(de_ui_load_stylesheet(f->engine.ui, de_ui_stylesheet(f->engine.ui, ids[1])) !=
                DE_OK);

    mpack_node_t sheets = mpack_node_map_cstr(res(call_empty(f, DE_EP_METHOD_UI_STYLESHEETS)),
                                              "sheets");
    const size_t n = mpack_node_array_length(sheets);
    ASSERT_TRUE(n >= 3u);
    mpack_node_t hud = mpack_node_array_at(sheets, n - 2);
    mpack_node_t broken = mpack_node_array_at(sheets, n - 1); /* failed first loads come last */
    EXPECT_TRUE(str_is(mpack_node_map_cstr(hud, "path"), "ui/hud"));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(hud, "asset"), "ui/hud"));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(hud, "source"), "hud.uistyle.toml"));
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(hud, "loaded")));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(hud, "builtin")));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(broken, "loaded")));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(broken, "source"), "broken.uistyle.toml"));
    mpack_node_t e = mpack_node_map_cstr(broken, "error");
    EXPECT_TRUE(str_is(mpack_node_map_cstr(e, "file"), "broken.uistyle.toml"));
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(e, "line")), 3u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(e, "column")), 11u);

    de_ui_destroy(f->engine.ui);
    f->engine.ui = NULL;
    de_asset_release(assets, load);
    de_asset_system_destroy(assets);
    for (u32 i = 0; i < 3; ++i) {
        char path[64];
        snprintf(path, sizeof path, "edsheets:/%s", names[i]);
        EXPECT_EQ(de_file_delete(path), DE_OK);
    }
    EXPECT_EQ(de_fs_unmount("edsheets", directory), DE_OK);
    EXPECT_EQ(rmdir(directory), 0);
}
/* ---- DENG-788: protocol 1.17, ui.measureText and the capability ui.textMetrics ---- */
TEST_F(editor_ui, text_metrics_needs_a_text_system_and_protocol_1_17, ui_ed_fx) {
    /* The fixture has no text system: the capability is not listed and a call is unsupported. */
    EXPECT_FALSE(has_cap(f, "ui.textMetrics"));
    ASSERT_TRUE(f->ed.handlers[DE_EP_METHOD_UI_MEASURE_TEXT] != NULL);
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, sizeof f->params);
    de_ep_write_ui_measure_text_params(
        &w, &(de_ep_ui_measure_text_params){.text = de_ep_cstr("Hi"),
                                            .style = {.font = de_ep_cstr("font/latin")}});
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    EXPECT_EQ(code_of(call(f, DE_EP_METHOD_UI_MEASURE_TEXT, size)), DE_EP_ERR_UNSUPPORTED);
}
#if defined(DE_UI_TEST_FONTS)
typedef struct text_env {
    runtime_fx fonts;
} text_env;
static void text_env_up(ui_ed_fx *f, text_env *t) {
    memset(t, 0, sizeof *t);
    runtime_fx_setup(&t->fonts);
    f->engine.text = t->fonts.t;
    f->engine.assets = t->fonts.assets;
    f->engine.ui->desc.text = t->fonts.t;
    de__ed_ui_register(&f->ed); /* the capability follows the text system */
}
static void text_env_down(ui_ed_fx *f, text_env *t) {
    f->engine.text = NULL;
    f->engine.assets = NULL;
    f->engine.ui->desc.text = NULL;
    runtime_fx_teardown(&t->fonts);
}
typedef struct measure_in {
    const char *text, *font;
    bool has_size, has_spacing, has_line_height, has_align, has_wrap, has_box, has_scale;
    f32 size, spacing, line_height, wrap, box, scale;
    de_ep_ui_text_align align;
    const char *language;
} measure_in;
static mpack_node_t measure(ui_ed_fx *f, const measure_in *in) {
    de_ep_ui_measure_text_params p = {.text = de_ep_cstr(in->text),
                                      .style = {.font = de_ep_cstr(in->font ? in->font : "font/latin"),
                                                .has_size_px = in->has_size,
                                                .size_px = in->size,
                                                .has_letter_spacing = in->has_spacing,
                                                .letter_spacing = in->spacing,
                                                .has_line_height = in->has_line_height,
                                                .line_height = in->line_height},
                                      .has_align = in->has_align,
                                      .align = in->align,
                                      .has_wrap_width = in->has_wrap,
                                      .wrap_width = in->wrap,
                                      .has_box_height = in->has_box,
                                      .box_height = in->box,
                                      .has_scale = in->has_scale,
                                      .scale = in->scale};
    if (in->language) {
        p.style.has_language = true;
        p.style.language = de_ep_cstr(in->language);
    }
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, sizeof f->params);
    de_ep_write_ui_measure_text_params(&w, &p);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    return call(f, DE_EP_METHOD_UI_MEASURE_TEXT, size);
}
static f32 num(mpack_node_t n, const char *key) {
    return mpack_node_float(mpack_node_map_cstr(n, key));
}

TEST_F(editor_ui, text_metrics_match_de_ui_measure_text_and_cover_the_text, ui_ed_fx) {
    text_env t;
    text_env_up(f, &t);
    EXPECT_TRUE(has_cap(f, "ui.textMetrics"));
    const char *s = "The quick brown fox wraps over multiple lines";
    mpack_node_t r = res(measure(f, &(measure_in){.text = s,
                                                  .has_size = true,
                                                  .size = 20,
                                                  .has_wrap = true,
                                                  .wrap = 100}));
    de_text_style style = {.font = t.fonts.latin, .size_px = 20};
    de_ui_text_metrics m = {0};
    de_text_line lines[16] = {0};
    ASSERT_EQ(de_ui_measure_text(f->engine.ui,
                                 &(de_ui_text_measure_desc){
                                     .utf8 = {s, strlen(s)}, .style = &style, .wrap_width = 100},
                                 lines, 16, &m),
              DE_OK);
    ASSERT_TRUE(m.line_count > 1u && m.line_count <= 16u);
    mpack_node_t size = mpack_node_map_cstr(r, "size");
    ASSERT_EQ(mpack_node_array_length(size), 2u);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(size, 0)), m.size.x, 1e-3f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(size, 1)), m.size.y, 1e-3f);
    EXPECT_FLOAT_EQ(num(r, "scale"), 1, 1e-6f);
    EXPECT_FLOAT_EQ(num(r, "lineHeight"), m.line_height, 1e-3f);
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "overflowX")));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "overflowY")));
    mpack_node_t out = mpack_node_map_cstr(r, "lines");
    ASSERT_EQ(mpack_node_array_length(out), (size_t)m.line_count);
    for (u32 i = 0; i < m.line_count; ++i) {
        mpack_node_t l = mpack_node_array_at(out, i);
        EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(l, "start")), lines[i].byte_start);
        EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(l, "end")), lines[i].byte_end);
        EXPECT_FLOAT_EQ(num(l, "x"), lines[i].x, 1e-3f);
        EXPECT_FLOAT_EQ(num(l, "y"), lines[i].y, 1e-3f);
        EXPECT_FLOAT_EQ(num(l, "width"), lines[i].width, 1e-3f);
        EXPECT_FLOAT_EQ(num(l, "height"), lines[i].height, 1e-3f);
    }
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(mpack_node_array_at(out, 0), "start")), 0u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(mpack_node_array_at(out, m.line_count - 1), "end")),
              (u32)strlen(s));
    text_env_down(f, &t);
}

TEST_F(editor_ui, text_metrics_scale_overflow_and_the_default_size, ui_ed_fx) {
    text_env t;
    text_env_up(f, &t);
    const char *s = "Unbreakableword";
    mpack_node_t one = res(measure(f, &(measure_in){.text = s, .has_size = true, .size = 16}));
    const f32 w1 = mpack_node_float(mpack_node_array_at(mpack_node_map_cstr(one, "size"), 0));
    /* size_px absent is 16. */
    mpack_node_t dflt = res(measure(f, &(measure_in){.text = s}));
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(mpack_node_map_cstr(dflt, "size"), 0)), w1,
                    1e-3f);
    /* The size is multiplied by the scale, and the reply carries the scale used. */
    mpack_node_t two = res(measure(f, &(measure_in){.text = s, .has_scale = true, .scale = 2}));
    EXPECT_FLOAT_EQ(num(two, "scale"), 2, 1e-6f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(mpack_node_map_cstr(two, "size"), 0)),
                    2 * w1, 0.05f * w1);
    /* A word wider than wrapWidth cannot break narrow enough; a box shorter than the text. */
    mpack_node_t over = res(measure(
        f, &(measure_in){.text = s, .has_wrap = true, .wrap = 5, .has_box = true, .box = 1}));
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(over, "overflowX")));
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(over, "overflowY")));
    /* Limits that are absent or 0 mean none. */
    mpack_node_t none = res(measure(
        f, &(measure_in){.text = s, .has_wrap = true, .wrap = 0, .has_box = true, .box = 0}));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(none, "overflowX")));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(none, "overflowY")));
    text_env_down(f, &t);
}

TEST_F(editor_ui, text_metrics_line_height_is_in_layout_pixels, ui_ed_fx) {
    text_env t;
    text_env_up(f, &t);
    const char *s = "one\ntwo\nthree";
    mpack_node_t base = res(measure(f, &(measure_in){.text = s, .has_size = true, .size = 20}));
    const f32 own = num(base, "lineHeight");
    ASSERT_TRUE(own > 0);
    /* Layout pixels: 40 gives 40 per line, not a factor of 40. */
    mpack_node_t px = res(measure(f, &(measure_in){.text = s,
                                                   .has_size = true,
                                                   .size = 20,
                                                   .has_line_height = true,
                                                   .line_height = 40}));
    EXPECT_FLOAT_EQ(num(px, "lineHeight"), 40, 0.01f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(mpack_node_map_cstr(px, "size"), 1)), 120,
                    0.05f);
    /* Not multiplied by the scale. */
    mpack_node_t sc = res(measure(f, &(measure_in){.text = s,
                                                   .has_size = true,
                                                   .size = 20,
                                                   .has_line_height = true,
                                                   .line_height = 40,
                                                   .has_scale = true,
                                                   .scale = 2}));
    EXPECT_FLOAT_EQ(num(sc, "lineHeight"), 40, 0.01f);
    /* 0 is the font's own. */
    mpack_node_t zero = res(measure(f, &(measure_in){.text = s,
                                                     .has_size = true,
                                                     .size = 20,
                                                     .has_line_height = true,
                                                     .line_height = 0}));
    EXPECT_FLOAT_EQ(num(zero, "lineHeight"), own, 1e-3f);
    EXPECT_EQ(code_of(measure(f, &(measure_in){.text = s, .has_line_height = true, .line_height = -1})),
              DE_EP_ERR_INVALID_PARAMS);
    text_env_down(f, &t);
}

TEST_F(editor_ui, text_metrics_errors_and_empty_text, ui_ed_fx) {
    text_env t;
    text_env_up(f, &t);
    EXPECT_EQ(code_of(measure(f, &(measure_in){.text = "x", .font = "font/missing"})),
              DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(code_of(measure(f, &(measure_in){.text = "x", .font = ""})), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(code_of(measure(f, &(measure_in){.text = "x", .has_wrap = true, .wrap = -1})),
              DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(code_of(measure(f, &(measure_in){.text = "x", .has_box = true, .box = NAN})),
              DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(code_of(measure(f, &(measure_in){.text = "x", .has_scale = true, .scale = -2})),
              DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(code_of(measure(f, &(measure_in){.text = "x", .has_size = true, .size = INFINITY})),
              DE_EP_ERR_INVALID_PARAMS);
    /* Below protocol 1.17 the method is unsupported. */
    f->ed.proto_minor = 16;
    EXPECT_EQ(code_of(measure(f, &(measure_in){.text = "x"})), DE_EP_ERR_UNSUPPORTED);
    f->ed.proto_minor = 18;
    /* Empty text: the engine lays out one empty line, as a label of no text is one line tall
     * (the dide mock returns no lines and size [0, 0]; DENG-788 files that as a DIDE issue). */
    mpack_node_t r = res(measure(f, &(measure_in){.text = ""}));
    mpack_node_t lines = mpack_node_map_cstr(r, "lines");
    ASSERT_EQ(mpack_node_array_length(lines), 1u);
    mpack_node_t l0 = mpack_node_array_at(lines, 0);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(l0, "start")), 0u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(l0, "end")), 0u);
    EXPECT_FLOAT_EQ(num(l0, "width"), 0, 1e-6f);
    mpack_node_t sz = mpack_node_map_cstr(r, "size");
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(sz, 0)), 0, 1e-6f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(sz, 1)), num(r, "lineHeight"), 1e-3f);
    text_env_down(f, &t);
}

TEST_F(editor_ui, text_metrics_retries_with_a_larger_buffer, ui_ed_fx) {
    text_env t;
    text_env_up(f, &t);
    char text[2048];
    size_t n = 0;
    for (int i = 0; i < 300; ++i)
        n += (size_t)snprintf(text + n, sizeof text - n, "l%d\n", i % 10);
    mpack_node_t r = res(measure(f, &(measure_in){.text = text}));
    mpack_node_t lines = mpack_node_map_cstr(r, "lines");
    ASSERT_EQ(mpack_node_array_length(lines), 301u); /* a final empty line after the last break */
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(mpack_node_array_at(lines, 300), "end")), (u32)n);
    text_env_down(f, &t);
}

TEST_F(editor_ui, text_overflow_lists_overflowing_text_in_pre_order, ui_ed_fx) {
    f->ed.proto_minor = 21;
    text_env t;
    text_env_up(f, &t);
    const de_entity root = node(f, (de_entity){0});
    node_edit(f, root)->direction = DE_UI_COLUMN;
    const de_entity tight = sized(f, root, 40, 30);
    const de_entity roomy = sized(f, root, 390, 30);
    node_edit(f, tight)->shrink = 0;
    node_edit(f, roomy)->shrink = 0;
    const de_entity labels[2] = {tight, roomy};
    for (u32 i = 0; i < 2; ++i) {
        DeUiText *label = de_entity_add(f->engine.world, labels[i], f->engine.ui->ids.text);
        label->style.font = t.fonts.latin;
        label->style.size_px = 20;
        memcpy(label->text, "Overflowing", 12);
    }
    frame(f);
    de_ui_text_overflow want[4];
    u32 n = 0;
    ASSERT_EQ(de_ui_find_text_overflow(f->engine.ui, (de_entity){0}, want, 4, &n), DE_OK);
    ASSERT_EQ(n, 1u);

    mpack_node_t r = res(call_overflow(f, false, 0));
    mpack_node_t list = mpack_node_map_cstr(r, "overflows");
    ASSERT_EQ(mpack_node_array_length(list), 1u);
    mpack_node_t e = mpack_node_array_at(list, 0);
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(e, "entity")), tight.bits);
    EXPECT_EQ(mpack_node_bool(mpack_node_map_cstr(e, "overflowX")), want[0].overflow_x);
    EXPECT_EQ(mpack_node_bool(mpack_node_map_cstr(e, "overflowY")), want[0].overflow_y);
    EXPECT_TRUE(want[0].overflow_y);
    mpack_node_t ts = mpack_node_map_cstr(e, "textSize");
    mpack_node_t bs = mpack_node_map_cstr(e, "boxSize");
    ASSERT_EQ(mpack_node_array_length(ts), 2u);
    ASSERT_EQ(mpack_node_array_length(bs), 2u);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(ts, 0)), want[0].text_size.x, 0.001f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(ts, 1)), want[0].text_size.y, 0.001f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(bs, 0)), want[0].box_size.x, 0.001f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(bs, 1)), want[0].box_size.y, 0.001f);

    /* A root scopes the check; the fitting label has none, and an unknown root is not_found. */
    r = res(call_overflow(f, true, root.bits));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "overflows")), 1u);
    r = res(call_overflow(f, true, roomy.bits));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "overflows")), 0u);
    EXPECT_EQ(code_of(call_overflow(f, true, 0xffu)), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(code_of(call_overflow(f, true, 0)), DE_EP_ERR_NOT_FOUND);
    de_error_clear();
    text_env_down(f, &t);
}

/* ---- DENG-585: editor.locPseudo and editor.uiOverflow, reached through console.exec ---- */
typedef struct log_capture {
    char text[8192];
    size_t used;
} log_capture;
static void capture_sink(const de_log_record *rec, void *user) {
    log_capture *c = (log_capture *)user;
    const int k = snprintf(c->text + c->used, sizeof c->text - c->used, "%.*s\n",
                           (int)rec->message.len, rec->message.ptr);
    if (k > 0 && c->used + (size_t)k < sizeof c->text)
        c->used += (size_t)k;
}
static de_result exec_captured(log_capture *c, const char *line) {
    de_log_flush();
    c->used = 0;
    c->text[0] = '\0';
    const de_result r = de_command_exec(line);
    de_log_flush();
    return r;
}
TEST_F(editor_ui, console_commands_toggle_pseudo_and_list_overflow, ui_ed_fx) {
    text_env t;
    text_env_up(f, &t);
    static log_capture cap;
    de_log_add_sink(capture_sink, &cap);

    /* Pseudo-localization: requested at once, and turned off again by a disconnect. */
    ASSERT_EQ(exec_captured(&cap, "editor.locPseudo"), DE_OK);
    EXPECT_TRUE(strstr(cap.text, "pseudo=0") != NULL);
#if DE_NAME_LOOKUP
    de_loc_locale_state st;
    ASSERT_EQ(exec_captured(&cap, "editor.locPseudo on"), DE_OK);
    EXPECT_TRUE(strstr(cap.text, "pseudo=1") != NULL);
    ASSERT_EQ(de_loc_get_locale_state(f->engine.text, &st), DE_OK);
    EXPECT_TRUE(st.pseudo);
    for (u32 i = 0; i < f->ed.reset_count; ++i)
        f->ed.resets[i](&f->ed);
    ASSERT_EQ(de_loc_get_locale_state(f->engine.text, &st), DE_OK);
    EXPECT_FALSE(st.pseudo);
    ASSERT_EQ(exec_captured(&cap, "editor.locPseudo off"), DE_OK);
    EXPECT_TRUE(strstr(cap.text, "pseudo=0") != NULL);
#endif
    EXPECT_EQ(exec_captured(&cap, "editor.locPseudo maybe"), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(exec_captured(&cap, "editor.locPseudo on off"), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();

    /* Overflow: a 40 px label whose long word breaks into lines too tall for its box, and one that fits. */
    const de_entity root = node(f, (de_entity){0});
    node_edit(f, root)->direction = DE_UI_COLUMN;
    const de_entity tight = sized(f, root, 40, 30);
    const de_entity roomy = sized(f, root, 390, 30);
    node_edit(f, tight)->shrink = 0;
    node_edit(f, roomy)->shrink = 0;
    const de_entity labels[2] = {tight, roomy};
    for (u32 i = 0; i < 2; ++i) {
        DeUiText *label = de_entity_add(f->engine.world, labels[i], f->engine.ui->ids.text);
        label->style.font = t.fonts.latin;
        label->style.size_px = 20;
        memcpy(label->text, "Overflowing", 12);
    }
    frame(f);
    ASSERT_EQ(exec_captured(&cap, "editor.uiOverflow"), DE_OK);
    EXPECT_TRUE(strstr(cap.text, "count=1\n") != NULL);
    char want[64];
    snprintf(want, sizeof want, "entity=%016llx x=0 y=1", (unsigned long long)tight.bits);
    EXPECT_TRUE(strstr(cap.text, want) != NULL);
    char line[64];
    snprintf(line, sizeof line, "editor.uiOverflow %016llx", (unsigned long long)roomy.bits);
    ASSERT_EQ(exec_captured(&cap, line), DE_OK);
    EXPECT_TRUE(strstr(cap.text, "count=0\n") != NULL);
    EXPECT_EQ(exec_captured(&cap, "editor.uiOverflow zz"), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(exec_captured(&cap, "editor.uiOverflow 00000000000000ff"), DE_ERR_NOT_FOUND);
    de_error_clear();

    /* Unregistered on shutdown. */
    de__ed_ui_shutdown(&f->ed);
    EXPECT_EQ(de_command_exec("editor.uiOverflow"), DE_ERR_NOT_FOUND);
    de_error_clear();
    text_env_down(f, &t);
}
#endif
#endif
#endif
