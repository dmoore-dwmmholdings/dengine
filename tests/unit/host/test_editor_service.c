#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "ecs/hot_reload_internal.h"
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include "host/hot_reload.h"
#include <dengine/de_asset.h>
#include <dengine/de_ecs.h>
#include <dengine/de_math.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>
#include <stdio.h>

/* DENG-642: unit tests for the editor service's session (modes, step, snapshots, diff) and
 * transactional edit handlers. Actual handlers, no transport, window or renderer. */

void de__ed_session_shutdown(void);

typedef struct service_fx {
    de_engine engine;
    de__host host;
    de__editor ed;
    de_task_system *tasks;
    unsigned char arena[65536];
    char note[32768];
} service_fx;

static void service_fx_setup(service_fx *f) {
    memset(f, 0, sizeof *f);
    ASSERT_EQ(de_log_init(&(de_log_desc){.no_console_sink = true}), DE_OK);
    ASSERT_EQ(de_type_registry_create(NULL, &f->engine.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->engine.types}, &f->engine.world), DE_OK);
    (void)de_builtin_components(f->engine.world);
    f->ed.e = &f->engine;
    f->ed.h = &f->host;
    f->ed.mode = DE_EP_WORLD_MODE_EDIT;
    f->ed.arena = f->arena;
    f->ed.arena_cap = sizeof f->arena;
    f->ed.note = f->note;
    f->ed.note_cap = sizeof f->note;
}
static void service_fx_teardown(service_fx *f) {
    for (u32 i = 0; i < f->ed.reset_count; ++i)
        f->ed.resets[i](&f->ed);
    de__ed_session_shutdown();
    de_world_destroy(f->engine.world);
    if (f->engine.assets)
        de_asset_system_destroy(f->engine.assets);
    if (f->tasks)
        de_task_system_destroy(f->tasks);
    de_type_registry_destroy(f->engine.types);
    de_log_shutdown();
}

typedef struct SvcHealth {
    f32 current, max;
} SvcHealth;
static const de_field_info svc_health_fields[] = {
    DE_FIELD(SvcHealth, current, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(SvcHealth, max, DE_FK_F32, DE_FIELD_DEFAULT),
};
static de_component register_health(service_fx *f) {
    ASSERT_TRUE(DE_HANDLE_VALID(
        de_type_register(f->engine.types, &DE_TYPE_DESC(SvcHealth, 1, svc_health_fields))));
    const de_component c =
        de_component_register(f->engine.world, de_type_find(f->engine.types, "SvcHealth"),
                              DE_COMP_SERIALIZE | DE_COMP_SNAPSHOT);
    ASSERT_TRUE(DE_HANDLE_VALID(c));
    return c;
}
static de_entity spawn_health(service_fx *f, de_component c, f32 current) {
    const de_entity e = de_entity_spawn(f->engine.world);
    SvcHealth *h = de_entity_add(f->engine.world, e, c);
    ASSERT_TRUE(h != NULL);
    h->current = current;
    h->max = 100;
    return e;
}
static f32 health_of(service_fx *f, de_entity e, de_component c) {
    const SvcHealth *h = de_entity_get(f->engine.world, e, c);
    ASSERT_TRUE(h != NULL);
    return h->current;
}

/* ---------- Requests ---------- */

typedef struct reply {
    mpack_tree_t tree;
    mpack_node_t root;
    char buffer[4096];
} reply;

static void call(service_fx *f, de_ep_method method, const char *params, size_t size, reply *r) {
    static const char empty_map[] = "\x80";
    const de_ep_request req = {.id = 642,
                               .method = de_ep_cstr(de_ep_method_name(method)),
                               .params =
                                   params ? (de_ep_raw){params, size} : (de_ep_raw){empty_map, 1}};
    de_ep_arena arena = {f->arena, sizeof f->arena, 0};
    mpack_writer_t w;
    mpack_writer_init(&w, r->buffer, sizeof r->buffer);
    ASSERT_TRUE(f->ed.handlers[method] != NULL);
    f->ed.handlers[method](&f->ed, &req, &arena, &w);
    const size_t used = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_init_data(&r->tree, r->buffer, used);
    mpack_tree_parse(&r->tree);
    ASSERT_EQ(mpack_tree_error(&r->tree), mpack_ok);
    r->root = mpack_tree_root(&r->tree);
    ASSERT_EQ(mpack_node_u64(mpack_node_map_cstr(r->root, "id")), 642u);
}
/* The error code, or 0 for a result. */
static int code_of(reply *r) {
    if (!mpack_node_map_contains_cstr(r->root, "error")) {
        EXPECT_TRUE(mpack_node_map_contains_cstr(r->root, "result"));
        return 0;
    }
    return (int)mpack_node_i32(mpack_node_map_cstr(mpack_node_map_cstr(r->root, "error"), "code"));
}
static void done(reply *r) {
    EXPECT_EQ(mpack_tree_destroy(&r->tree), mpack_ok);
}

static int call_code(service_fx *f, de_ep_method method, const char *params, size_t size) {
    reply r;
    call(f, method, params, size, &r);
    const int code = code_of(&r);
    done(&r);
    return code;
}

#define WRITE_PARAMS(buf, writer, value)                                                           \
    size_t buf##_size;                                                                             \
    do {                                                                                           \
        mpack_writer_t w_;                                                                         \
        mpack_writer_init(&w_, buf, sizeof buf);                                                   \
        writer(&w_, value);                                                                        \
        buf##_size = mpack_writer_buffer_used(&w_);                                                \
        ASSERT_EQ(mpack_writer_destroy(&w_), mpack_ok);                                            \
    } while (0)

static int set_mode(service_fx *f, de_ep_world_mode mode) {
    char p[64];
    const de_ep_set_mode_params q = {.mode = mode};
    WRITE_PARAMS(p, de_ep_write_set_mode_params, &q);
    return call_code(f, DE_EP_METHOD_SESSION_SET_MODE, p, p_size);
}
static int step(service_fx *f, u32 frames) {
    char p[64];
    const de_ep_step_params q = {.has_frames = true, .frames = frames};
    WRITE_PARAMS(p, de_ep_write_step_params, &q);
    return call_code(f, DE_EP_METHOD_SESSION_STEP, p, p_size);
}
static u32 snap_take(service_fx *f) {
    reply r;
    call(f, DE_EP_METHOD_SNAPSHOT_TAKE, NULL, 0, &r);
    ASSERT_EQ(code_of(&r), 0);
    const u32 id =
        mpack_node_u32(mpack_node_map_cstr(mpack_node_map_cstr(r.root, "result"), "snap"));
    done(&r);
    return id;
}
static int snap_call(service_fx *f, de_ep_method method, u32 snap) {
    char p[32];
    const de_ep_snap_params q = {.snap = snap};
    WRITE_PARAMS(p, de_ep_write_snap_params, &q);
    return call_code(f, method, p, p_size);
}
static void run_ticks(service_fx *f) {
    for (u32 i = 0; i < f->ed.tick_count; ++i)
        f->ed.ticks[i](&f->ed);
}

/* ---------- Session ---------- */

TEST_F(editor_service, ping_and_info_report_the_edit_session, service_fx) {
    de__ed_add_capability(&f->ed, "world.edit");
    de__ed_session_register(&f->ed);
    EXPECT_EQ(call_code(f, DE_EP_METHOD_PING, NULL, 0), 0);
    reply r;
    call(f, DE_EP_METHOD_SESSION_INFO, NULL, 0, &r);
    ASSERT_EQ(code_of(&r), 0);
    const mpack_node_t result = mpack_node_map_cstr(r.root, "result");
    const mpack_node_t mode = mpack_node_map_cstr(result, "mode");
    EXPECT_EQ(mpack_node_strlen(mode), strlen("edit"));
    EXPECT_EQ(memcmp(mpack_node_str(mode), de_ep_world_mode_name(DE_EP_WORLD_MODE_EDIT),
                     mpack_node_strlen(mode)),
              0);
    const mpack_node_t caps = mpack_node_map_cstr(result, "capabilities");
    ASSERT_EQ(mpack_node_array_length(caps), 1u);
    EXPECT_EQ(memcmp(mpack_node_str(mpack_node_array_at(caps, 0)), "world.edit", 10), 0);
    done(&r);
}

static void no_op(de_system_ctx *ctx) {
    (void)ctx;
}

TEST_F(editor_service, modes_and_step_gate_only_the_game_systems, service_fx) {
    de__world_set_system_owner(f->engine.world, DE__HOST_GAME_SYSTEM_OWNER);
    const de_system game = de_system_add(
        f->engine.world,
        &(de_system_desc){.name = "game.sim", .schedule = DE_SCHEDULE_UPDATE, .fn = no_op});
    de__world_set_system_owner(f->engine.world, DE__SYSTEM_OWNER_ENGINE);
    const de_system engine = de_system_add(
        f->engine.world,
        &(de_system_desc){.name = "engine.sim", .schedule = DE_SCHEDULE_UPDATE, .fn = no_op});
    ASSERT_TRUE(DE_HANDLE_VALID(game) && DE_HANDLE_VALID(engine));
    de__ed_session_register(&f->ed);
    /* The session starts in Edit: the game holds still, the engine keeps running. */
    EXPECT_FALSE(de_system_enabled(f->engine.world, game));
    EXPECT_TRUE(de_system_enabled(f->engine.world, engine));
    /* Step needs Paused. */
    EXPECT_EQ(step(f, 2), DE_EP_ERR_WRONG_MODE);
    EXPECT_FALSE(de_system_enabled(f->engine.world, game));

    EXPECT_EQ(set_mode(f, DE_EP_WORLD_MODE_PLAY), 0);
    EXPECT_EQ(f->ed.mode, DE_EP_WORLD_MODE_PLAY);
    EXPECT_TRUE(de_system_enabled(f->engine.world, game));
    EXPECT_EQ(set_mode(f, DE_EP_WORLD_MODE_PAUSED), 0);
    EXPECT_FALSE(de_system_enabled(f->engine.world, game));

    /* Step 2 runs the game for exactly two service ticks, then holds it again. */
    EXPECT_EQ(step(f, 2), 0);
    EXPECT_TRUE(de_system_enabled(f->engine.world, game));
    run_ticks(f);
    EXPECT_TRUE(de_system_enabled(f->engine.world, game));
    run_ticks(f);
    EXPECT_FALSE(de_system_enabled(f->engine.world, game));
    EXPECT_TRUE(de_system_enabled(f->engine.world, engine));
    EXPECT_EQ(f->ed.mode, DE_EP_WORLD_MODE_PAUSED);
}

TEST_F(editor_service, stop_restores_the_world_from_before_play, service_fx) {
    const de_component health = register_health(f);
    const de_entity e = spawn_health(f, health, 10);
    de__ed_session_register(&f->ed);
    EXPECT_EQ(set_mode(f, DE_EP_WORLD_MODE_PLAY), 0);
    ((SvcHealth *)de_entity_get(f->engine.world, e, health))->current = 3;
    const de_entity spawned = spawn_health(f, health, 1);
    EXPECT_EQ(set_mode(f, DE_EP_WORLD_MODE_EDIT), 0);
    EXPECT_FLOAT_EQ(health_of(f, e, health), 10, 0);
    EXPECT_FALSE(de_entity_alive(f->engine.world, spawned));
}

TEST_F(editor_service, restart_returns_to_the_initial_world_in_edit, service_fx) {
    const de_component health = register_health(f);
    const de_entity e = spawn_health(f, health, 25);
    de__ed_session_register(&f->ed);
    EXPECT_EQ(set_mode(f, DE_EP_WORLD_MODE_SIMULATE), 0);
    ((SvcHealth *)de_entity_get(f->engine.world, e, health))->current = 0;
    EXPECT_EQ(call_code(f, DE_EP_METHOD_SESSION_RESTART, NULL, 0), 0);
    EXPECT_EQ(f->ed.mode, DE_EP_WORLD_MODE_EDIT);
    EXPECT_FLOAT_EQ(health_of(f, e, health), 25, 0);
}

/* ---------- Snapshots ---------- */

TEST_F(editor_service, snapshot_restore_and_release_round_trip, service_fx) {
    const de_component health = register_health(f);
    const de_entity e = spawn_health(f, health, 40);
    de__ed_session_register(&f->ed);
    const u32 a = snap_take(f);
    ASSERT_TRUE(a != 0);
    ((SvcHealth *)de_entity_get(f->engine.world, e, health))->current = 7;
    EXPECT_EQ(snap_call(f, DE_EP_METHOD_SNAPSHOT_RESTORE, a), 0);
    EXPECT_FLOAT_EQ(health_of(f, e, health), 40, 0);
    EXPECT_EQ(snap_call(f, DE_EP_METHOD_SNAPSHOT_RELEASE, a), 0);
    EXPECT_EQ(snap_call(f, DE_EP_METHOD_SNAPSHOT_RELEASE, a), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(snap_call(f, DE_EP_METHOD_SNAPSHOT_RESTORE, a), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(snap_call(f, DE_EP_METHOD_SNAPSHOT_RESTORE, 0), DE_EP_ERR_NOT_FOUND);
}

static bool list_has(mpack_node_t list, u64 value) {
    for (size_t i = 0; i < mpack_node_array_length(list); ++i)
        if (mpack_node_u64(mpack_node_array_at(list, i)) == value)
            return true;
    return false;
}

TEST_F(editor_service, snapshot_diff_reports_spawns_despawns_and_changed_fields, service_fx) {
    const de_component health = register_health(f);
    const de_entity kept = spawn_health(f, health, 50);
    const de_entity gone = spawn_health(f, health, 60);
    de__ed_session_register(&f->ed);
    const u32 a = snap_take(f);
    ((SvcHealth *)de_entity_get(f->engine.world, kept, health))->current = 49;
    de_entity_despawn(f->engine.world, gone);
    const de_entity born = spawn_health(f, health, 1);
    const u32 b = snap_take(f);

    char p[32];
    const de_ep_snap_diff_params q = {.a = a, .b = b};
    WRITE_PARAMS(p, de_ep_write_snap_diff_params, &q);
    reply r;
    call(f, DE_EP_METHOD_SNAPSHOT_DIFF, p, p_size, &r);
    ASSERT_EQ(code_of(&r), 0);
    const mpack_node_t result = mpack_node_map_cstr(r.root, "result");
    const mpack_node_t spawned = mpack_node_map_cstr(result, "spawned");
    const mpack_node_t despawned = mpack_node_map_cstr(result, "despawned");
    const mpack_node_t changed = mpack_node_map_cstr(result, "changed");
    EXPECT_TRUE(list_has(spawned, born.bits));
    EXPECT_TRUE(list_has(despawned, gone.bits));
    EXPECT_FALSE(list_has(spawned, kept.bits));
    EXPECT_FALSE(list_has(despawned, kept.bits));
    /* Only `current` of the kept entity changed; its value is the new one. */
    ASSERT_EQ(mpack_node_array_length(changed), 1u);
    const mpack_node_t c = mpack_node_array_at(changed, 0);
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(c, "entity")), kept.bits);
    const mpack_node_t fields = mpack_node_map_cstr(c, "fields");
    ASSERT_EQ(mpack_node_map_count(fields), 1u);
    EXPECT_TRUE(mpack_node_map_contains_cstr(fields, "current"));
    EXPECT_FLOAT_EQ(mpack_node_float(mpack_node_map_cstr(fields, "current")), 49, 0);
    done(&r);

    /* A snapshot diffed with itself is empty; an unknown one is NOT_FOUND. */
    const de_ep_snap_diff_params same = {.a = b, .b = b};
    char p2[32];
    WRITE_PARAMS(p2, de_ep_write_snap_diff_params, &same);
    call(f, DE_EP_METHOD_SNAPSHOT_DIFF, p2, p2_size, &r);
    ASSERT_EQ(code_of(&r), 0);
    const mpack_node_t res2 = mpack_node_map_cstr(r.root, "result");
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(res2, "spawned")), 0u);
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(res2, "despawned")), 0u);
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(res2, "changed")), 0u);
    done(&r);
    const de_ep_snap_diff_params missing = {.a = a, .b = b + 100};
    char p3[32];
    WRITE_PARAMS(p3, de_ep_write_snap_diff_params, &missing);
    EXPECT_EQ(call_code(f, DE_EP_METHOD_SNAPSHOT_DIFF, p3, p3_size), DE_EP_ERR_NOT_FOUND);
}

/* ---------- Transactional edits ---------- */

static u32 edit_begin(service_fx *f) {
    char p[64];
    const de_ep_edit_begin_params q = {.label = de_ep_cstr("svc"), .scene = 1};
    WRITE_PARAMS(p, de_ep_write_edit_begin_params, &q);
    reply r;
    call(f, DE_EP_METHOD_EDIT_BEGIN, p, p_size, &r);
    ASSERT_EQ(code_of(&r), 0);
    const u32 tx = mpack_node_u32(mpack_node_map_cstr(mpack_node_map_cstr(r.root, "result"), "tx"));
    done(&r);
    return tx;
}
static int edit_set(service_fx *f, u32 tx, de_entity e, const char *path, f32 value) {
    char v[8];
    mpack_writer_t vw;
    mpack_writer_init(&vw, v, sizeof v);
    mpack_write_float(&vw, value);
    const size_t vn = mpack_writer_buffer_used(&vw);
    ASSERT_EQ(mpack_writer_destroy(&vw), mpack_ok);
    de_ep_edit_op op = {.op = de_ep_cstr("set"),
                        .has_entity = true,
                        .entity = e.bits,
                        .has_path = true,
                        .path = de_ep_cstr(path),
                        .has_value = true,
                        .value = {v, vn}};
    const de_ep_edit_apply_params q = {.tx = tx, .ops = {&op, 1}};
    char p[256];
    WRITE_PARAMS(p, de_ep_write_edit_apply_params, &q);
    return call_code(f, DE_EP_METHOD_EDIT_APPLY, p, p_size);
}
static int tx_call(service_fx *f, de_ep_method method, u32 tx) {
    char p[32];
    const de_ep_tx_params q = {.tx = tx};
    WRITE_PARAMS(p, de_ep_write_tx_params, &q);
    return call_code(f, method, p, p_size);
}

TEST_F(editor_service, set_edit_commits_undoes_and_redoes, service_fx) {
    const de_component health = register_health(f);
    const de_entity e = spawn_health(f, health, 5);
    de__ed_edit_register(&f->ed);
    const u32 tx = edit_begin(f);
    EXPECT_EQ(edit_set(f, tx, e, "SvcHealth.current", 8), 0);
    EXPECT_EQ(edit_set(f, tx, e, "SvcHealth.current", 9), 0);
    EXPECT_FLOAT_EQ(health_of(f, e, health), 9, 0);
    /* Undo needs a committed transaction. */
    EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), DE_EP_ERR_CONFLICT);
    EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_COMMIT, tx), 0);
    EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_COMMIT, tx), DE_EP_ERR_CONFLICT);
    EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_REDO, tx), DE_EP_ERR_CONFLICT);
    EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
    EXPECT_FLOAT_EQ(health_of(f, e, health), 5, 0);
    EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_REDO, tx), 0);
    EXPECT_FLOAT_EQ(health_of(f, e, health), 9, 0);
}

TEST_F(editor_service, stop_forgets_transactions_so_undo_cannot_write_play_values, service_fx) {
    const de_component health = register_health(f);
    const de_entity e = spawn_health(f, health, 5);
    de__ed_session_register(&f->ed);
    de__ed_edit_register(&f->ed);
    EXPECT_EQ(set_mode(f, DE_EP_WORLD_MODE_PLAY), 0);
    const u32 tx = edit_begin(f);
    EXPECT_EQ(edit_set(f, tx, e, "SvcHealth.current", 8), 0);
    EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_COMMIT, tx), 0);
    EXPECT_EQ(set_mode(f, DE_EP_WORLD_MODE_EDIT), 0);
    EXPECT_FLOAT_EQ(health_of(f, e, health), 5, 0);
    /* The transaction died with the play-time world. */
    EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_REDO, tx), DE_EP_ERR_NOT_FOUND);
    EXPECT_FLOAT_EQ(health_of(f, e, health), 5, 0);
}

TEST_F(editor_service, snapshot_restore_forgets_transactions, service_fx) {
    const de_component health = register_health(f);
    const de_entity e = spawn_health(f, health, 5);
    de__ed_session_register(&f->ed);
    de__ed_edit_register(&f->ed);
    const u32 snap = snap_take(f);
    const u32 tx = edit_begin(f);
    EXPECT_EQ(edit_set(f, tx, e, "SvcHealth.current", 8), 0);
    EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_COMMIT, tx), 0);
    EXPECT_EQ(snap_call(f, DE_EP_METHOD_SNAPSHOT_RESTORE, snap), 0);
    EXPECT_FLOAT_EQ(health_of(f, e, health), 5, 0);
    EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), DE_EP_ERR_NOT_FOUND);
    EXPECT_FLOAT_EQ(health_of(f, e, health), 5, 0);
}

TEST_F(editor_service, cancel_reverts_every_applied_op, service_fx) {
    const de_component health = register_health(f);
    const de_entity e = spawn_health(f, health, 12);
    de__ed_edit_register(&f->ed);
    const u32 tx = edit_begin(f);
    EXPECT_EQ(edit_set(f, tx, e, "SvcHealth.current", 1), 0);
    EXPECT_EQ(edit_set(f, tx, e, "SvcHealth.max", 2), 0);
    EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_CANCEL, tx), 0);
    EXPECT_FLOAT_EQ(health_of(f, e, health), 12, 0);
    const SvcHealth *h = de_entity_get(f->engine.world, e, health);
    EXPECT_FLOAT_EQ(h->max, 100, 0);
    /* A cancelled transaction is gone. */
    EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_COMMIT, tx), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), DE_EP_ERR_NOT_FOUND);
}

TEST_F(editor_service, invalid_set_ops_change_nothing, service_fx) {
    const de_component health = register_health(f);
    const de_entity e = spawn_health(f, health, 33);
    de__ed_edit_register(&f->ed);
    const u32 tx = edit_begin(f);
    EXPECT_EQ(edit_set(f, tx, e, "SvcHealth.nope", 1), DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(edit_set(f, tx, e, "Missing.current", 1), DE_EP_ERR_NOT_FOUND);
    const de_entity dead = de_entity_spawn(f->engine.world);
    de_entity_despawn(f->engine.world, dead);
    EXPECT_EQ(edit_set(f, tx, dead, "SvcHealth.current", 1), DE_EP_ERR_NOT_FOUND);
    EXPECT_FLOAT_EQ(health_of(f, e, health), 33, 0);
    EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_COMMIT, 9999), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_CANCEL, tx), 0);
}

/* ---------- Sibling order: EditOp.index (DIDE protocol 1.14, ui.editor; DENG-778) ---------- */

/* Child number `i` of `p` in list order, or DE_ENTITY_NULL. */
static de_entity kid_at(service_fx *f, de_entity p, u32 i) {
    de_entity c = de_entity_children_first(f->engine.world, p);
    for (; c.bits != 0 && i > 0; --i)
        c = de_entity_children_next(f->engine.world, c);
    return c;
}
/* A parent with `n` children, made directly (not through edits). */
static de_entity family(service_fx *f, de_entity *kids, u32 n) {
    de_world *w = f->engine.world;
    const de_entity p = de_entity_spawn(w);
    for (u32 i = 0; i < n; ++i) {
        kids[i] = de_entity_spawn(w);
        ASSERT_EQ(de_entity_set_parent(w, kids[i], p), DE_OK);
    }
    return p;
}
/* `p`'s children are exactly `want`, in that order. */
static void expect_kids(service_fx *f, de_entity p, const de_entity *want, u32 n) {
    EXPECT_EQ(de_entity_children_count(f->engine.world, p), n);
    for (u32 i = 0; i < n; ++i) {
        EXPECT_EQ(kid_at(f, p, i).bits, want[i].bits);
        EXPECT_EQ(de_entity_child_index(f->engine.world, want[i]), i);
    }
}
/* One op in its own committed transaction; the error code, or 0. */
static int commit_one(service_fx *f, de_ep_edit_op op, u32 *tx) {
    *tx = edit_begin(f);
    const de_ep_edit_apply_params q = {.tx = *tx, .ops = {&op, 1}};
    char p[1024];
    WRITE_PARAMS(p, de_ep_write_edit_apply_params, &q);
    const int code = call_code(f, DE_EP_METHOD_EDIT_APPLY, p, p_size);
    if (code != 0) {
        EXPECT_EQ(tx_call(f, DE_EP_METHOD_EDIT_CANCEL, *tx), 0);
        return code;
    }
    return tx_call(f, DE_EP_METHOD_EDIT_COMMIT, *tx);
}
static de_ep_edit_op spawn_under(de_entity parent, bool has_index, u32 index) {
    return (de_ep_edit_op){.op = de_ep_cstr("spawn"),
                           .has_scene = true,
                           .scene = 1,
                           .has_parent = parent.bits != 0,
                           .parent = parent.bits,
                           .has_index = has_index,
                           .index = index};
}
static de_ep_edit_op reparent_to(de_entity e, de_entity parent, bool has_index, u32 index) {
    return (de_ep_edit_op){.op = de_ep_cstr("reparent"),
                           .has_entity = true,
                           .entity = e.bits,
                           .has_parent = parent.bits != 0,
                           .parent = parent.bits,
                           .has_keep_world = true,
                           .keep_world = false,
                           .has_index = has_index,
                           .index = index};
}

TEST_F(editor_service, spawn_index_inserts_and_past_the_end_appends, service_fx) {
    de__ed_edit_register(&f->ed);
    de_entity k[3];
    const de_entity p = family(f, k, 3);
    u32 tx, tx_end, tx_plain;
    ASSERT_EQ(commit_one(f, spawn_under(p, true, 1), &tx), 0);
    const de_entity n = kid_at(f, p, 1);
    ASSERT_TRUE(n.bits != 0 && n.bits != k[1].bits);
    expect_kids(f, p, (de_entity[]){k[0], n, k[1], k[2]}, 4);
    /* At or past the end appends; absent appends as before. */
    ASSERT_EQ(commit_one(f, spawn_under(p, true, 99), &tx_end), 0);
    const de_entity m = kid_at(f, p, 4);
    ASSERT_EQ(commit_one(f, spawn_under(p, false, 0), &tx_plain), 0);
    const de_entity o = kid_at(f, p, 5);
    expect_kids(f, p, (de_entity[]){k[0], n, k[1], k[2], m, o}, 6);
    /* Undo removes it; redo puts the same entity back at index 1. */
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
    expect_kids(f, p, (de_entity[]){k[0], k[1], k[2], m, o}, 5);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_REDO, tx), 0);
    expect_kids(f, p, (de_entity[]){k[0], n, k[1], k[2], m, o}, 6);
    /* Without a parent the index is ignored. */
    u32 tx_root;
    ASSERT_EQ(commit_one(f, spawn_under(DE_ENTITY_NULL, true, 0), &tx_root), 0);
    EXPECT_EQ(de_entity_children_count(f->engine.world, p), 6u);
}

/* The DIDE conformance check (protocol 1.14): an indexed reparent to 0 reorders, and undo puts
 * the child back at its old index (the inverse is a reparent at index 2). */
TEST_F(editor_service, reparent_index_reorders_and_undo_restores_the_old_index, service_fx) {
    de__ed_edit_register(&f->ed);
    de_entity k[3];
    const de_entity p = family(f, k, 3);
    u32 tx;
    ASSERT_EQ(commit_one(f, reparent_to(k[2], p, true, 0), &tx), 0);
    expect_kids(f, p, (de_entity[]){k[2], k[0], k[1]}, 3);
    for (u32 i = 0; i < 2; ++i) {
        ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
        expect_kids(f, p, k, 3);
        ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_REDO, tx), 0);
        expect_kids(f, p, (de_entity[]){k[2], k[0], k[1]}, 3);
    }
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
    /* The index counts the siblings without the entity: moving k0 to 1 puts it after k1. */
    u32 tx_mid;
    ASSERT_EQ(commit_one(f, reparent_to(k[0], p, true, 1), &tx_mid), 0);
    expect_kids(f, p, (de_entity[]){k[1], k[0], k[2]}, 3);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx_mid), 0);
    expect_kids(f, p, k, 3);
    /* To another parent at an index; undo returns it to the middle of the first parent. */
    de_entity q[2];
    const de_entity p2 = family(f, q, 2);
    u32 tx_move;
    ASSERT_EQ(commit_one(f, reparent_to(k[1], p2, true, 1), &tx_move), 0);
    expect_kids(f, p2, (de_entity[]){q[0], k[1], q[1]}, 3);
    expect_kids(f, p, (de_entity[]){k[0], k[2]}, 2);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx_move), 0);
    expect_kids(f, p, k, 3);
    expect_kids(f, p2, q, 2);
    /* From a root to an index, and back to a root on undo. */
    const de_entity loose = de_entity_spawn(f->engine.world);
    u32 tx_adopt;
    ASSERT_EQ(commit_one(f, reparent_to(loose, p, true, 0), &tx_adopt), 0);
    expect_kids(f, p, (de_entity[]){loose, k[0], k[1], k[2]}, 4);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx_adopt), 0);
    EXPECT_EQ(de_entity_parent(f->engine.world, loose).bits, 0u);
    expect_kids(f, p, k, 3);
    /* An only child stays where it is. */
    de_entity only[1];
    const de_entity p3 = family(f, only, 1);
    u32 tx_only;
    ASSERT_EQ(commit_one(f, reparent_to(only[0], p3, true, 0), &tx_only), 0);
    expect_kids(f, p3, only, 1);
}

TEST_F(editor_service, despawn_undo_puts_the_entity_back_at_its_index, service_fx) {
    de__ed_edit_register(&f->ed);
    de_entity k[3];
    const de_entity p = family(f, k, 3);
    u32 tx;
    ASSERT_EQ(commit_one(f,
                         (de_ep_edit_op){
                             .op = de_ep_cstr("despawn"), .has_entity = true, .entity = k[1].bits},
                         &tx),
              0);
    expect_kids(f, p, (de_entity[]){k[0], k[2]}, 2);
    for (u32 i = 0; i < 2; ++i) {
        ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
        expect_kids(f, p, k, 3);
        ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_REDO, tx), 0);
        expect_kids(f, p, (de_entity[]){k[0], k[2]}, 2);
    }
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
    u32 tx_first;
    ASSERT_EQ(commit_one(f,
                         (de_ep_edit_op){
                             .op = de_ep_cstr("despawn"), .has_entity = true, .entity = k[0].bits},
                         &tx_first),
              0);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx_first), 0);
    expect_kids(f, p, k, 3);
}

/* ---------- Transactional prefab instantiate (DENG-764) ---------- */

#if DE_HAS_COOKER
typedef struct THealth {
    f32 current, max;
} THealth;
typedef struct TTarget {
    de_entity target;
    u32 texture;
    f32 speed;
} TTarget;
static const de_field_info t_health_fields[] = {
    DE_FIELD(THealth, current, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(THealth, max, DE_FK_F32, DE_FIELD_DEFAULT),
};
static const de_field_info t_target_fields[] = {
    DE_FIELD(TTarget, target, DE_FK_ENTITY, DE_FIELD_DEFAULT),
    DE_FIELD_EX(TTarget, texture, DE_FK_ASSET, DE_FIELD_DEFAULT | DE_FIELD_ASSET_REF,
                {0, 0, 0, 0, "blob", 0}),
    DE_FIELD(TTarget, speed, DE_FK_F32, DE_FIELD_DEFAULT),
};

typedef struct prefab_ids {
    de_component health, target;
} prefab_ids;

/* The crate prefab (a root with THealth, a child with TTarget aimed at the root), with the scene
 * and edit handlers. */
static prefab_ids prefab_setup(service_fx *f) {
    prefab_ids ids;
    ASSERT_TRUE(DE_HANDLE_VALID(
        de_type_register(f->engine.types, &DE_TYPE_DESC(THealth, 1, t_health_fields))));
    ASSERT_TRUE(DE_HANDLE_VALID(
        de_type_register(f->engine.types, &DE_TYPE_DESC(TTarget, 1, t_target_fields))));
    ids.health = de_component_register(f->engine.world, de_type_find(f->engine.types, "THealth"),
                                       DE_COMP_SERIALIZE);
    ids.target = de_component_register(f->engine.world, de_type_find(f->engine.types, "TTarget"),
                                       DE_COMP_SERIALIZE);
    ASSERT_TRUE(DE_HANDLE_VALID(ids.health) && DE_HANDLE_VALID(ids.target));
    ASSERT_EQ(de_fs_mount("svc764", DE_EDITOR_PREFAB_DATA_DIR, 0, 0), DE_OK);
    de_fp_env_init_thread();
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 1}, &f->tasks), DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.tasks = f->tasks,
                                                             .types = f->engine.types,
                                                             .io_threads = 1,
                                                             .cache_dir = "nocache764:/cache"},
                                     &f->engine.assets),
              DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->engine.assets, "svc764:/assets.toml"), DE_OK);
    de__ed_scene_register(&f->ed);
    de__ed_edit_register(&f->ed);
    return ids;
}
static void prefab_teardown(void) {
    (void)de_fs_unmount("svc764", DE_EDITOR_PREFAB_DATA_DIR);
}

typedef struct ent_list {
    u64 ids[16];
    u32 count;
} ent_list;
static void collect(const de_query_chunk *chunk, void *user) {
    ent_list *l = (ent_list *)user;
    for (u32 i = 0; i < chunk->count && l->count < 16; ++i)
        l->ids[l->count++] = chunk->entities[i].bits;
}
/* Every live entity. */
static ent_list live_entities(service_fx *f) {
    ent_list l;
    memset(&l, 0, sizeof l);
    const de_query q = de_query_create(f->engine.world, &(de_query_desc){0});
    de_query_each(f->engine.world, q, collect, &l);
    de_query_destroy(f->engine.world, q);
    return l;
}

/* scene.serialize of the game scene (its stable ids, parents and components). */
static void serialize(service_fx *f, char *out, size_t cap) {
    char p[32];
    const de_ep_scene_params q = {1};
    WRITE_PARAMS(p, de_ep_write_scene_params, &q);
    reply r;
    call(f, DE_EP_METHOD_SCENE_SERIALIZE, p, p_size, &r);
    ASSERT_EQ(code_of(&r), 0);
    mpack_node_t text = mpack_node_map_cstr(mpack_node_map_cstr(r.root, "result"), "text");
    const size_t n = mpack_node_strlen(text);
    ASSERT_TRUE(n < cap);
    memcpy(out, mpack_node_str(text), n);
    out[n] = '\0';
    done(&r);
}

static int apply_ops(service_fx *f, u32 tx, de_ep_edit_op *ops, u32 n) {
    const de_ep_edit_apply_params q = {.tx = tx, .ops = {ops, n}};
    char p[1024];
    WRITE_PARAMS(p, de_ep_write_edit_apply_params, &q);
    return call_code(f, DE_EP_METHOD_EDIT_APPLY, p, p_size);
}
static de_ep_edit_op instantiate_op(const char *asset) {
    return (de_ep_edit_op){.op = de_ep_cstr("instantiate"),
                           .has_scene = true,
                           .scene = 1,
                           .has_path = true,
                           .path = de_ep_cstr(asset)};
}
static int overrides_code(service_fx *f, u64 entity) {
    char p[32];
    const de_ep_entity_ref q = {entity};
    WRITE_PARAMS(p, de_ep_write_entity_ref, &q);
    return call_code(f, DE_EP_METHOD_PREFAB_OVERRIDES, p, p_size);
}
static int revert_code(service_fx *f, u64 entity, const char *path) {
    char p[128];
    const de_ep_override_path_params q = {.entity = entity, .path = de_ep_cstr(path)};
    WRITE_PARAMS(p, de_ep_write_override_path_params, &q);
    return call_code(f, DE_EP_METHOD_PREFAB_REVERT_OVERRIDE, p, p_size);
}

/* The crate's root and child among the live entities. */
static void crate_of(service_fx *f, const prefab_ids *ids, de_entity *root, de_entity *child) {
    const ent_list l = live_entities(f);
    ASSERT_EQ(l.count, 2u);
    *root = *child = DE_ENTITY_NULL;
    for (u32 i = 0; i < l.count; ++i) {
        const de_entity e = {l.ids[i]};
        if (de_entity_has(f->engine.world, e, ids->health))
            *root = e;
        if (de_entity_has(f->engine.world, e, ids->target))
            *child = e;
    }
    ASSERT_TRUE(root->bits != 0 && child->bits != 0);
    EXPECT_EQ(de_entity_parent(f->engine.world, *child).bits, root->bits);
    const TTarget *t = de_entity_get(f->engine.world, *child, ids->target);
    ASSERT_TRUE(t != NULL);
    EXPECT_EQ(t->target.bits, root->bits);
}

/* The instance is back as it was: same ids, stable ids, record and base values. */
static void expect_crate(service_fx *f, const prefab_ids *ids, de_entity root, de_entity child,
                         const char *text) {
    de_entity r2, c2;
    crate_of(f, ids, &r2, &c2);
    EXPECT_EQ(r2.bits, root.bits);
    EXPECT_EQ(c2.bits, child.bits);
    static char now[4096];
    serialize(f, now, sizeof now);
    EXPECT_STREQ(now, text);
    EXPECT_EQ(overrides_code(f, root.bits), 0);
    THealth *h = de_entity_get(f->engine.world, root, ids->health);
    ASSERT_TRUE(h != NULL);
    h->current = 3;
    EXPECT_EQ(revert_code(f, root.bits, "THealth.current"), 0);
    EXPECT_FLOAT_EQ(h->current, 50, 0);
}
/* The whole tree, the instance record and the scene members are gone. */
static void expect_no_crate(service_fx *f, de_entity root, de_entity child, const char *empty) {
    EXPECT_FALSE(de_entity_alive(f->engine.world, root));
    EXPECT_FALSE(de_entity_alive(f->engine.world, child));
    EXPECT_EQ(live_entities(f).count, 0u);
    static char now[4096];
    serialize(f, now, sizeof now);
    EXPECT_STREQ(now, empty);
    EXPECT_EQ(overrides_code(f, root.bits), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(overrides_code(f, child.bits), DE_EP_ERR_NOT_FOUND);
}

TEST_F(editor_service, prefab_instantiate_op_undoes_the_whole_instance_and_redoes_it_exactly,
       service_fx) {
    const prefab_ids ids = prefab_setup(f);
    static char empty[4096], text[4096];
    serialize(f, empty, sizeof empty);
    const u32 tx = edit_begin(f);
    de_ep_edit_op op = instantiate_op("props/crate");
    ASSERT_EQ(apply_ops(f, tx, &op, 1), 0);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_COMMIT, tx), 0);
    de_entity root, child;
    crate_of(f, &ids, &root, &child);
    serialize(f, text, sizeof text);
    EXPECT_TRUE(strstr(text, "THealth") != NULL && strstr(text, "TTarget") != NULL);
    EXPECT_EQ(overrides_code(f, root.bits), 0);
    for (u32 i = 0; i < 2; ++i) {
        ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
        expect_no_crate(f, root, child, empty);
        ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_REDO, tx), 0);
        expect_crate(f, &ids, root, child, text);
    }
    prefab_teardown();
}

TEST_F(editor_service, prefab_instantiate_rolls_back_with_a_failed_op_or_a_cancel, service_fx) {
    const prefab_ids ids = prefab_setup(f);
    (void)ids;
    static char empty[4096];
    serialize(f, empty, sizeof empty);
    const u32 tx = edit_begin(f);
    /* A later op fails: the instantiate goes too, record and members included. */
    de_ep_edit_op ops[2] = {instantiate_op("props/crate"),
                            {.op = de_ep_cstr("set"),
                             .has_entity = true,
                             .entity = 0xFFFFFFFFull,
                             .has_path = true,
                             .path = de_ep_cstr("THealth.current"),
                             .has_value = true,
                             .value = {"\x01", 1}}};
    EXPECT_EQ(apply_ops(f, tx, ops, 2), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(live_entities(f).count, 0u);
    static char now[4096];
    serialize(f, now, sizeof now);
    EXPECT_STREQ(now, empty);
    /* A cancel removes an applied instantiate. */
    ASSERT_EQ(apply_ops(f, tx, ops, 1), 0);
    de_entity root, child;
    crate_of(f, &ids, &root, &child);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_CANCEL, tx), 0);
    expect_no_crate(f, root, child, empty);
    /* An unknown prefab changes nothing. */
    const u32 tx2 = edit_begin(f);
    de_ep_edit_op bad = instantiate_op("props/missing");
    EXPECT_EQ(apply_ops(f, tx2, &bad, 1), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(live_entities(f).count, 0u);
    prefab_teardown();
}

TEST_F(editor_service, prefab_uninstantiate_op_removes_an_instance_and_its_inverse_restores_it,
       service_fx) {
    const prefab_ids ids = prefab_setup(f);
    static char empty[4096], text[4096];
    serialize(f, empty, sizeof empty);
    const u32 tx = edit_begin(f);
    de_ep_edit_op op = instantiate_op("props/crate");
    ASSERT_EQ(apply_ops(f, tx, &op, 1), 0);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_COMMIT, tx), 0);
    de_entity root, child;
    crate_of(f, &ids, &root, &child);
    serialize(f, text, sizeof text);
    /* Only an instance root can be uninstantiated. */
    const u32 tx2 = edit_begin(f);
    de_ep_edit_op un = {
        .op = de_ep_cstr("uninstantiate"), .has_entity = true, .entity = child.bits};
    EXPECT_EQ(apply_ops(f, tx2, &un, 1), DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(live_entities(f).count, 2u);
    un.entity = root.bits;
    ASSERT_EQ(apply_ops(f, tx2, &un, 1), 0);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_COMMIT, tx2), 0);
    expect_no_crate(f, root, child, empty);
    for (u32 i = 0; i < 2; ++i) {
        ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx2), 0);
        expect_crate(f, &ids, root, child, text);
        ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_REDO, tx2), 0);
        expect_no_crate(f, root, child, empty);
    }
    /* Undo both, newest first, then redo the instantiate. */
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx2), 0);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
    expect_no_crate(f, root, child, empty);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_REDO, tx), 0);
    expect_crate(f, &ids, root, child, text);
    prefab_teardown();
}

/* DIDE protocol 1.14 (DENG-778): instantiate at an index; undo of an uninstantiate puts the
 * instance back at its index. */
TEST_F(editor_service, prefab_instantiate_index_places_the_root_among_siblings, service_fx) {
    const prefab_ids ids = prefab_setup(f);
    de_entity k[2];
    const de_entity p = family(f, k, 2);
    de_ep_edit_op op = instantiate_op("props/crate");
    op.has_parent = true;
    op.parent = p.bits;
    op.has_index = true;
    op.index = 1;
    u32 tx;
    ASSERT_EQ(commit_one(f, op, &tx), 0);
    const de_entity root = kid_at(f, p, 1);
    ASSERT_TRUE(de_entity_has(f->engine.world, root, ids.health));
    expect_kids(f, p, (de_entity[]){k[0], root, k[1]}, 3);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx), 0);
    expect_kids(f, p, k, 2);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_REDO, tx), 0);
    expect_kids(f, p, (de_entity[]){k[0], root, k[1]}, 3);
    u32 tx_un;
    ASSERT_EQ(commit_one(f,
                         (de_ep_edit_op){.op = de_ep_cstr("uninstantiate"),
                                         .has_entity = true,
                                         .entity = root.bits},
                         &tx_un),
              0);
    expect_kids(f, p, k, 2);
    ASSERT_EQ(tx_call(f, DE_EP_METHOD_EDIT_UNDO, tx_un), 0);
    expect_kids(f, p, (de_entity[]){k[0], root, k[1]}, 3);
    prefab_teardown();
}
#endif
#endif
