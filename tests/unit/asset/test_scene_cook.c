/*
 * test_scene_cook.c: the `scene` and `prefab` types (DENG-215, SPEC §9.2, §11.6): the TOML
 * importer, the cooked format, the loader and de_prefab_instantiate. CTest names
 * asset_scene.scene.<test> and asset_scene.prefab.<test>.
 *
 * The fixtures are in data/scenes/ (mounted read-only as scn:/; scn:/assets.toml is the manifest).
 * Two reflected component types: THealth { current, max } (version 2, with a migrate hook from
 * version 1 { hp }) and TTarget { target: entity, texture: blob asset, speed }. The importer needs
 * the cooker, so every test that cooks is skipped in builds without it (DE_HAS_COOKER).
 *
 * Cooks go through the runtime pipeline (SOURCE mode, as in a Dev build) or straight through a
 * de_cooker with the runtime's type desc (as `de-cook` does). The cook cache is off: the system's
 * cache directory is on a root that is not mounted, so every load runs the importer.
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_task.h>

#include "asset/asset_internal.h"
#include "asset/resolve_internal.h"
#include "asset/scene/scene_format.h"
#include "asset/slots_internal.h"
#ifndef DE_HAS_COOKER
#define DE_HAS_COOKER 0
#endif
#if DE_HAS_COOKER
#include "cook/lib/de_cooker.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS ((u64)60 * 1000000000u)

/* ---- Component types ----------------------------------------------------------------------- */

typedef struct THealth {
    f32 current;
    f32 max;
} THealth;

typedef struct THealthV1 {
    f32 hp;
} THealthV1;

typedef struct TTarget {
    de_entity target;
    u32 texture; /* a de_blob handle */
    f32 speed;
} TTarget;

static const de_field_info k_health_fields[] = {
    DE_FIELD(THealth, current, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(THealth, max, DE_FK_F32, DE_FIELD_DEFAULT),
};
static const de_field_info k_health_v1_fields[] = {
    DE_FIELD(THealthV1, hp, DE_FK_F32, DE_FIELD_DEFAULT),
};
static const de_field_info k_target_fields[] = {
    DE_FIELD(TTarget, target, DE_FK_ENTITY, DE_FIELD_DEFAULT),
    DE_FIELD_EX(TTarget, texture, DE_FK_ASSET, DE_FIELD_DEFAULT | DE_FIELD_ASSET_REF,
                {0, 0, 0, 0, "blob", 0}),
    DE_FIELD(TTarget, speed, DE_FK_F32, DE_FIELD_DEFAULT),
};

static u32 g_migrations;
static bool g_migrate_drops;

/* THealth v1 { hp } -> v2 { current = hp, max = 100 }. */
static bool health_migrate(void *obj, u32 from_version, const void *old_data,
                           const de_field_info *old_fields, u32 old_field_count) {
    g_migrations++;
    if (g_migrate_drops || from_version != 1)
        return false;
    THealth *h = (THealth *)obj;
    for (u32 i = 0; i < old_field_count; ++i) {
        if (strcmp(old_fields[i].name, "hp") == 0 && old_fields[i].kind == DE_FK_F32 &&
            old_fields[i].size == 4) {
            f32 hp;
            memcpy(&hp, (const u8 *)old_data + old_fields[i].offset, sizeof hp);
            h->current = hp;
            h->max = 100.0f;
        }
    }
    return true;
}

/* THealth version 2 (with migrate) or 1, and TTarget. */
static de_type_registry *make_registry(u32 health_version) {
    de_type_registry *r = NULL;
    ASSERT_EQ(de_type_registry_create(NULL, &r), DE_OK);
    de_type_desc d;
    if (health_version == 1) {
        d = DE_TYPE_DESC(THealthV1, 1, k_health_v1_fields);
        d.name = "THealth";
    } else {
        d = DE_TYPE_DESC(THealth, 2, k_health_fields);
        d.hooks.migrate = health_migrate;
    }
    EXPECT_TRUE(DE_HANDLE_VALID(de_type_register(r, &d)));
    EXPECT_TRUE(DE_HANDLE_VALID(de_type_register(r, &DE_TYPE_DESC(TTarget, 1, k_target_fields))));
    return r;
}

/* ---- Log capture --------------------------------------------------------------------------- */

#define CAP_MAX 64

static struct {
    u32 count;
    char msgs[CAP_MAX][256];
} g_cap;

static void capture_sink(const de_log_record *rec, void *user) {
    (void)user;
    if (rec->level != DE_LOG_WARN)
        return;
    if (g_cap.count < CAP_MAX) {
        size_t n = rec->message.len < 255 ? rec->message.len : 255;
        memcpy(g_cap.msgs[g_cap.count], rec->message.ptr, n);
        g_cap.msgs[g_cap.count][n] = '\0';
    }
    g_cap.count++;
}

#if DE_HAS_COOKER
static bool warned(const char *needle) {
    de_log_flush();
    for (u32 i = 0; i < g_cap.count && i < CAP_MAX; ++i)
        if (strstr(g_cap.msgs[i], needle) != NULL)
            return true;
    return false;
}

static void dump_warnings(void) {
    de_log_flush();
    for (u32 i = 0; i < g_cap.count && i < CAP_MAX; ++i)
        printf("  warn: %s\n", g_cap.msgs[i]);
}
#endif

/* ---- Fixture -------------------------------------------------------------------------------- */

typedef struct scn_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_type_registry *reg; /* THealth v2 + TTarget: the cook registry and the world's */
    de_task_system *tasks;
    de_asset_system *s;
    de_world *w;
    de_component health, target;
} scn_fx;

static void scn_mount(void) {
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("scn", DE_ASSET_TEST_DATA_DIR "/scenes", 0, 0), DE_OK);
        mounted = true;
    }
}

/* An asset system whose scene importer reads `types`. */
static de_asset_system *scn_system(scn_fx *f, de_type_registry *types) {
    de_asset_system_desc d = {.allocator = &f->alloc,
                              .tasks = f->tasks,
                              .io_threads = 1,
                              .cache_dir = "nocache:/decache",
                              .types = types};
    de_asset_system *s = NULL;
    ASSERT_EQ(de_asset_system_create(&d, &s), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(s, "scn:/assets.toml"), DE_OK);
    return s;
}

/* A world over `types` with the ECS built-ins and the components that are asked for. */
static de_world *scn_world(de_type_registry *types, bool health, bool target, de_component *hc,
                           de_component *tc) {
    de_world *w = NULL;
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = types}, &w), DE_OK);
    ASSERT_TRUE(DE_HANDLE_VALID(de_builtin_components(w).transform));
    if (health)
        *hc = de_component_register(w, de_type_find(types, "THealth"), DE_COMP_SERIALIZE);
    if (target)
        *tc = de_component_register(w, de_type_find(types, "TTarget"), DE_COMP_SERIALIZE);
    return w;
}

static void scn_fx_setup(scn_fx *f) {
    memset(&g_cap, 0, sizeof g_cap);
    g_migrations = 0;
    g_migrate_drops = false;
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    de_log_add_sink(capture_sink, NULL);
    scn_mount();
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    f->reg = make_registry(2);
    de_fp_env_init_thread();
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &f->tasks), DE_OK);
    f->s = scn_system(f, f->reg);
    f->w = scn_world(f->reg, true, true, &f->health, &f->target);
}

static void scn_fx_teardown(scn_fx *f) {
    de_world_destroy(f->w);
    de_asset_system_destroy(f->s);
    de_task_system_destroy(f->tasks);
    de_type_registry_destroy(f->reg);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    de_log_shutdown();
}

#if DE_HAS_COOKER
static de_load_state load_wait(de_asset_system *s, const char *id, de_load_handle *out,
                               de_load_info *info_out) {
    de_load_handle h = de_asset_load(s, de_asset_id_from(id), 0);
    de_load_state st = de_asset_wait(s, h, WAIT_NS);
    de_load_info info;
    (void)de_asset_status(s, h, &info);
    if (st != DE_LOAD_READY && info.first_error != DE_OK)
        printf("  %s: %s\n", id, info.first_error_msg);
    if (info_out != NULL)
        *info_out = info;
    *out = h;
    return st;
}

/* Cooks asset `id` of the system's manifest straight through a new cooker that holds the
 * system's own type desc (as `de-cook` does). */
static de_result cook_direct(de_asset_system *s, const char *id, de_cook_result *out) {
    const de__manifest_asset *a = de__resolved_asset(s->resolved, de_asset_id_from(id));
    ASSERT_TRUE(a != NULL);
    de_cooker *c = NULL;
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.verify_determinism = DE_COOK_VERIFY_ON}, &c),
              DE_OK);
    const de_asset_type_desc *td = de__asset_type_get(s, de__asset_type_find(s, a->type));
    ASSERT_TRUE(td != NULL);
    ASSERT_EQ(de_cooker_register_type(c, td), DE_OK);
    de_result r = de_cooker_cook(c, &(de_cook_input){.asset = a, .asset_root = "scn:/assets"}, out);
    de_cooker_destroy(c);
    return r;
}
#endif

#define NO_COOKER_MSG                                                                              \
    "this build does not link the cooker (DE_HAS_COOKER: Debug and Dev with tools)"

/* ---- test_scene_cook_roundtrip -------------------------------------------------------------- */

/* 3 entities (a1 -> b2 -> c3), 2 component types: cooked in SOURCE mode, loaded, instantiated
 * with the same field values and parent links. */
TEST_F(scene, cook_roundtrip, scn_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    de_asset_system *s = f->s;
    de_world *w = f->w;
    de_load_handle h;
    ASSERT_EQ((int)load_wait(s, "levels/level", &h, NULL), (int)DE_LOAD_READY);
    de_prefab p = de_asset_scene(s, de_asset_id_from("levels/level"));
    ASSERT_TRUE(DE_HANDLE_VALID(p));
    de_blob heart = de_asset_blob(s, de_asset_id_from("ui/heart"));
    ASSERT_TRUE(DE_HANDLE_VALID(heart));

    de_entity root = DE_ENTITY_NULL;
    ASSERT_EQ(de_prefab_instantiate(s, w, p, DE_ENTITY_NULL, &root), DE_OK);
    ASSERT_TRUE(de_entity_alive(w, root));
    EXPECT_EQ(de_entity_parent(w, root).bits, 0u);
    const THealth *hr = (const THealth *)de_entity_get_const(w, root, f->health);
    ASSERT_TRUE(hr != NULL);
    EXPECT_FLOAT_EQ(hr->current, 75.5f, 0.0);
    EXPECT_FLOAT_EQ(hr->max, 100.0f, 0.0);
    EXPECT_FALSE(de_entity_has(w, root, f->target));

    de_entity b2 = de_entity_children_first(w, root);
    ASSERT_TRUE(de_entity_alive(w, b2));
    EXPECT_EQ(de_entity_children_next(w, b2).bits, 0u); /* one child */
    de_entity c3 = de_entity_children_first(w, b2);
    ASSERT_TRUE(de_entity_alive(w, c3));
    EXPECT_EQ(de_entity_parent(w, b2).bits, root.bits);
    EXPECT_EQ(de_entity_parent(w, c3).bits, b2.bits);
    EXPECT_EQ(de_entity_children_first(w, c3).bits, 0u);

    const THealth *hb = (const THealth *)de_entity_get_const(w, b2, f->health);
    ASSERT_TRUE(hb != NULL);
    EXPECT_FLOAT_EQ(hb->current, 10.0f, 0.0);
    EXPECT_FLOAT_EQ(hb->max, 20.0f, 0.0);
    const TTarget *tb = (const TTarget *)de_entity_get_const(w, b2, f->target);
    ASSERT_TRUE(tb != NULL);
    EXPECT_EQ(tb->target.bits, c3.bits); /* scene ID 0xc3 -> the entity spawned for it */
    EXPECT_EQ(tb->texture, heart.id);    /* asset ID "ui/heart" -> its typed handle */
    EXPECT_FLOAT_EQ(tb->speed, 2.5f, 0.0);
    EXPECT_FALSE(de_entity_has(w, c3, f->health));
    const TTarget *tc = (const TTarget *)de_entity_get_const(w, c3, f->target);
    ASSERT_TRUE(tc != NULL);
    EXPECT_EQ(tc->target.bits, root.bits);
    EXPECT_EQ(tc->texture, heart.id);
    EXPECT_FLOAT_EQ(tc->speed, -1.25f, 0.0);

    /* The cooked object: 3 entities, 2 types, 4 components, 1 asset reference, 1 root. */
    const de__scene_object *o = (const de__scene_object *)de__asset_object(s, p.id);
    ASSERT_TRUE(o != NULL);
    EXPECT_EQ(o->view.kind, DE__SCENE_KIND_SCENE);
    EXPECT_EQ(o->view.entity_count, 3u);
    EXPECT_EQ(o->view.type_count, 2u);
    EXPECT_EQ(o->view.component_count, 4u);
    EXPECT_EQ(o->view.asset_count, 1u);
    EXPECT_EQ(o->view.root_count, 1u);
    EXPECT_STREQ(de__scene_type_at(&o->view, 0).name, "THealth");
    EXPECT_STREQ(de__scene_type_at(&o->view, 1).name, "TTarget");

    /* A second instance is independent. */
    de_entity root2 = DE_ENTITY_NULL;
    ASSERT_EQ(de_prefab_instantiate(s, w, p, DE_ENTITY_NULL, &root2), DE_OK);
    EXPECT_NE(root2.bits, root.bits);
    de_entity b2b = de_entity_children_first(w, root2);
    const TTarget *t2 = (const TTarget *)de_entity_get_const(w, b2b, f->target);
    ASSERT_TRUE(t2 != NULL);
    EXPECT_EQ(t2->target.bits, de_entity_children_first(w, b2b).bits);
    dump_warnings(); /* flushes the log first: the sink runs on the log thread */
    de_asset_release(s, h);
#endif
}

/* ---- test_scene_cook_deterministic ---------------------------------------------------------- */

TEST_F(scene, cook_deterministic, scn_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    de_cook_result a, b, c;
    /* verify_determinism is on: each cook already ran the importer twice. */
    ASSERT_EQ(cook_direct(f->s, "levels/level", &a), DE_OK);
    ASSERT_EQ(cook_direct(f->s, "levels/level", &b), DE_OK);
    ASSERT_EQ(a.size, b.size);
    EXPECT_MEMEQ(a.data, b.data, (size_t)a.size);
    EXPECT_EQ(a.output_hash, b.output_hash);

    /* Another entity order, key order, table style and number spelling: the same bytes. */
    ASSERT_EQ(cook_direct(f->s, "levels/reordered", &c), DE_OK);
    ASSERT_EQ(a.size, c.size);
    EXPECT_MEMEQ(a.data, c.data, (size_t)a.size);
    EXPECT_EQ(a.dependency_count, c.dependency_count);
    EXPECT_TRUE(warned("unknown component type `__dummy_never_read`"));

    /* The bytes are a valid scene of the documented layout. */
    de__scene_view v;
    char err[192] = "";
    ASSERT_EQ(de__scene_view_parse(a.data, a.size, &v, err, sizeof err), DE_OK);
    EXPECT_EQ(v.entity_count, 3u);
    EXPECT_EQ(de__scene_entity_at(&v, 0).scene_id, 0xa1u);
    EXPECT_EQ(de__scene_entity_at(&v, 1).scene_id, 0xb2u);
    EXPECT_EQ(de__scene_entity_at(&v, 2).scene_id, 0xc3u);
    EXPECT_EQ(de__scene_entity_at(&v, 0).parent, DE__SCENE_NO_PARENT);
    EXPECT_EQ(de__scene_entity_at(&v, 1).parent, 0u);
    EXPECT_EQ(de__scene_entity_at(&v, 2).parent, 1u);
    de_cook_result_free(&a);
    de_cook_result_free(&b);
    de_cook_result_free(&c);
#endif
}

/* ---- test_scene_unknown_component ----------------------------------------------------------- */

TEST_F(scene, unknown_component, scn_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    de_asset_system *s = f->s;
    de_load_handle h;
    ASSERT_EQ((int)load_wait(s, "levels/unknown", &h, NULL), (int)DE_LOAD_READY);
    EXPECT_TRUE(warned("unknown component type `TNope`"));
    EXPECT_TRUE(warned("key `color`"));
    EXPECT_TRUE(warned("`DeChildOf` is skipped"));
    EXPECT_TRUE(warned("top-level key `meta`"));
    EXPECT_TRUE(warned("bogus")); /* the text reader's unknown-field warning */

    /* A world without TTarget: that component is skipped with a warning, the rest spawns. */
    de_component hc = {0}, tc = {0};
    de_world *w = scn_world(f->reg, true, false, &hc, &tc);
    de_entity root = DE_ENTITY_NULL;
    EXPECT_EQ(de_prefab_instantiate(s, w, de_asset_scene(s, de_asset_id_from("levels/unknown")),
                                    DE_ENTITY_NULL, &root),
              DE_OK);
    EXPECT_TRUE(warned("`TTarget` is not a component of the world"));
    const THealth *hr = (const THealth *)de_entity_get_const(w, root, hc);
    ASSERT_TRUE(hr != NULL);
    EXPECT_FLOAT_EQ(hr->current, 1.0f, 0.0);
    EXPECT_FLOAT_EQ(hr->max, 2.0f, 0.0);
    de_entity child = de_entity_children_first(w, root);
    EXPECT_TRUE(de_entity_alive(w, child));
    EXPECT_FALSE(de_entity_has(w, child, hc));
    de_world_destroy(w);

    /* In the fixture's world TTarget is a component: it loads. */
    ASSERT_EQ(de_prefab_instantiate(s, f->w, de_asset_scene(s, de_asset_id_from("levels/unknown")),
                                    DE_ENTITY_NULL, &root),
              DE_OK);
    child = de_entity_children_first(f->w, root);
    const TTarget *t = (const TTarget *)de_entity_get_const(f->w, child, f->target);
    ASSERT_TRUE(t != NULL);
    EXPECT_EQ(t->target.bits, root.bits);
    EXPECT_FLOAT_EQ(t->speed, 4.0f, 0.0);
    EXPECT_EQ(t->texture, 0u);
    de_asset_release(s, h);
#endif
}

/* ---- test_scene_migrate_on_instantiate ------------------------------------------------------ */

TEST_F(scene, migrate_on_instantiate, scn_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    /* Cooked where THealth is version 1 ({ hp }). */
    de_type_registry *v1 = make_registry(1);
    de_asset_system *s = scn_system(f, v1);
    de_load_handle h;
    ASSERT_EQ((int)load_wait(s, "levels/migrate", &h, NULL), (int)DE_LOAD_READY);
    de_prefab p = de_asset_scene(s, de_asset_id_from("levels/migrate"));
    const de__scene_object *o = (const de__scene_object *)de__asset_object(s, p.id);
    ASSERT_TRUE(o != NULL);
    EXPECT_EQ(de__scene_type_at(&o->view, 0).version, 1u);

    /* Instantiated where THealth is version 2 with a migrate hook. */
    de_entity root = DE_ENTITY_NULL;
    ASSERT_EQ(de_prefab_instantiate(s, f->w, p, DE_ENTITY_NULL, &root), DE_OK);
    EXPECT_EQ(g_migrations, 1u);
    const THealth *hr = (const THealth *)de_entity_get_const(f->w, root, f->health);
    ASSERT_TRUE(hr != NULL);
    EXPECT_FLOAT_EQ(hr->current, 42.0f, 0.0);
    EXPECT_FLOAT_EQ(hr->max, 100.0f, 0.0);

    /* A hook that drops the object: the component is skipped with a warning. */
    g_migrate_drops = true;
    ASSERT_EQ(de_prefab_instantiate(s, f->w, p, DE_ENTITY_NULL, &root), DE_OK);
    EXPECT_EQ(g_migrations, 2u);
    EXPECT_TRUE(de_entity_alive(f->w, root));
    EXPECT_FALSE(de_entity_has(f->w, root, f->health));
    EXPECT_TRUE(warned("component `THealth` dropped"));

    de_asset_release(s, h);
    de_asset_system_destroy(s);
    de_type_registry_destroy(v1);
#endif
}

/* ---- test_scene_asset_ref_dependency -------------------------------------------------------- */

TEST_F(scene, asset_ref_dependency, scn_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    de_asset_system *s = f->s;
    /* The importer declares the referenced asset as a cook dependency. */
    de_cook_result r;
    ASSERT_EQ(cook_direct(s, "levels/level", &r), DE_OK);
    ASSERT_EQ(r.dependency_count, 1u);
    EXPECT_EQ(r.dependencies[0].hash, de_asset_id_from("ui/heart").hash);
    de_cook_result_free(&r);
    ASSERT_EQ(cook_direct(s, "levels/unknown", &r), DE_OK);
    EXPECT_EQ(r.dependency_count, 0u);
    de_cook_result_free(&r);

    /* At run time it loads with the scene: READY in the same update, and held by it. */
    de_asset_id heart = de_asset_id_from("ui/heart");
    EXPECT_EQ((int)de_asset_status_of(s, heart), (int)DE_LOAD_INVALID);
    de_load_handle h;
    ASSERT_EQ((int)load_wait(s, "props/crate", &h, NULL), (int)DE_LOAD_READY);
    EXPECT_EQ((int)de_asset_status_of(s, heart), (int)DE_LOAD_READY);
    const de__slot *sl = de__slot_get(s, de_asset_prefab(s, de_asset_id_from("props/crate")).id);
    ASSERT_TRUE(sl != NULL);
    ASSERT_EQ(sl->dep_count, 1u);
    EXPECT_EQ(sl->deps[0].hash, heart.hash);
    u64 n = 0;
    EXPECT_TRUE(de_blob_data(s, de_asset_blob(s, heart), &n) != NULL);
    EXPECT_EQ(n, 270u);
    de_asset_release(s, h);
#endif
}

/* ---- test_prefab_instantiate_parent --------------------------------------------------------- */

TEST_F(prefab, instantiate_parent, scn_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    de_asset_system *s = f->s;
    de_world *w = f->w;
    de_prefab p = de_asset_prefab(s, de_asset_id_from("props/crate"));
    ASSERT_TRUE(DE_HANDLE_VALID(p));
    de_entity root = DE_ENTITY_NULL;
    /* Not loaded yet. */
    EXPECT_EQ(de_prefab_instantiate(s, w, p, DE_ENTITY_NULL, &root), DE_ERR_NOT_FOUND);
    de_load_handle h;
    ASSERT_EQ((int)load_wait(s, "props/crate", &h, NULL), (int)DE_LOAD_READY);

    de_entity holder = de_entity_spawn(w);
    de_entity r1 = DE_ENTITY_NULL, r2 = DE_ENTITY_NULL;
    ASSERT_EQ(de_prefab_instantiate(s, w, p, holder, &r1), DE_OK);
    ASSERT_EQ(de_prefab_instantiate(s, w, p, holder, &r2), DE_OK);
    EXPECT_NE(r1.bits, r2.bits);
    EXPECT_EQ(de_entity_parent(w, r1).bits, holder.bits);
    EXPECT_EQ(de_entity_parent(w, r2).bits, holder.bits);
    EXPECT_EQ(de_entity_children_first(w, holder).bits, r1.bits); /* instantiation order */
    EXPECT_EQ(de_entity_children_next(w, r1).bits, r2.bits);
    EXPECT_EQ(de_entity_children_count(w, holder), 2u);

    /* Each instance's child targets its own root. */
    de_entity c1 = de_entity_children_first(w, r1), c2 = de_entity_children_first(w, r2);
    const TTarget *t1 = (const TTarget *)de_entity_get_const(w, c1, f->target);
    const TTarget *t2 = (const TTarget *)de_entity_get_const(w, c2, f->target);
    ASSERT_TRUE(t1 != NULL && t2 != NULL);
    EXPECT_EQ(t1->target.bits, r1.bits);
    EXPECT_EQ(t2->target.bits, r2.bits);
    const THealth *hp = (const THealth *)de_entity_get_const(w, r2, f->health);
    ASSERT_TRUE(hp != NULL);
    EXPECT_FLOAT_EQ(hp->current, 50.0f, 0.0);

    /* Recursive despawn of the holder removes both instances. */
    de_entity_despawn_recursive(w, holder);
    EXPECT_FALSE(de_entity_alive(w, r1));
    EXPECT_FALSE(de_entity_alive(w, c2));

    /* Refusals: a dead parent, a handle of another type, NULL arguments. */
    de_error_clear();
    EXPECT_EQ(de_prefab_instantiate(s, w, p, holder, &root), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(root.bits, 0u);
    de_blob b = de_asset_blob(s, de_asset_id_from("ui/heart"));
    EXPECT_EQ(de_prefab_instantiate(s, w, (de_prefab){b.id}, DE_ENTITY_NULL, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_prefab_instantiate(s, w, (de_prefab){0}, DE_ENTITY_NULL, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_prefab_instantiate(NULL, w, p, DE_ENTITY_NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_prefab_instantiate(s, NULL, p, DE_ENTITY_NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    /* de_asset_scene wants a scene, de_asset_prefab a prefab. */
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_scene(s, de_asset_id_from("props/crate"))));
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_prefab(s, de_asset_id_from("levels/level"))));
    de_error_clear();
    de_asset_release(s, h);
#endif
}

/* ---- Importer refusals and the format check -------------------------------------------------- */

TEST_F(scene, import_errors, scn_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    de_asset_system *s = f->s;
    de_load_handle h;
    de_load_info info;
    EXPECT_EQ((int)load_wait(s, "props/two_roots", &h, &info), (int)DE_LOAD_FAILED);
    EXPECT_TRUE(strstr(info.first_error_msg, "exactly one root") != NULL);
    de_asset_release(s, h);
    EXPECT_EQ((int)load_wait(s, "levels/bad_ref", &h, &info), (int)DE_LOAD_FAILED);
    EXPECT_TRUE(strstr(info.first_error_msg, "not an entity of the file") != NULL);
    de_asset_release(s, h);

    /* No type registry: scenes do not cook. */
    de_asset_system *none = scn_system(f, NULL);
    EXPECT_EQ((int)load_wait(none, "levels/level", &h, &info), (int)DE_LOAD_FAILED);
    EXPECT_TRUE(strstr(info.first_error_msg, "no type registry") != NULL);
    de_asset_release(none, h);
    de_asset_system_destroy(none);
    de_error_clear();
#endif
}

TEST_F(scene, format_rejects_damage, scn_fx) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    de_cook_result r;
    ASSERT_EQ(cook_direct(f->s, "levels/level", &r), DE_OK);
    const u8 *good = (const u8 *)r.data;
    size_t n = (size_t)r.size;
    de__scene_view v;
    ASSERT_EQ(de__scene_view_parse(good, n, &v, NULL, 0), DE_OK);
    const u32 good_types = v.type_count;
    const size_t tables_end = (size_t)(v.data - v.bytes);

    /* Every truncation and an extension are refused. */
    u8 *buf = (u8 *)malloc(n + 8u);
    ASSERT_TRUE(buf != NULL);
    for (size_t k = 0; k < n; ++k) {
        memcpy(buf, good, k);
        if (de__scene_view_parse(buf, k, &v, NULL, 0) == DE_OK)
            FAIL("a truncated scene parsed");
    }
    memcpy(buf, good, n);
    buf[n] = 0;
    EXPECT_NE(de__scene_view_parse(buf, n + 1u, &v, NULL, 0), DE_OK);

    /* Flipping any byte before the data section never reads out of bounds (ASan). */
    for (size_t k = 0; k < tables_end; ++k) {
        memcpy(buf, good, n);
        buf[k] ^= 0x5a;
        de__scene_view bad;
        (void)de__scene_view_parse(buf, n, &bad, NULL, 0);
    }
    char err[192] = "";
    memcpy(buf, good, n);
    buf[0] = 'X';
    EXPECT_EQ(de__scene_view_parse(buf, n, &v, err, sizeof err), DE_ERR_PARSE);
    memcpy(buf, good, n);
    buf[4] = 9; /* format version */
    EXPECT_EQ(de__scene_view_parse(buf, n, &v, err, sizeof err), DE_ERR_VERSION_MISMATCH);
    memcpy(buf, good, n);
    buf[8] = 1; /* kind prefab: one root, so it parses; the loader checks the kind */
    EXPECT_EQ(de__scene_view_parse(buf, n, &v, err, sizeof err), DE_OK);
    memcpy(buf, good, n);
    buf[40] = 2; /* root count */
    EXPECT_EQ(de__scene_view_parse(buf, n, &v, err, sizeof err), DE_ERR_PARSE);
    /* A parent cycle: a1's parent becomes c3 (index 2), while c3 -> b2 -> a1. */
    memcpy(buf, good, n);
    size_t ent0 = DE__SCENE_HEADER_SIZE + (size_t)good_types * DE__SCENE_TYPE_SIZE;
    de__scene_put_u32(buf + ent0 + 8, 2u);
    de__scene_put_u32(buf + 40, 0u);
    EXPECT_EQ(de__scene_view_parse(buf, n, &v, err, sizeof err), DE_ERR_PARSE);
    EXPECT_TRUE(strstr(err, "cycle") != NULL);
    free(buf);
    de_cook_result_free(&r);
#endif
}
