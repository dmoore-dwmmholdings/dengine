/* DENG-581: foliage.info, foliage.instances, foliage.type and the foliage.* edit ops (paint,
 * erase, scatter, restore) of the DIDE world.foliage capability (protocol 1.22), through the real
 * handlers, the edit transactions and the generated protocol readers and writers. The cases are
 * the DIDE mock engine's (mock-engine/src/foliage.rs). The two foliage types are the mock's
 * fixtures, in data/foliage. DENG-823: the instances live on DeFoliagePage entities; the tests read
 * them back through de__ed_foliage_load, and DIDE-R167 AC1 (a 100 m brush at 4/m^2) is stored,
 * undone and redone. */
#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include "world/foliage/foliage.h"
#include <dengine/de_asset.h>
#include <dengine/de_ecs.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include <dengine/de_task.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORAGE  (8u << 20) /* the editor arena (ED_ARENA_CAP) */
#define RESPONSE (512u << 10)

void de__ed_session_shutdown(void);

typedef struct foliage_fx {
    de_engine engine;
    de__host host;
    de__editor ed;
    de_task_system *tasks;
    unsigned char *storage;
    char *response;
    char *note;
    char params[16384];
    mpack_tree_t tree;
    bool parsed;
    de_component comp, terrain_comp;
    de_entity layer;   /**< the DeFoliage layer; palette 0 grass, 1 pine */
    de_entity ground;  /**< a plane surface at y = 2 */
    de_entity terrain; /**< the default (flat, 32 m) DeTerrain at the origin */
} foliage_fx;

static de_entity spawn_at(foliage_fx *f, f64 x, f64 y, f64 z) {
    const de_entity e = de_entity_spawn(f->engine.world);
    DeTransform *t = (DeTransform *)de_entity_add(
        f->engine.world, e, de_builtin_components(f->engine.world).transform);
    ASSERT_TRUE(t != NULL);
    *t = de_transform_identity();
    t->position = (de_dvec3){x, y, z};
    return e;
}

static void foliage_fx_setup(foliage_fx *f) {
    memset(f, 0, sizeof *f);
    ASSERT_EQ(de_log_init(&(de_log_desc){.no_console_sink = true}), DE_OK);
    ASSERT_EQ(de_type_registry_create(NULL, &f->engine.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->engine.types}, &f->engine.world), DE_OK);
    (void)de_builtin_components(f->engine.world);
    f->storage = (unsigned char *)malloc(STORAGE);
    f->response = (char *)malloc(RESPONSE);
    f->note = (char *)malloc(256u << 10);
    ASSERT_TRUE(f->storage != NULL && f->response != NULL && f->note != NULL);
    f->ed.e = &f->engine;
    f->ed.h = &f->host;
    f->ed.mode = DE_EP_WORLD_MODE_EDIT;
    f->ed.arena = f->storage;
    f->ed.arena_cap = STORAGE;
    f->ed.note = f->note;
    f->ed.note_cap = 256u << 10;
    f->ed.proto_major = 1;
    f->ed.proto_minor = 22;
    ASSERT_EQ(de_fs_mount("fol581", DE_EDITOR_FOLIAGE_DATA_DIR, 0, 0), DE_OK);
    de_fp_env_init_thread();
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 1}, &f->tasks), DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.tasks = f->tasks,
                                                             .types = f->engine.types,
                                                             .io_threads = 1,
                                                             .cache_dir = "nocache581:/cache"},
                                     &f->engine.assets),
              DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->engine.assets, "fol581:/assets.toml"), DE_OK);
    de__ed_edit_register(&f->ed);
    de__ed_foliage_register(&f->ed);
    f->comp = de_component_find(f->engine.world, DE_FOLIAGE_COMPONENT);
    f->terrain_comp = de_component_find(f->engine.world, "DeTerrain");
    ASSERT_TRUE(DE_HANDLE_VALID(f->comp) && DE_HANDLE_VALID(f->terrain_comp));
    f->layer = spawn_at(f, 0, 0, 0);
    DeFoliage *fo = (DeFoliage *)de_entity_add(f->engine.world, f->layer, f->comp);
    ASSERT_TRUE(fo != NULL);
    fo->types[0].id = de__ed_asset_handle(&f->ed, "foliage/grass", NULL);
    fo->types[1].id = de__ed_asset_handle(&f->ed, "foliage/pine", NULL);
    ASSERT_TRUE(fo->types[0].id != 0 && fo->types[1].id != 0);
    f->ground = spawn_at(f, 0, 2, 0);
    f->terrain = spawn_at(f, 0, 0, 0);
    ASSERT_TRUE(de_entity_add(f->engine.world, f->terrain, f->terrain_comp) != NULL);
}
static void foliage_fx_teardown(foliage_fx *f) {
    if (f->parsed)
        mpack_tree_destroy(&f->tree);
    for (u32 i = 0; i < f->ed.reset_count; ++i)
        f->ed.resets[i](&f->ed);
    de__ed_session_shutdown();
    de_world_destroy(f->engine.world);
    if (f->engine.assets)
        de_asset_system_destroy(f->engine.assets);
    if (f->tasks)
        de_task_system_destroy(f->tasks);
    (void)de_fs_unmount("fol581", DE_EDITOR_FOLIAGE_DATA_DIR);
    de_type_registry_destroy(f->engine.types);
    free(f->storage);
    free(f->response);
    free(f->note);
    de_log_shutdown();
}

static DeFoliage *layer(foliage_fx *f) {
    DeFoliage *fo = (DeFoliage *)de_entity_get(f->engine.world, f->layer, f->comp);
    ASSERT_TRUE(fo != NULL);
    return fo;
}
/* The layer's instances, loaded from its pages (the store must be freed). */
static void store_of(foliage_fx *f, de_foliage_store *st) {
    de__foliage_store_init(st, de_heap_allocator());
    ASSERT_TRUE(de__ed_foliage_load(&f->ed, f->layer, st));
}
static u32 count_of(foliage_fx *f) {
    de_foliage_store st;
    store_of(f, &st);
    const u32 n = st.instance_count;
    de__foliage_store_free(&st);
    return n;
}
static u32 cells_of(foliage_fx *f) {
    de_foliage_store st;
    store_of(f, &st);
    const u32 n = st.cell_count;
    de__foliage_store_free(&st);
    return n;
}
/* The DeFoliagePage entities of the layer. */
typedef struct page_set {
    de_entity e[4096];
    u32 n;
    u64 layer;
} page_set;
static void page_chunk(const de_query_chunk *c, void *user) {
    page_set *ps = (page_set *)user;
    const DeFoliagePage *p = (const DeFoliagePage *)c->columns[0];
    for (u32 r = 0; r < c->count; ++r)
        if (p[r].layer.bits == ps->layer && ps->n < 4096u)
            ps->e[ps->n++] = c->entities[r];
}
static de_component page_comp(foliage_fx *f) {
    const de_component pc = de_component_find(f->engine.world, "DeFoliagePage");
    ASSERT_TRUE(DE_HANDLE_VALID(pc));
    return pc;
}
static void pages_of(foliage_fx *f, page_set *ps) {
    ps->n = 0;
    ps->layer = f->layer.bits;
    const de_query_term terms[1] = {{page_comp(f), DE_READ}};
    const de_query_desc qd = {terms, 1, NULL, 0};
    const de_query q = de_query_create(f->engine.world, &qd);
    de_query_each(f->engine.world, q, page_chunk, ps);
    de_query_destroy(f->engine.world, q);
}
/* A copy of the layer's instances, in cell order. */
#define INST_MAX 65536u
typedef struct inst_list {
    u32 n;
    de_foliage_instance i[INST_MAX];
} inst_list;
static void instances_of(foliage_fx *f, inst_list *out) {
    de_foliage_store st;
    store_of(f, &st);
    ASSERT_TRUE(st.instance_count <= INST_MAX);
    out->n = 0;
    for (u32 c = 0; c < st.cell_count; ++c) {
        memcpy(&out->i[out->n], st.cells[c].inst, sizeof(de_foliage_instance) * st.cells[c].count);
        out->n += st.cells[c].count;
    }
    de__foliage_store_free(&st);
}
static bool same(const inst_list *a, const inst_list *b) {
    return a->n == b->n && memcmp(a->i, b->i, sizeof(de_foliage_instance) * a->n) == 0;
}
static u32 count_where(const inst_list *l, int type, int rule) {
    u32 n = 0;
    for (u32 k = 0; k < l->n; ++k)
        n += (type < 0 || l->i[k].type == (u8)type) && (rule < 0 || l->i[k].rule == (u32)rule);
    return n;
}

/* Calls `m` with the `size` bytes in f->params and returns the parsed reply. */
static mpack_node_t call(foliage_fx *f, de_ep_method m, size_t size) {
    const de_ep_request req = {
        .id = 581, .method = de_ep_cstr(de_ep_method_name(m)), .params = {f->params, size}};
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
    ASSERT_EQ(mpack_node_u64(mpack_node_map_cstr(root, "id")), 581u);
    return root;
}
#define CALL(f, m, writer, p)                                                                      \
    do {                                                                                           \
        mpack_writer_t w_;                                                                         \
        mpack_writer_init(&w_, (f)->params, sizeof(f)->params);                                    \
        writer(&w_, p);                                                                            \
        const size_t n_ = mpack_writer_buffer_used(&w_);                                           \
        ASSERT_EQ(mpack_writer_destroy(&w_), mpack_ok);                                            \
        root = call(f, m, n_);                                                                     \
    } while (0)

/* The error code, or 0 for a result. */
static int code_of(mpack_node_t root) {
    if (!mpack_node_map_contains_cstr(root, "error")) {
        EXPECT_TRUE(mpack_node_map_contains_cstr(root, "result"));
        return 0;
    }
    return (int)mpack_node_i32(mpack_node_map_cstr(mpack_node_map_cstr(root, "error"), "code"));
}
static mpack_node_t result(mpack_node_t root) {
    ASSERT_EQ(code_of(root), 0);
    return mpack_node_map_cstr(root, "result");
}
static bool str_is(mpack_node_t n, const char *s) {
    return mpack_node_type(n) == mpack_type_str && mpack_node_strlen(n) == strlen(s) &&
           memcmp(mpack_node_str(n), s, strlen(s)) == 0;
}

static mpack_node_t info(foliage_fx *f) {
    mpack_node_t root;
    CALL(f, DE_EP_METHOD_FOLIAGE_INFO, de_ep_write_foliage_params,
         &(de_ep_foliage_params){f->layer.bits});
    return root;
}
static mpack_node_t cell_instances(foliage_fx *f, const i32 *cell, u32 n, int type) {
    mpack_node_t root;
    de_ep_foliage_cell_params q = {.entity = f->layer.bits, .cell = {(int32_t *)cell, n}};
    if (type >= 0) {
        q.has_type = true;
        q.type = (u32)type;
    }
    CALL(f, DE_EP_METHOD_FOLIAGE_INSTANCES, de_ep_write_foliage_cell_params, &q);
    return root;
}
static mpack_node_t type_of(foliage_fx *f, const char *id) {
    mpack_node_t root;
    CALL(f, DE_EP_METHOD_FOLIAGE_TYPE, de_ep_write_foliage_type_params,
         (&(de_ep_foliage_type_params){.id = de_ep_cstr(id)}));
    return root;
}

/* ---- Edit transactions ---- */

static u32 begin(foliage_fx *f) {
    mpack_node_t root;
    CALL(f, DE_EP_METHOD_EDIT_BEGIN, de_ep_write_edit_begin_params,
         (&(de_ep_edit_begin_params){.label = de_ep_cstr("foliage"), .scene = 1}));
    return mpack_node_u32(mpack_node_map_cstr(result(root), "tx"));
}
static int tx_call(foliage_fx *f, de_ep_method m, u32 tx) {
    mpack_node_t root;
    CALL(f, m, de_ep_write_tx_params, (&(de_ep_tx_params){.tx = tx}));
    return code_of(root);
}
static int apply(foliage_fx *f, u32 tx, const de_ep_edit_op *op) {
    mpack_node_t root;
    CALL(f, DE_EP_METHOD_EDIT_APPLY, de_ep_write_edit_apply_params,
         (&(de_ep_edit_apply_params){.tx = tx, .ops = {(de_ep_edit_op *)op, 1}}));
    return code_of(root);
}
/* One op in its own transaction: the apply code; committed when it is 0, cancelled otherwise. */
static int edit(foliage_fx *f, const de_ep_edit_op *op, u32 *tx_out) {
    const u32 tx = begin(f);
    const int code = apply(f, tx, op);
    EXPECT_EQ(tx_call(f, code == 0 ? DE_EP_METHOD_EDIT_COMMIT : DE_EP_METHOD_EDIT_CANCEL, tx), 0);
    if (tx_out != NULL)
        *tx_out = tx;
    return code;
}

static char g_value[8192];
static f32 g_dab[4];
static f32 g_falloff[2] = {1.0f, 1.0f};
static u64 g_surface[1];

/* The mock's stroke: one dab at (x, z), radius 4, flat falloff, density 2, seed 3, on the ground
 * plane. `type` < 0 leaves the type out. */
static de_ep_foliage_stroke stroke(foliage_fx *f, int type, f32 x, f32 z, f32 strength) {
    g_dab[0] = x;
    g_dab[1] = 0.0f;
    g_dab[2] = z;
    g_dab[3] = 1.0f;
    g_surface[0] = f->ground.bits;
    de_ep_foliage_stroke s;
    memset(&s, 0, sizeof s);
    s.has_type = type >= 0;
    s.type = type >= 0 ? (u32)type : 0;
    s.points = (de_ep_list_f32){g_dab, 4};
    s.radius = 4.0f;
    s.strength = strength;
    s.falloff = (de_ep_list_f32){g_falloff, 2};
    s.has_density = true;
    s.density = 2.0f;
    s.has_seed = true;
    s.seed = 3;
    s.has_surfaces = true;
    s.surfaces = (de_ep_list_u64){(uint64_t *)g_surface, 1};
    return s;
}
static de_ep_edit_op stroke_op(foliage_fx *f, const char *kind, const de_ep_foliage_stroke *s) {
    mpack_writer_t w;
    mpack_writer_init(&w, g_value, sizeof g_value);
    de_ep_write_foliage_stroke(&w, s);
    const size_t n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    return (de_ep_edit_op){.op = de_ep_cstr(kind),
                           .has_entity = true,
                           .entity = f->layer.bits,
                           .has_value = true,
                           .value = {g_value, n}};
}
static int paint(foliage_fx *f, const de_ep_foliage_stroke *s) {
    const de_ep_edit_op op = stroke_op(f, "foliage.paint", s);
    return edit(f, &op, NULL);
}
static de_ep_edit_op scatter_op(foliage_fx *f, const u32 *rules, u32 n, bool all, bool clear) {
    de_ep_foliage_scatter sc;
    memset(&sc, 0, sizeof sc);
    sc.has_rules = !all;
    sc.rules = (de_ep_list_u32){(uint32_t *)rules, n};
    sc.has_clear = clear;
    sc.clear = clear;
    mpack_writer_t w;
    mpack_writer_init(&w, g_value, sizeof g_value);
    de_ep_write_foliage_scatter(&w, &sc);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    return (de_ep_edit_op){.op = de_ep_cstr("foliage.scatter"),
                           .has_entity = true,
                           .entity = f->layer.bits,
                           .has_value = true,
                           .value = {g_value, size}};
}
/* The mock's rule, over a 12 m square, to keep the cases small. */
static void set_rule(foliage_fx *f, u32 slot, u32 id, de_entity terrain) {
    de_foliage_rule *r = &layer(f)->rules[slot];
    r->id = id;
    r->enabled = true;
    r->type = 0;
    r->terrain = terrain;
    r->density = 0.5f;
    r->seed = 11;
    r->area_min = (de_vec2){0.0f, 0.0f};
    r->area_max = (de_vec2){12.0f, 12.0f};
}
/* Runs `op` through the edit hook directly, for its inverse. */
static bool op_direct(foliage_fx *f, const de_ep_edit_op *op, de_ep_edit_op *inv) {
    de_ep_arena a = {f->storage, STORAGE, 0};
    de_ep_error code = 0;
    char msg[256];
    return f->ed.foliage_op(&f->ed, &a, op, inv, &code, msg, sizeof msg);
}

/* ---- Queries ---- */

TEST_F(editor_foliage, capability_and_info_shape, foliage_fx) {
    bool offered = false;
    for (u32 i = 0; i < f->ed.cap_count; ++i)
        offered = offered || strcmp(f->ed.caps[i], "world.foliage") == 0;
    EXPECT_TRUE(offered);
    mpack_node_t r = result(info(f));
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(r, "entity")), f->layer.bits);
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "component"), "DeFoliage"));
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_map_cstr(r, "cellSize")), 16.0f, 1e-6f);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "maxTypes")), 8u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "maxRules")), 8u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "maxPerCell")), DE_FOLIAGE_CELL_INSTANCES);
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "typeSuffix"), ".foliage.toml"));
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(r, "count")), 0u);
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "cells")), 0u);
    /* An entity without the component, and a dead one. */
    mpack_node_t root;
    CALL(f, DE_EP_METHOD_FOLIAGE_INFO, de_ep_write_foliage_params,
         &(de_ep_foliage_params){f->ground.bits});
    EXPECT_EQ(code_of(root), DE_EP_ERR_NOT_FOUND);
    CALL(f, DE_EP_METHOD_FOLIAGE_INFO, de_ep_write_foliage_params,
         &(de_ep_foliage_params){0x7777});
    EXPECT_EQ(code_of(root), DE_EP_ERR_NOT_FOUND);
}

TEST_F(editor_foliage, type_reports_the_asset_or_an_error, foliage_fx) {
    mpack_node_t r = result(type_of(f, "foliage/pine"));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "id"), "foliage/pine"));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "mesh"), "mesh:pine"));
    EXPECT_TRUE(str_is(mpack_node_map_cstr(r, "impostor"), "tex:pine_impostor"));
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_map_cstr(r, "impostorDistance")), 80.0f, 1e-6f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_map_cstr(r, "density")), 0.05f, 1e-7f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(mpack_node_map_cstr(r, "scale"), 1)),
                    1.4f, 1e-6f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_map_cstr(r, "minSpacing")), 3.0f, 1e-6f);
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(r, "collision")));
    EXPECT_FALSE(mpack_node_bool(mpack_node_map_cstr(r, "alignToNormal")));
    r = result(type_of(f, "foliage/grass"));
    EXPECT_FALSE(mpack_node_map_contains_cstr(r, "impostor"));
    EXPECT_TRUE(mpack_node_bool(mpack_node_map_cstr(r, "alignToNormal")));
    EXPECT_EQ(code_of(type_of(f, "foliage/none")), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(code_of(type_of(f, "foliage/bad")), DE_EP_ERR_INVALID_PARAMS);
}

/* ---- Paint ---- */

TEST_F(editor_foliage, paint_places_on_the_surface_and_undo_redo_restore_it, foliage_fx) {
    const de_ep_foliage_stroke s = stroke(f, 0, 4.0f, 4.0f, 1.0f);
    const de_ep_edit_op op = stroke_op(f, "foliage.paint", &s);
    u32 tx = 0;
    ASSERT_EQ(edit(f, &op, &tx), 0);
    static inst_list after, now;
    instances_of(f, &after);
    EXPECT_TRUE(after.n > 20u);
    for (u32 k = 0; k < after.n; ++k) {
        EXPECT_FLOAT_EQ(after.i[k].y, 2.0f, 1e-6f);
        EXPECT_EQ(after.i[k].type, 0u);
        EXPECT_EQ(after.i[k].rule, 0u);
        const f32 dx = after.i[k].x - 4.0f, dz = after.i[k].z - 4.0f;
        EXPECT_TRUE(dx * dx + dz * dz <= 16.0f + 1e-4f);
        EXPECT_TRUE(after.i[k].scale >= 0.8f && after.i[k].scale <= 1.2f);
    }
    /* info: one cell, (0, 0), with every instance and the entity's revision. */
    mpack_node_t r = result(info(f));
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(r, "count")), after.n);
    const mpack_node_t cells = mpack_node_map_cstr(r, "cells");
    ASSERT_EQ(mpack_node_array_length(cells), 1u);
    const mpack_node_t c0 = mpack_node_array_at(cells, 0);
    EXPECT_EQ(mpack_node_i32(mpack_node_array_at(mpack_node_map_cstr(c0, "cell"), 0)), 0);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(c0, "count")), after.n);
    const u64 rev = mpack_node_u64(mpack_node_map_cstr(c0, "revision"));
    EXPECT_TRUE(rev > 0u);
    EXPECT_EQ(rev, mpack_node_u64(mpack_node_map_cstr(r, "revision")));

    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
    EXPECT_EQ(count_of(f), 0u);
    EXPECT_EQ(cells_of(f), 0u);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_REDO, tx), 0);
    instances_of(f, &now);
    EXPECT_TRUE(same(&now, &after));
}

TEST_F(editor_foliage, painting_again_keeps_spacing_and_the_same_seed_is_deterministic,
       foliage_fx) {
    static inst_list a, b, all;
    de_ep_foliage_stroke s = stroke(f, 0, 4.0f, 4.0f, 1.0f);
    ASSERT_EQ(paint(f, &s), 0);
    instances_of(f, &a);
    /* The same stroke on an empty layer gives the same bytes. */
    static page_set ps;
    pages_of(f, &ps);
    for (u32 i = 0; i < ps.n; ++i)
        de_entity_despawn(f->engine.world, ps.e[i]);
    *layer(f) = (DeFoliage){.types = {layer(f)->types[0], layer(f)->types[1]}};
    for (u32 i = 0; i < DE_FOLIAGE_RULES; ++i)
        layer(f)->rules[i].layer = -1;
    ASSERT_EQ(count_of(f), 0u);
    ASSERT_EQ(paint(f, &s), 0);
    instances_of(f, &b);
    EXPECT_TRUE(same(&a, &b));
    s.seed = 4;
    ASSERT_EQ(paint(f, &s), 0);
    instances_of(f, &all);
    EXPECT_TRUE(all.n > a.n && all.n < a.n * 2);
    for (u32 i = 0; i < all.n; ++i)
        for (u32 j = i + 1; j < all.n; ++j) {
            const f32 dx = all.i[i].x - all.i[j].x, dz = all.i[i].z - all.i[j].z;
            EXPECT_TRUE(sqrtf(dx * dx + dz * dz) >= 0.2f);
        }
}

TEST_F(editor_foliage, erase_at_full_strength_clears_the_brush_and_keeps_other_types,
       foliage_fx) {
    de_ep_foliage_stroke s = stroke(f, 0, 4.0f, 4.0f, 1.0f);
    ASSERT_EQ(paint(f, &s), 0);
    s = stroke(f, 1, 4.0f, 4.0f, 1.0f);
    s.density = 0.5f;
    ASSERT_EQ(paint(f, &s), 0);
    static inst_list l;
    instances_of(f, &l);
    const u32 pines = count_where(&l, 1, -1);
    EXPECT_TRUE(pines > 0u);
    s = stroke(f, 0, 4.0f, 4.0f, 1.0f);
    s.radius = 10.0f;
    const de_ep_edit_op op = stroke_op(f, "foliage.erase", &s);
    u32 tx = 0;
    ASSERT_EQ(edit(f, &op, &tx), 0);
    instances_of(f, &l);
    EXPECT_EQ(count_where(&l, 0, -1), 0u);
    EXPECT_EQ(l.n, pines);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
    instances_of(f, &l);
    EXPECT_TRUE(count_where(&l, 0, -1) > 0u);
    /* Erase with no type clears every type under the brush. */
    s.has_type = false;
    const de_ep_edit_op all = stroke_op(f, "foliage.erase", &s);
    ASSERT_EQ(edit(f, &all, NULL), 0);
    EXPECT_EQ(count_of(f), 0u);
}

/* ---- Scatter ---- */

TEST_F(editor_foliage, scatter_reruns_deterministically_keeps_painted_and_clears, foliage_fx) {
    set_rule(f, 0, 1, f->terrain);
    const de_ep_foliage_stroke s = stroke(f, 1, 40.0f, 40.0f, 1.0f);
    ASSERT_EQ(paint(f, &s), 0);
    const u32 painted = count_of(f);
    EXPECT_TRUE(painted > 0u);
    de_ep_edit_op op = scatter_op(f, NULL, 0, true, false);
    u32 tx = 0;
    ASSERT_EQ(edit(f, &op, &tx), 0);
    static inst_list first, now;
    instances_of(f, &first);
    const u32 scattered = count_where(&first, -1, 1);
    EXPECT_TRUE(scattered > 20u);
    for (u32 k = 0; k < first.n; ++k)
        if (first.i[k].rule == 1) {
            EXPECT_TRUE(first.i[k].x >= 0.0f && first.i[k].x <= 12.0f);
            EXPECT_TRUE(first.i[k].z >= 0.0f && first.i[k].z <= 12.0f);
            EXPECT_FLOAT_EQ(first.i[k].y, 0.0f, 1e-6f); /* the flat default terrain */
        }
    /* A re-run of rule 1 gives the same placement, so its inverse restores no cell. */
    static const u32 one[] = {1};
    op = scatter_op(f, one, 1, false, false);
    de_ep_edit_op inv;
    ASSERT_TRUE(op_direct(f, &op, &inv));
    instances_of(f, &now);
    EXPECT_TRUE(same(&now, &first));
    mpack_tree_t t;
    mpack_tree_init_data(&t, inv.value.ptr, inv.value.len);
    mpack_tree_parse(&t);
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(mpack_tree_root(&t), "cells")), 0u);
    EXPECT_EQ(mpack_tree_destroy(&t), mpack_ok);
    /* Clear removes the rule's instances and keeps the painted ones; undo brings them back. */
    op = scatter_op(f, NULL, 0, true, true);
    ASSERT_EQ(edit(f, &op, &tx), 0);
    instances_of(f, &now);
    EXPECT_EQ(now.n, painted);
    EXPECT_EQ(count_where(&now, -1, 0), painted);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
    instances_of(f, &now);
    EXPECT_EQ(count_where(&now, -1, 1), scattered);
    EXPECT_EQ(now.n, first.n);
}

/* ---- Errors ---- */

TEST_F(editor_foliage, bad_ops_are_refused, foliage_fx) {
    set_rule(f, 0, 1, f->terrain);
    static const u32 four[] = {4};
    de_ep_edit_op op = scatter_op(f, four, 1, false, false);
    EXPECT_EQ(edit(f, &op, NULL), DE_EP_ERR_INVALID_PARAMS);
    set_rule(f, 0, 2, (de_entity){0x7777});
    op = scatter_op(f, NULL, 0, true, false);
    EXPECT_EQ(edit(f, &op, NULL), DE_EP_ERR_NOT_FOUND);
    set_rule(f, 0, 2, f->ground); /* not a terrain */
    op = scatter_op(f, NULL, 0, true, false);
    EXPECT_EQ(edit(f, &op, NULL), DE_EP_ERR_INVALID_PARAMS);
    memset(&layer(f)->rules[0], 0, sizeof layer(f)->rules[0]);

    de_ep_foliage_stroke s = stroke(f, -1, 0.0f, 0.0f, 1.0f); /* no type */
    EXPECT_EQ(paint(f, &s), DE_EP_ERR_INVALID_PARAMS);
    s = stroke(f, 5, 0.0f, 0.0f, 1.0f); /* an empty palette slot */
    EXPECT_EQ(paint(f, &s), DE_EP_ERR_INVALID_PARAMS);
    s = stroke(f, 0, 0.0f, 0.0f, 1.0f);
    s.radius = 0.0f;
    EXPECT_EQ(paint(f, &s), DE_EP_ERR_INVALID_PARAMS);
    s = stroke(f, 0, 0.0f, 0.0f, 1.0f);
    s.has_surfaces = false;
    EXPECT_EQ(paint(f, &s), DE_EP_ERR_INVALID_PARAMS);
    s = stroke(f, 0, 0.0f, 0.0f, 1.0f);
    g_surface[0] = 0x7777; /* a dead surface */
    EXPECT_EQ(paint(f, &s), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(count_of(f), 0u);

    /* An entity without DeFoliage, a missing value, bad restore data and an unknown op. */
    s = stroke(f, 0, 0.0f, 0.0f, 1.0f);
    op = stroke_op(f, "foliage.erase", &s);
    op.entity = f->ground.bits;
    EXPECT_EQ(edit(f, &op, NULL), DE_EP_ERR_NOT_FOUND);
    op = (de_ep_edit_op){
        .op = de_ep_cstr("foliage.restore"), .has_entity = true, .entity = f->layer.bits};
    EXPECT_EQ(edit(f, &op, NULL), DE_EP_ERR_INVALID_PARAMS);
    static const char junk[] = {(char)0x81, (char)0xa5, 'c', 'e', 'l', 'l', 's', 0x01};
    op.has_value = true;
    op.value = (de_ep_raw){junk, sizeof junk};
    EXPECT_EQ(edit(f, &op, NULL), DE_EP_ERR_INVALID_PARAMS);
    op.op = de_ep_cstr("foliage.grow");
    EXPECT_EQ(edit(f, &op, NULL), DE_EP_ERR_INVALID_PARAMS);
}

/* ---- foliage.instances ---- */

TEST_F(editor_foliage, instances_report_ten_floats_each_and_filter_by_type, foliage_fx) {
    const de_ep_foliage_stroke s = stroke(f, 0, 4.0f, 4.0f, 1.0f);
    ASSERT_EQ(paint(f, &s), 0);
    static const i32 c00[2] = {0, 0}, c55[2] = {5, 5};
    mpack_node_t r = result(cell_instances(f, c00, 2, -1));
    const size_t n = mpack_node_array_length(mpack_node_map_cstr(r, "types"));
    EXPECT_EQ(n, count_of(f));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "rules")), n);
    const mpack_node_t xf = mpack_node_map_cstr(r, "transforms");
    ASSERT_EQ(mpack_node_array_length(xf), n * 10);
    de_foliage_store st;
    store_of(f, &st);
    ASSERT_EQ(st.cell_count, 1u);
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(r, "revision")), st.cells[0].revision);
    de__foliage_store_free(&st);
    /* Position, a unit quaternion about +y, uniform scale. */
    f32 norm = 0;
    for (u32 k = 3; k < 7; ++k) {
        const f32 q = mpack_node_float(mpack_node_array_at(xf, k));
        norm += q * q;
    }
    EXPECT_FLOAT_EQ(norm, 1.0f, 1e-4f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(xf, 3)), 0.0f, 1e-7f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(xf, 1)), 2.0f, 1e-7f);
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_array_at(xf, 7)),
              mpack_node_float(mpack_node_array_at(xf, 9)), 0.0f);
    r = result(cell_instances(f, c00, 2, 1));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "types")), 0u);
    r = result(cell_instances(f, c55, 2, -1));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "types")), 0u);
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "transforms")), 0u);
    EXPECT_EQ(code_of(cell_instances(f, c00, 1, -1)), DE_EP_ERR_INVALID_PARAMS);
}

/* DENG-822: alignToNormal. On a ramp rising 0.25 m per metre along +x, grass (alignToNormal) takes
 * the terrain normal, pine stays upright, the reported quaternion turns +y onto the normal, and an
 * undo/redo (restore data without normals) gives the normals back. */
static void ramp(foliage_fx *f) {
    DeTerrain *t = (DeTerrain *)de_entity_get(f->engine.world, f->terrain, f->terrain_comp);
    ASSERT_TRUE(t != NULL);
    t->height = 8.0f;
    for (u32 z = 0; z < DE_TERRAIN_RES; ++z)
        for (u32 x = 0; x < DE_TERRAIN_RES; ++x)
            t->heights[z * DE_TERRAIN_RES + x] =
                (u16)((f64)x / (f64)(DE_TERRAIN_RES - 1) * 65535.0 + 0.5);
}

TEST_F(editor_foliage, align_to_normal_tilts_on_the_terrain_and_survives_undo, foliage_fx) {
    ramp(f);
    de_ep_foliage_stroke s = stroke(f, 0, 4.0f, 4.0f, 1.0f);
    g_surface[0] = f->terrain.bits;
    de_ep_edit_op op = stroke_op(f, "foliage.paint", &s);
    u32 tx = 0;
    ASSERT_EQ(edit(f, &op, &tx), 0);
    static inst_list after, now;
    instances_of(f, &after);
    ASSERT_TRUE(after.n > 10u);
    /* n = (-0.25, 1, 0) / |..|: nx = round(-0.2425 * 127) = -31. */
    for (u32 k = 0; k < after.n; ++k) {
        EXPECT_TRUE(after.i[k].nx >= -32 && after.i[k].nx <= -30);
        EXPECT_EQ(after.i[k].nz, 0);
    }
    /* The reported quaternion turns +y onto the normal. */
    f32 xf[10];
    de__foliage_transform(&after.i[0], xf);
    const de_vec3 up = de_quat_rotate((de_quat){xf[3], xf[4], xf[5], xf[6]}, (de_vec3){0, 1, 0});
    EXPECT_FLOAT_EQ(up.x, -0.2425f, 0.01f);
    EXPECT_FLOAT_EQ(up.y, 0.9701f, 0.01f);
    EXPECT_FLOAT_EQ(up.z, 0.0f, 0.01f);
    /* Undo, then redo through the restore data (no normals): the normals come back. */
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
    EXPECT_EQ(count_of(f), 0u);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_REDO, tx), 0);
    instances_of(f, &now);
    ASSERT_TRUE(same(&now, &after));
    for (u32 k = 0; k < now.n; ++k) {
        EXPECT_EQ(now.i[k].nx, after.i[k].nx);
        EXPECT_EQ(now.i[k].nz, after.i[k].nz);
    }
    /* Pine does not align: upright on the same ramp. */
    s = stroke(f, 1, 20.0f, 4.0f, 1.0f);
    g_surface[0] = f->terrain.bits;
    ASSERT_EQ(paint(f, &s), 0);
    instances_of(f, &now);
    u32 pines = 0;
    for (u32 k = 0; k < now.n; ++k)
        if (now.i[k].type == 1) {
            pines++;
            EXPECT_EQ(now.i[k].nx, 0);
            EXPECT_EQ(now.i[k].nz, 0);
        }
    EXPECT_TRUE(pines > 0u);
}

/* Persistence: the palette (DeFoliage) and every page (DeFoliagePage, hidden but serialized) go
 * through the scene text format and back unchanged. */
static void text_round_trip(foliage_fx *f, const char *name, const void *src, void *dst) {
    const de_type_id t = de_type_find(f->engine.types, name);
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    const de_ser_options opt = {DE_SER_TEXT, DE_FIELD_SERIALIZE, NULL};
    de_mem_writer mw;
    de_mem_writer_init(&mw, NULL);
    de_writer w = de_writer_from_mem(&mw);
    ASSERT_EQ(de_serialize_object(f->engine.types, t, src, &opt, &w), DE_OK);
    de_mem_reader mr;
    de_mem_reader_init(&mr, mw.data, mw.size);
    de_reader rd = de_reader_from_mem(&mr);
    ASSERT_EQ(de_deserialize_object(f->engine.types, t, dst, &opt, &rd), DE_OK);
    de_mem_writer_free(&mw);
}

TEST_F(editor_foliage, the_layer_round_trips_through_the_text_format, foliage_fx) {
    de_ep_foliage_stroke s = stroke(f, 0, 4.0f, 4.0f, 1.0f);
    ASSERT_EQ(paint(f, &s), 0);
    s = stroke(f, 1, 20.0f, 4.0f, 1.0f);
    ASSERT_EQ(paint(f, &s), 0);
    ASSERT_GT(count_of(f), 0u);
    ASSERT_GT(cells_of(f), 1u);

    static DeFoliage o;
    memset(&o, 0, sizeof o);
    text_round_trip(f, DE_FOLIAGE_COMPONENT, layer(f), &o);
    EXPECT_MEMEQ(o.types, layer(f)->types, sizeof o.types);

    static page_set ps;
    pages_of(f, &ps);
    ASSERT_GT(ps.n, 1u);
    static DeFoliagePage copy;
    for (u32 i = 0; i < ps.n; ++i) {
        const DeFoliagePage *pg =
            (const DeFoliagePage *)de_entity_get_const(f->engine.world, ps.e[i], page_comp(f));
        ASSERT_TRUE(pg != NULL);
        memset(&copy, 0, sizeof copy);
        text_round_trip(f, "DeFoliagePage", pg, &copy);
        EXPECT_EQ(copy.layer.bits, pg->layer.bits);
        EXPECT_EQ(copy.x, pg->x);
        EXPECT_EQ(copy.z, pg->z);
        EXPECT_EQ(copy.page, pg->page);
        EXPECT_EQ(copy.count, pg->count);
        EXPECT_EQ(copy.revision, pg->revision);
        EXPECT_MEMEQ(copy.instances, pg->instances, sizeof pg->instances);
    }
}

/* ---- DIDE-R167 AC1: a large brush ---- */

/* A 100 m brush (radius 50 m) at 4 instances per m^2 is stored in full: far more than the old
 * 384-instance, 32-cell layer, over many cells and pages; info agrees; undo and redo round-trip. */
TEST_F(editor_foliage, a_100m_brush_at_4_per_m2_is_stored_in_full, foliage_fx) {
    de_ep_foliage_stroke s = stroke(f, 0, 0.0f, 0.0f, 1.0f);
    s.radius = 50.0f;
    s.density = 4.0f;
    const de_ep_edit_op op = stroke_op(f, "foliage.paint", &s);
    u32 tx = 0;
    ASSERT_EQ(edit(f, &op, &tx), 0);
    static inst_list after, now;
    instances_of(f, &after);
    /* pi * 50^2 * 4 = 31416 candidates; the spacing rule (0.25 m at 4/m^2) rejects about a
     * quarter of them, so about 22500 are placed: 58 times the old 384-instance layer. */
    EXPECT_GT(after.n, 20000u);
    EXPECT_TRUE(after.n <= 31416u + 64u);
    for (u32 k = 0; k < after.n; ++k) {
        const f32 x = after.i[k].x, z = after.i[k].z;
        EXPECT_TRUE(x * x + z * z <= 50.0f * 50.0f + 1e-2f);
        EXPECT_FLOAT_EQ(after.i[k].y, 2.0f, 1e-6f);
    }
    const u32 cells = cells_of(f);
    EXPECT_GT(cells, 32u); /* the 100 m brush spans about 7 x 7 cells of 16 m */
    static page_set ps;
    pages_of(f, &ps);
    EXPECT_GE(ps.n, cells);
    EXPECT_GE(ps.n, (after.n + DE_FOLIAGE_PAGE_INSTANCES - 1u) / DE_FOLIAGE_PAGE_INSTANCES);

    mpack_node_t r = result(info(f));
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(r, "count")), after.n);
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(r, "cells")), cells);

    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
    EXPECT_EQ(count_of(f), 0u);
    pages_of(f, &ps);
    EXPECT_EQ(ps.n, 0u);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_REDO, tx), 0);
    instances_of(f, &now);
    EXPECT_TRUE(now.n == after.n &&
                memcmp(now.i, after.i, sizeof(de_foliage_instance) * after.n) == 0);
}

#endif /* !DE_SHIPPING */
