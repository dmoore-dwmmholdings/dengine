#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "host/editor/editor_gizmo.h"
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include <dengine/de_ecs.h>
#include <dengine/de_math.h>
#include <stdio.h>
#include <stdlib.h>

/* DENG-570 (E12; DIDE-R147, DIDE-R148): scene.serialize writes deterministic TOML scene text,
 * one table per entity keyed by scene ID (SPEC §9.2, DIDE Q13), and scene.reloadFromText reads
 * it back. Actual handlers, no transport. */

void de__ed_session_shutdown(void);

typedef struct text_fx {
    de_engine engine;
    de__host host;
    de__editor ed;
    de_builtin_component_ids ids;
    de_component health;
    unsigned char arena[65536];
    char note[32768];
} text_fx;

typedef struct SvcHealth {
    f32 current, max;
} SvcHealth;
static const de_field_info svc_health_fields[] = {
    DE_FIELD(SvcHealth, current, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(SvcHealth, max, DE_FK_F32, DE_FIELD_DEFAULT),
};

static void text_fx_setup(text_fx *f) {
    memset(f, 0, sizeof *f);
    ASSERT_EQ(de_log_init(&(de_log_desc){.no_console_sink = true}), DE_OK);
    ASSERT_EQ(de_type_registry_create(NULL, &f->engine.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->engine.types}, &f->engine.world), DE_OK);
    f->ids = de_builtin_components(f->engine.world);
    ASSERT_TRUE(DE_HANDLE_VALID(
        de_type_register(f->engine.types, &DE_TYPE_DESC(SvcHealth, 1, svc_health_fields))));
    f->health = de_component_register(
        f->engine.world, de_type_find(f->engine.types, "SvcHealth"), DE_COMP_SERIALIZE);
    ASSERT_TRUE(DE_HANDLE_VALID(f->health));
    f->ed.e = &f->engine;
    f->ed.h = &f->host;
    f->ed.mode = DE_EP_WORLD_MODE_EDIT;
    f->ed.arena = f->arena;
    f->ed.arena_cap = sizeof f->arena;
    f->ed.note = f->note;
    f->ed.note_cap = sizeof f->note;
    de__ed_scene_register(&f->ed);
}
static void text_fx_teardown(text_fx *f) {
    for (u32 i = 0; i < f->ed.reset_count; ++i)
        f->ed.resets[i](&f->ed);
    de__ed_session_shutdown();
    de_world_destroy(f->engine.world);
    de_type_registry_destroy(f->engine.types);
    de_log_shutdown();
}

/* ---------- Requests ---------- */

typedef struct reply {
    mpack_tree_t tree;
    mpack_node_t root;
    char buffer[16384];
} reply;

static void call(text_fx *f, de_ep_method method, const char *params, size_t size, reply *r) {
    const de_ep_request req = {.id = 570,
                               .method = de_ep_cstr(de_ep_method_name(method)),
                               .params = (de_ep_raw){params, size}};
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
}
static int code_of(reply *r) {
    if (!mpack_node_map_contains_cstr(r->root, "error"))
        return 0;
    return (int)mpack_node_i32(mpack_node_map_cstr(mpack_node_map_cstr(r->root, "error"), "code"));
}
static void done(reply *r) {
    EXPECT_EQ(mpack_tree_destroy(&r->tree), mpack_ok);
}

static char g_text[16384];

/* scene.serialize of `scene` into g_text. */
static const char *serialize(text_fx *f, u64 scene) {
    char p[32];
    mpack_writer_t w;
    mpack_writer_init(&w, p, sizeof p);
    const de_ep_scene_params q = {scene};
    de_ep_write_scene_params(&w, &q);
    const size_t n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    reply r;
    call(f, DE_EP_METHOD_SCENE_SERIALIZE, p, n, &r);
    ASSERT_EQ(code_of(&r), 0);
    const mpack_node_t t = mpack_node_map_cstr(mpack_node_map_cstr(r.root, "result"), "text");
    const size_t len = mpack_node_strlen(t);
    ASSERT_TRUE(len < sizeof g_text);
    memcpy(g_text, mpack_node_str(t), len);
    g_text[len] = '\0';
    done(&r);
    return g_text;
}

/* scene.reloadFromText; the reply stays open in `r` for the caller to read. */
static int reload(text_fx *f, u64 scene, const char *text, reply *r) {
    static char p[16384];
    mpack_writer_t w;
    mpack_writer_init(&w, p, sizeof p);
    const de_ep_scene_reload_params q = {scene, de_ep_cstr(text)};
    de_ep_write_scene_reload_params(&w, &q);
    const size_t n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    call(f, DE_EP_METHOD_SCENE_RELOAD_FROM_TEXT, p, n, r);
    return code_of(r);
}
static int reload_code(text_fx *f, u64 scene, const char *text) {
    reply r;
    const int code = reload(f, scene, text, &r);
    done(&r);
    return code;
}

/* ---------- Scene content ---------- */

static de_entity spawn(text_fx *f, u64 sid, bool transform, f32 x, bool health, f32 hp) {
    const de_entity e = de_entity_spawn(f->engine.world);
    if (transform) {
        DeTransform *t = de_entity_add(f->engine.world, e, f->ids.transform);
        ASSERT_TRUE(t != NULL);
        t->position.x = x;
        t->position.y = 0.5;
    }
    if (health) {
        SvcHealth *h = de_entity_add(f->engine.world, e, f->health);
        ASSERT_TRUE(h != NULL);
        h->current = hp;
        h->max = 100;
    }
    ASSERT_TRUE(de__ed_scene_member_set(e.bits, 1, sid));
    return e;
}

/* Three entities: 0x20 a child of 0x10, and 0x05 on its own; spawned in `order`. */
static void build(text_fx *f, const int order[3]) {
    de_entity e10 = DE_ENTITY_NULL, e20 = DE_ENTITY_NULL;
    for (int i = 0; i < 3; ++i) {
        if (order[i] == 0x10)
            e10 = spawn(f, 0x10, true, 1.25f, true, 75.5f);
        else if (order[i] == 0x20)
            e20 = spawn(f, 0x20, true, -2.0f, false, 0);
        else
            (void)spawn(f, 0x05, false, 0, true, 10);
    }
    ASSERT_EQ(de_entity_set_parent(f->engine.world, e20, e10), DE_OK);
}

static u32 live_count(text_fx *f) {
    de_world *w = f->engine.world;
    const de_query q = de_query_create(w, &(de_query_desc){0});
    const u32 n = de_query_count(w, q);
    de_query_destroy(w, q);
    return n;
}

typedef struct find_sid {
    u64 sid;
    de_entity found;
} find_sid;
static void find_sid_chunk(const de_query_chunk *ch, void *user) {
    find_sid *fs = user;
    for (u32 i = 0; i < ch->count; ++i) {
        u64 scene = 0, sid = 0;
        if (de__ed_scene_member(ch->entities[i].bits, &scene, &sid) && sid == fs->sid)
            fs->found = ch->entities[i];
    }
}
/* The entity whose scene ID is `sid`. */
static de_entity by_sid(text_fx *f, u64 sid) {
    de_world *w = f->engine.world;
    find_sid fs = {sid, DE_ENTITY_NULL};
    const de_query q = de_query_create(w, &(de_query_desc){0});
    de_query_each(w, q, find_sid_chunk, &fs);
    de_query_destroy(w, q);
    return fs.found;
}

static bool read_file(const char *path, char *buf, size_t cap) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
        return false;
    const size_t n = fread(buf, 1, cap - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return n < cap - 1;
}

/* ---------- Tests ---------- */

/* DIDE-R147.AC1: serialize, reload, serialize again gives the same bytes. */
TEST_F(scene_text, round_trip_is_byte_identical, text_fx) {
    build(f, (const int[3]){0x10, 0x20, 0x05});
    static char first[16384];
    snprintf(first, sizeof first, "%s", serialize(f, 1));
    ASSERT_TRUE(first[0] != '\0');
    ASSERT_EQ(reload_code(f, 1, first), 0);
    EXPECT_EQ(live_count(f), 3u);
    EXPECT_STREQ(serialize(f, 1), first);
    /* And again: reloading what it wrote is a fixed point. */
    ASSERT_EQ(reload_code(f, 1, g_text), 0);
    EXPECT_STREQ(serialize(f, 1), first);

    /* Hand-written keyed text in another order and spelling settles on the same bytes. */
    const char *authored = "[entity.0x5.SvcHealth]\nmax = 100.0\ncurrent = 10.0\n\n"
                           "[entity.0x20]\nparent = \"0x10\"\n[entity.0x20.DeTransform]\n"
                           "position = [-2.0, 0.5, 0.0]\n\n"
                           "[entity.16]\n[entity.16.SvcHealth]\ncurrent = 75.5\nmax = 100\n"
                           "[entity.16.DeTransform]\nposition = [1.25, 0.5, 0.0]\n";
    ASSERT_EQ(reload_code(f, 1, authored), 0);
    EXPECT_EQ(live_count(f), 3u);
    static char again[16384];
    snprintf(again, sizeof again, "%s", serialize(f, 1));
    ASSERT_EQ(reload_code(f, 1, again), 0);
    EXPECT_STREQ(serialize(f, 1), again);
}

/* Entities by scene ID, components by type name, whatever the spawn and add order. */
TEST_F(scene_text, output_order_is_stable, text_fx) {
    build(f, (const int[3]){0x20, 0x05, 0x10});
    const char *t = serialize(f, 1);
    const char *a = strstr(t, "[entity.0x0000000000000005]");
    const char *b = strstr(t, "[entity.0x0000000000000010]");
    const char *c = strstr(t, "[entity.0x0000000000000020]");
    ASSERT_TRUE(a != NULL && b != NULL && c != NULL);
    EXPECT_TRUE(a < b && b < c);
    const char *tr = strstr(t, "[entity.0x0000000000000010.DeTransform]");
    const char *hp = strstr(t, "[entity.0x0000000000000010.SvcHealth]");
    ASSERT_TRUE(tr != NULL && hp != NULL);
    EXPECT_TRUE(tr < hp);
    static char other[16384];
    snprintf(other, sizeof other, "%s", t);

    /* The same content spawned in another order in a fresh service gives the same bytes. */
    text_fx_teardown(f);
    text_fx_setup(f);
    build(f, (const int[3]){0x05, 0x10, 0x20});
    EXPECT_STREQ(serialize(f, 1), other);
}

/* SPEC §9.2: one table per entity keyed by scene ID; no `[[entity]]` array, no `id` key. */
TEST_F(scene_text, entity_tables_are_keyed_by_scene_id, text_fx) {
    build(f, (const int[3]){0x10, 0x20, 0x05});
    const char *t = serialize(f, 1);
    EXPECT_TRUE(strstr(t, "[[entity]]") == NULL);
    EXPECT_TRUE(strstr(t, "\nid = ") == NULL && strncmp(t, "id = ", 5) != 0);
    EXPECT_TRUE(strstr(t, "[entity.0x0000000000000020]\nparent = \"0x0000000000000010\"\n") !=
                NULL);
    EXPECT_TRUE(strstr(t, "[entity.0x0000000000000005]\n[entity.0x0000000000000005.SvcHealth]\n") !=
                NULL);
}

/* Authored `[[entity]]` text (the DENG-215 layout) still loads, and saves keyed. */
TEST_F(scene_text, legacy_entity_arrays_still_load, text_fx) {
    const char *legacy = "[[entity]]\nid = \"0x10\"\n[entity.SvcHealth]\ncurrent = 75.5\nmax = "
                         "100.0\n\n[[entity]]\nid = 32\nparent = \"0x10\"\n[entity.SvcHealth]\n"
                         "current = 1.0\nmax = 2.0\n";
    ASSERT_EQ(reload_code(f, 1, legacy), 0);
    EXPECT_EQ(live_count(f), 2u);
    const char *t = serialize(f, 1);
    EXPECT_TRUE(strstr(t, "[entity.0x0000000000000010.SvcHealth]\ncurrent = 75.5\n") != NULL);
    EXPECT_TRUE(strstr(t, "[entity.0x0000000000000020]\nparent = \"0x0000000000000010\"\n") !=
                NULL);
}

/* DIDE-R147.AC2: moving one entity changes only that entity's transform lines. */
TEST_F(scene_text, moving_one_entity_changes_only_its_transform_lines, text_fx) {
    build(f, (const int[3]){0x10, 0x20, 0x05});
    static char before[16384];
    snprintf(before, sizeof before, "%s", serialize(f, 1));
    const de_entity moved = by_sid(f, 0x20);
    de_world *w = f->engine.world;
    ASSERT_TRUE(moved.bits != 0);
    DeTransform *t = de_entity_get(w, moved, f->ids.transform);
    ASSERT_TRUE(t != NULL);
    t->position.x = 7.0;
    const char *after = serialize(f, 1);
    /* Line by line: same count, and only the 0x20 transform's position line differs. */
    u32 diffs = 0;
    const char *a = before, *b = after;
    char section[96] = "";
    while (*a != '\0' && *b != '\0') {
        const char *ae = strchr(a, '\n'), *be = strchr(b, '\n');
        ASSERT_TRUE(ae != NULL && be != NULL);
        if (*a == '[')
            snprintf(section, sizeof section, "%.*s", (int)(ae - a), a);
        if ((ae - a) != (be - b) || memcmp(a, b, (size_t)(ae - a)) != 0) {
            ++diffs;
            EXPECT_STREQ(section, "[entity.0x0000000000000020.DeTransform]");
            EXPECT_TRUE(strncmp(a, "position = ", 11) == 0);
        }
        a = ae + 1;
        b = be + 1;
    }
    EXPECT_TRUE(*a == '\0' && *b == '\0');
    EXPECT_EQ(diffs, 1u);
}

/* DIDE-R148: after a reload the selection names the new entities with the same scene IDs; a
 * selected entity whose scene ID left the text drops out. */
TEST_F(scene_text, reload_keeps_the_selection_by_scene_id, text_fx) {
    build(f, (const int[3]){0x10, 0x20, 0x05});
    const u64 picked[2] = {by_sid(f, 0x10).bits, by_sid(f, 0x05).bits};
    de__ed_gizmo_set_selection(picked, 2);
    static char text[16384];
    snprintf(text, sizeof text, "%s", serialize(f, 1));
    ASSERT_EQ(reload_code(f, 1, text), 0);
    u64 sel[4];
    ASSERT_EQ(de__ed_gizmo_selection(sel, 4), 2u);
    EXPECT_EQ(sel[0], by_sid(f, 0x10).bits);
    EXPECT_EQ(sel[1], by_sid(f, 0x05).bits);
    EXPECT_NE(sel[0], picked[0]);
    /* Without 0x05 in the text, only 0x10 stays selected. */
    ASSERT_EQ(reload_code(f, 1, "[entity.0x10]\n[entity.0x20]\nparent = \"0x10\"\n"), 0);
    ASSERT_EQ(de__ed_gizmo_selection(sel, 4), 1u);
    EXPECT_EQ(sel[0], by_sid(f, 0x10).bits);
    de__ed_gizmo_set_selection(NULL, 0);
}

/* The `data` of an error reply, which must be a map. */
static mpack_node_t error_data(reply *r) {
    const mpack_node_t e = mpack_node_map_cstr(r->root, "error");
    EXPECT_TRUE(mpack_node_map_contains_cstr(e, "message"));
    EXPECT_TRUE(mpack_node_map_contains_cstr(e, "data"));
    return mpack_node_map_cstr(e, "data");
}
static bool data_str_is(mpack_node_t d, const char *key, const char *want) {
    if (!mpack_node_map_contains_cstr(d, key))
        return false;
    const mpack_node_t v = mpack_node_map_cstr(d, key);
    return mpack_node_strlen(v) == strlen(want) &&
           memcmp(mpack_node_str(v), want, strlen(want)) == 0;
}
static i64 data_line(mpack_node_t d) {
    return mpack_node_map_contains_cstr(d, "line") ? mpack_node_i64(mpack_node_map_cstr(d, "line"))
                                                   : -1;
}

/* Bad text gets `invalid_params` with data { line, entity, component } where they are known,
 * and the scene stays exactly as it was. */
TEST_F(scene_text, reload_errors_are_structured_and_change_nothing, text_fx) {
    build(f, (const int[3]){0x10, 0x20, 0x05});
    static char before[16384];
    snprintf(before, sizeof before, "%s", serialize(f, 1));
    reply r;

    /* TOML syntax. */
    ASSERT_EQ(reload(f, 1, "[entity.0x01]\nparent = \n", &r), DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(data_line(error_data(&r)), 2);
    EXPECT_TRUE(data_str_is(error_data(&r), "reason", "syntax"));
    done(&r);

    /* A value the field's type can't hold. */
    ASSERT_EQ(reload(f, 1,
                     "[entity.0x01]\n[entity.0x01.SvcHealth]\ncurrent = 1.0\n\n[entity.0x02]\n"
                     "[entity.0x02.SvcHealth]\ncurrent = \"lots\"\n",
                     &r),
              DE_EP_ERR_INVALID_PARAMS);
    EXPECT_TRUE(data_str_is(error_data(&r), "entity", "0x0000000000000002"));
    EXPECT_TRUE(data_str_is(error_data(&r), "component", "SvcHealth"));
    EXPECT_TRUE(data_str_is(error_data(&r), "reason", "value"));
    EXPECT_EQ(data_line(error_data(&r)), 7);
    done(&r);

    /* A parent that is not an entity of the text. */
    ASSERT_EQ(reload(f, 1, "[entity.0x01]\nparent = \"0x99\"\n", &r), DE_EP_ERR_INVALID_PARAMS);
    EXPECT_TRUE(data_str_is(error_data(&r), "entity", "0x0000000000000001"));
    EXPECT_TRUE(data_str_is(error_data(&r), "reason", "parent"));
    EXPECT_EQ(data_line(error_data(&r)), 2);
    done(&r);

    /* Two spellings of one scene ID. */
    ASSERT_EQ(reload(f, 1, "[entity.0x7]\n\n[entity.0x0000000000000007]\n", &r),
              DE_EP_ERR_INVALID_PARAMS);
    EXPECT_TRUE(data_str_is(error_data(&r), "entity", "0x0000000000000007"));
    EXPECT_TRUE(data_str_is(error_data(&r), "reason", "duplicate"));
    done(&r);

    /* Keys that are not scene IDs, and entities that are not tables. */
    ASSERT_EQ(reload(f, 1, "[entity.hero]\n", &r), DE_EP_ERR_INVALID_PARAMS);
    EXPECT_TRUE(data_str_is(error_data(&r), "reason", "id"));
    EXPECT_EQ(data_line(error_data(&r)), 1);
    done(&r);
    EXPECT_EQ(reload_code(f, 1, "[entity.0x0]\n"), DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(reload_code(f, 1, "entity = 3\n"), DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(reload_code(f, 1, "[entity]\n0x01 = 3\n"), DE_EP_ERR_INVALID_PARAMS);
    /* An `id` in a keyed table would say the ID twice. */
    EXPECT_EQ(reload_code(f, 1, "[entity.0x01]\nid = \"0x02\"\n"), DE_EP_ERR_INVALID_PARAMS);

    EXPECT_EQ(live_count(f), 3u);
    EXPECT_STREQ(serialize(f, 1), before);
}

TEST_F(scene_text, scene_methods_check_the_scene_and_the_mode, text_fx) {
    char p[32];
    mpack_writer_t w;
    mpack_writer_init(&w, p, sizeof p);
    const de_ep_scene_params q = {77};
    de_ep_write_scene_params(&w, &q);
    const size_t n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    reply r;
    call(f, DE_EP_METHOD_SCENE_SERIALIZE, p, n, &r);
    EXPECT_EQ(code_of(&r), DE_EP_ERR_NOT_FOUND);
    done(&r);
    EXPECT_EQ(reload_code(f, 77, ""), DE_EP_ERR_NOT_FOUND);
    f->ed.mode = DE_EP_WORLD_MODE_PLAY;
    EXPECT_EQ(reload_code(f, 1, ""), DE_EP_ERR_WRONG_MODE);
    f->ed.mode = DE_EP_WORLD_MODE_EDIT;
    /* Empty text is an empty scene. */
    build(f, (const int[3]){0x10, 0x20, 0x05});
    EXPECT_EQ(reload_code(f, 1, ""), 0);
    EXPECT_EQ(live_count(f), 0u);
    EXPECT_STREQ(serialize(f, 1), "");
}

/* The determinism golden: this content always saves as these bytes, and the golden text reloads
 * to itself. A change of the format must update the golden on purpose. */
TEST_F(scene_text, determinism_golden, text_fx) {
    static char golden[16384];
    build(f, (const int[3]){0x20, 0x10, 0x05});
    const char *regen = getenv("DE_REGEN_GOLDEN"); /* DE_REGEN_GOLDEN=1 rewrites it */
    if (regen != NULL && strcmp(regen, "1") == 0) {
        FILE *fp = fopen(DE_SCENE_TEXT_GOLDEN, "wb");
        ASSERT_TRUE(fp != NULL);
        const char *t = serialize(f, 1);
        fwrite(t, 1, strlen(t), fp);
        fclose(fp);
    }
    ASSERT_TRUE(read_file(DE_SCENE_TEXT_GOLDEN, golden, sizeof golden));
    EXPECT_STREQ(serialize(f, 1), golden);
    ASSERT_EQ(reload_code(f, 1, golden), 0);
    EXPECT_STREQ(serialize(f, 1), golden);
}
#endif
