/*
 * test_hot_reload.c: game library hot reload (DENG-93, SPEC §10.1): src/host/hot_reload.c and
 * the ECS hooks under it (src/ecs/hot_reload.c).
 *
 * The games are the reload_game_v1..v5 fixtures (fixtures/reload_game_v1.c has the list). They
 * call the engine, so this executable links the whole engine and exports it, as `dengine` does
 * (tests/unit/host/CMakeLists.txt). Each test copies a fixture to a "watched" library path in its
 * own directory, attaches it and starts the game as de_host_run does. A reload is triggered the
 * way a rebuild triggers it: the next fixture is copied over the watched path. Most tests then
 * call de__hot_reload_request and poll, so they do not depend on file-watch timing; one test
 * waits for the real watch.
 *
 * Built where de_add_game() builds libraries and the engine is static (not Shipping, not Windows
 * without DE_BUILD_SHARED); the tests skip in Release, where hot reload is compiled out.
 */
#define _CRT_SECURE_NO_WARNINGS /* fopen */
#include "de_test.h"

#include "host/host_internal.h"

#include "ecs/ecs_internal.h"
#include "ecs/hot_reload_internal.h"

#include "reload_game.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h> /* _getpid */
#define hr_getpid() ((unsigned long)_getpid())
#else
#include <dlfcn.h>
#include <unistd.h> /* getpid */
#define hr_getpid() ((unsigned long)getpid())
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_HOT_RELOAD_ASAN 1
#endif
#endif
#if defined(__APPLE__) && defined(TEST_HOT_RELOAD_ASAN)
/* AddressSanitizer on Apple platforms never unregisters the globals of a library that dlclose
 * unloads (dyld runs no module destructor for it: report_globals=2 prints no "Removed" line), so
 * their redzones stay poisoned. This test unloads libraries over and over, and the next library
 * (or a mach message buffer of FSEvents) mapped at a reused address then reads memory that is
 * still marked as a redzone: false global-buffer-overflow reports, and a crash when the report
 * describes the stale global. Global checks are off for this executable on Apple platforms
 * only; heap and stack checks stay on, and the Linux ASan job checks globals too. */
const char *__asan_default_options(void);
const char *__asan_default_options(void) {
    return "report_globals=0";
}
#endif

#ifndef DE_HOST_TEST_TMP_DIR
#error "DE_HOST_TEST_TMP_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif
#ifndef DE_HOST_RELOAD_V1
#error "DE_HOST_RELOAD_V1..V5 must be defined by tests/unit/host/CMakeLists.txt"
#endif

/* DeTestCounter as versions 2, 4 and 5 lay it out. */
typedef struct counter_v2 {
    i32 value;
    f32 scale;
} counter_v2;

/* ---- Files ---------------------------------------------------------------------------------- */

/* Copies native file `from` over native file `to` (in place, as a linker writes it). */
static void copy_native(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    ASSERT_TRUE(in != NULL);
    FILE *out = fopen(to, "wb");
    if (out == NULL)
        fclose(in);
    ASSERT_TRUE(out != NULL);
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    fclose(in);
    ok = fclose(out) == 0 && ok;
    ASSERT_TRUE(ok);
}

/* True while the library at native `path` is loaded in this process. */
static bool lib_loaded(const char *path) {
#if defined(_WIN32)
    (void)path;
    return false;
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    if (h == NULL)
        return false;
    dlclose(h);
    return true;
#endif
}

static void rm_tree(const char *vpath);

static bool rm_entry(const char *name, bool is_dir, void *user) {
    char p[DE_FS_PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", (const char *)user, name);
    if (is_dir)
        rm_tree(p);
    else
        (void)de_file_delete(p);
    return true;
}

/* Deletes the directory `vpath` with everything in it (errors ignored). */
static void rm_tree(const char *vpath) {
    (void)de_dir_list(vpath, rm_entry, (void *)vpath);
    (void)de_file_delete(vpath);
    de_error_clear();
}

/* ---- Fixture -------------------------------------------------------------------------------- */

typedef struct hr_fx {
    de__host h;
    char run[64];                      /* this test's directory under tmp:/ */
    char watched[DE_FS_PATH_MAX + 64]; /* native path of the library the "build" writes */
    bool tmp_mounted, fs, log, started;
} hr_fx;

static u32 g_run_counter;

/* Mounts tmp:/, creates this test's directories and starts the VFS and the log. */
static void env_start(hr_fx *f) {
    f->log = de_log_init(NULL) == DE_OK;
    de_error_clear();
    ASSERT_EQ(de_fs_mount("tmp", DE_HOST_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    f->tmp_mounted = true;
    /* CTest runs the tests of this file in parallel processes: the pid keeps them apart. */
    snprintf(f->run, sizeof f->run, "run-%010lu-%02u", hr_getpid(), g_run_counter++);
    char v[DE_FS_PATH_MAX];
    const char *dirs[] = {"lib", "user", "cache"};
    for (u32 i = 0; i < 3; ++i) {
        snprintf(v, sizeof v, "tmp:/%s/%s", f->run, dirs[i]);
        ASSERT_EQ(de_dir_create(v), DE_OK);
    }
    char user[DE_FS_PATH_MAX], cache[DE_FS_PATH_MAX], lib[DE_FS_PATH_MAX];
    snprintf(v, sizeof v, "tmp:/%s/user", f->run);
    ASSERT_EQ(de_fs_resolve(v, user, sizeof user), DE_OK);
    snprintf(v, sizeof v, "tmp:/%s/cache", f->run);
    ASSERT_EQ(de_fs_resolve(v, cache, sizeof cache), DE_OK);
    snprintf(v, sizeof v, "tmp:/%s/lib", f->run);
    ASSERT_EQ(de_fs_resolve(v, lib, sizeof lib), DE_OK);
    de_fs_desc fd;
    memset(&fd, 0, sizeof fd);
    fd.game_dir = lib;
    fd.user_dir = user;
    fd.cache_dir = cache;
    ASSERT_EQ(de_fs_init(&fd), DE_OK);
    f->fs = true;

    char name[64];
    ASSERT_EQ(de_dylib_platform_name("reload_game", name, sizeof name), DE_OK);
    snprintf(f->watched, sizeof f->watched, "%s/%s", lib, name);
}

/* Attaches the watched library and starts the game the way de_host_run does. Without
 * `keep_watch` the file watch is stopped, so only de__hot_reload_request triggers reloads and a
 * late watch event cannot add a second one. */
static void game_start_ex(hr_fx *f, const char *first_library, bool keep_watch) {
    copy_native(first_library, f->watched);
    de__host *h = &f->h;
    de__game_config c;
    de__game_config_defaults(&c);
    c.task_workers = 1;
    de__engine_desc d;
    memset(&d, 0, sizeof d);
    d.config = &c;
    d.headless = true;
    ASSERT_EQ(de__engine_create(&d, &h->engine), DE_OK);
    h->log = de_log_register_category("host", DE_LOG_INFO);
    h->desc.game_library = f->watched;
    ASSERT_EQ(de__hot_reload_init(h), DE_OK);
    ASSERT_TRUE(h->hot_reload.enabled);
    ASSERT_EQ(de__game_attach(h, &h->desc), DE_OK);

    de_engine *e = h->engine;
    de__world_set_system_owner(e->world, DE__HOST_GAME_SYSTEM_OWNER);
    ASSERT_EQ(h->api->init(e), DE_OK);
    f->started = true;
    h->api->register_types(e, e->types);
    h->api->register_systems(e, e->world);
    de__world_set_system_owner(e->world, DE__SYSTEM_OWNER_ENGINE);
    ASSERT_TRUE(de_error_last() == NULL);
    if (!keep_watch) {
        de_file_watch_destroy(h->hot_reload.watch);
        h->hot_reload.watch = NULL;
    }
}

static void game_start(hr_fx *f, const char *first_library) {
    game_start_ex(f, first_library, false);
}

static void hr_fx_setup(hr_fx *f) {
#if !DE__HOT_RELOAD
    (void)f;
    (void)env_start;
    SKIP("hot reload is compiled out in this build type (Debug and Dev only)");
#else
    env_start(f);
#endif
}

static void hr_fx_teardown(hr_fx *f) {
    de__host *h = &f->h;
    if (h->engine != NULL) {
        de_engine *e = h->engine;
        if (f->started && h->api != NULL)
            h->api->shutdown(e);
        de__game_detach(h);
        de__hot_reload_shutdown(h);
        if (e->game_state != NULL)
            DE_FREE(e->allocator, e->game_state, sizeof(reload_game_state));
        e->game_state = NULL;
        de__engine_destroy(h->engine);
        h->engine = NULL;
    }
    if (f->fs)
        de_fs_shutdown(); /* removes every mount, tmp:/ too */
    if (f->tmp_mounted && f->run[0] != '\0' &&
        de_fs_mount("tmp", DE_HOST_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0) == DE_OK) {
        char v[DE_FS_PATH_MAX];
        snprintf(v, sizeof v, "tmp:/%s", f->run);
        rm_tree(v);
    }
    if (f->tmp_mounted)
        (void)de_fs_unmount("tmp", DE_HOST_TEST_TMP_DIR);
    if (f->log)
        de_log_shutdown();
    de_error_clear();
}

/* ---- Helpers -------------------------------------------------------------------------------- */

static reload_game_state *state(hr_fx *f) {
    reload_game_state *s = (reload_game_state *)f->h.engine->game_state;
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ(s->magic, RELOAD_GAME_STATE_MAGIC);
    return s;
}

static de_component counter(hr_fx *f) {
    const de_component c = de_component_find(f->h.engine->world, "DeTestCounter");
    ASSERT_TRUE(DE_HANDLE_VALID(c));
    return c;
}

static void *counter_data(hr_fx *f, u32 i) {
    void *p = de_entity_get(f->h.engine->world, state(f)->entities[i], counter(f));
    ASSERT_TRUE(p != NULL);
    return p;
}

static i32 value(hr_fx *f, u32 i) {
    i32 v;
    memcpy(&v, counter_data(f, i), sizeof v);
    return v;
}

static f32 scale(hr_fx *f, u32 i) {
    counter_v2 c;
    memcpy(&c, counter_data(f, i), sizeof c);
    return c.scale;
}

/* DeTestTotals.sum (the first field in every version). */
static i64 totals_sum(hr_fx *f) {
    de_world *w = f->h.engine->world;
    const de_resource_id r = de_resource_find(w, de_type_find(f->h.engine->types, "DeTestTotals"));
    const void *p = de_resource_get(w, r);
    ASSERT_TRUE(p != NULL);
    i64 v;
    memcpy(&v, p, sizeof v);
    return v;
}

/* Every alive entity (a query without terms matches every archetype). */
static u32 entity_count(hr_fx *f) {
    de_world *w = f->h.engine->world;
    const de_query_desc qd = {NULL, 0, NULL, 0};
    const de_query q = de_query_create(w, &qd);
    ASSERT_TRUE(DE_HANDLE_VALID(q));
    const u32 n = de_query_count(w, q);
    de_query_destroy(w, q);
    return n;
}

/* One frame of the test: Update, then the frame-end poll. */
static void frame(hr_fx *f) {
    ASSERT_EQ(de_world_run_schedule(f->h.engine->world, DE_SCHEDULE_UPDATE, 1.0f / 60.0f, 0.0f),
              DE_OK);
    ASSERT_EQ(de__hot_reload_poll(&f->h), DE_OK);
}

/* The "build": `library` is written over the watched path; the next poll reloads it. */
static void rebuild(hr_fx *f, const char *library) {
    copy_native(library, f->watched);
    de__hot_reload_request(&f->h);
}

/* Native path of the copy that is loaded now. */
static void current_copy_native(hr_fx *f, char *buf, size_t cap) {
    ASSERT_TRUE(f->h.hot_reload.current_copy[0] != '\0');
    ASSERT_EQ(de_fs_resolve(f->h.hot_reload.current_copy, buf, cap), DE_OK);
}

static void expect_values(hr_fx *f, i32 added) {
    for (u32 i = 0; i < RELOAD_GAME_ENTITIES; ++i)
        EXPECT_EQ(value(f, i), (i32)(i * 100u) + added);
}

/* ---- Start-up ------------------------------------------------------------------------------- */

TEST_F(hot_reload, runs_from_a_copy, hr_fx) {
    game_start(f, DE_HOST_RELOAD_V1);
    de__hot_reload *hr = &f->h.hot_reload;
    /* The library the build writes is not loaded; a copy in cache:/hot/ is. */
    EXPECT_STREQ(hr->source, f->watched);
    EXPECT_FALSE(lib_loaded(f->watched));
    EXPECT_TRUE(strncmp(hr->current_copy, "cache:/hot/", 11) == 0);
    EXPECT_TRUE(strstr(hr->current_copy, ".reload-0") != NULL);
    char copy[DE_FS_PATH_MAX];
    current_copy_native(f, copy, sizeof copy);
    EXPECT_TRUE(lib_loaded(copy));
    EXPECT_STREQ(f->h.game.path, copy);
    EXPECT_STREQ(f->h.api->name, "reload_game_v1");
    EXPECT_TRUE(f->h.api == &f->h.game.table);
    EXPECT_EQ(f->h.game.generation, 1u);
    /* The game's system carries its owner tag; the engine's built-ins keep 0. */
    EXPECT_EQ(de__world_system_count_by_owner(f->h.engine->world, DE__HOST_GAME_SYSTEM_OWNER), 1u);
    EXPECT_GT(de__world_system_count_by_owner(f->h.engine->world, DE__SYSTEM_OWNER_ENGINE), 0u);
    frame(f);
    expect_values(f, 1);
    EXPECT_EQ(hr->reloads, 0u);
}

/* ---- Acceptance tests (issue test plan) ----------------------------------------------------- */

TEST_F(hot_reload, swap_systems, hr_fx) {
    game_start(f, DE_HOST_RELOAD_V1);
    de_world *w = f->h.engine->world;
    const u32 engine_systems = de__world_system_count_by_owner(w, DE__SYSTEM_OWNER_ENGINE);
    char old_copy[DE_FS_PATH_MAX];
    current_copy_native(f, old_copy, sizeof old_copy);

    frame(f);
    expect_values(f, 1);
    rebuild(f, DE_HOST_RELOAD_V2);
    frame(f); /* Update runs v1 (+1), then the frame-end poll reloads */
    expect_values(f, 2);
    EXPECT_EQ(f->h.hot_reload.reloads, 1u);
    EXPECT_EQ(f->h.hot_reload.failures, 0u);
    EXPECT_EQ(f->h.game.generation, 2u);
    EXPECT_STREQ(f->h.api->name, "reload_game_v2");
    EXPECT_EQ(f->h.hot_reload.last_systems_removed, 1u);
    EXPECT_FALSE(lib_loaded(old_copy));

    frame(f); /* v2's system: +10 */
    expect_values(f, 12);
    EXPECT_EQ(de__world_system_count_by_owner(w, DE__HOST_GAME_SYSTEM_OWNER), 1u);
    EXPECT_EQ(de__world_system_count_by_owner(w, DE__SYSTEM_OWNER_ENGINE), engine_systems);

    const reload_game_state *s = state(f);
    EXPECT_EQ(s->inits, 1u); /* init runs on the first load only */
    EXPECT_EQ(s->loads, 2u);
    EXPECT_EQ(s->unloads, 1u);
    EXPECT_EQ(s->reloads, 1u);
    EXPECT_EQ(s->version, 2u);
    EXPECT_TRUE(f->h.engine->game_state == s); /* game_state survives */
}

TEST_F(hot_reload, component_migration, hr_fx) {
    game_start(f, DE_HOST_RELOAD_V1);
    frame(f);
    frame(f);
    expect_values(f, 2);
    EXPECT_EQ(totals_sum(f), 2 * (i64)RELOAD_GAME_ENTITIES);
    const u32 entities = entity_count(f);
    EXPECT_EQ(entities, RELOAD_GAME_ENTITIES);
    const de_type_id type = de_type_find(f->h.engine->types, "DeTestCounter");
    const u64 hash_v1 = de_type_schema_hash(f->h.engine->types, type);

    rebuild(f, DE_HOST_RELOAD_V2);
    ASSERT_EQ(de__hot_reload_poll(&f->h), DE_OK);
    ASSERT_EQ(f->h.hot_reload.reloads, 1u);
    /* The component id and type id stay; the layout is version 2's. */
    EXPECT_EQ(de_type_find(f->h.engine->types, "DeTestCounter").id, type.id);
    EXPECT_NE(de_type_schema_hash(f->h.engine->types, type), hash_v1);
    const ecs_component_info *info = ecs_component_get(f->h.engine->world, counter(f).id);
    EXPECT_EQ(info->size, (u32)sizeof(counter_v2));
    EXPECT_EQ(f->h.hot_reload.last_sync.relaid_out, 1u);
    EXPECT_EQ(f->h.hot_reload.last_sync.resources, 1u); /* DeTestTotals gained `frames` */
    EXPECT_EQ(f->h.hot_reload.last_sync.migrate_refused, 0u);

    /* Every entity kept `value`; `scale` is new, so zero; nothing was spawned or lost. */
    expect_values(f, 2);
    for (u32 i = 0; i < RELOAD_GAME_ENTITIES; ++i)
        EXPECT_FLOAT_EQ(scale(f, i), 0.0f, 0.0);
    EXPECT_EQ(entity_count(f), entities);
    EXPECT_EQ(totals_sum(f), 2 * (i64)RELOAD_GAME_ENTITIES);

    frame(f);
    expect_values(f, 12);
    EXPECT_EQ(totals_sum(f), 12 * (i64)RELOAD_GAME_ENTITIES);
    /* A new entity gets the new layout from the new construct hook. */
    de_world *w = f->h.engine->world;
    const de_entity extra = de_entity_spawn(w);
    counter_v2 *c = (counter_v2 *)de_entity_add(w, extra, counter(f));
    ASSERT_TRUE(c != NULL);
    EXPECT_EQ(c->value, 0);
    EXPECT_FLOAT_EQ(c->scale, 0.0f, 0.0);
}

TEST_F(hot_reload, same_schema_no_relayout, hr_fx) {
    game_start(f, DE_HOST_RELOAD_V1);
    frame(f);
    de_world *w = f->h.engine->world;
    const de_type_id type = de_type_find(f->h.engine->types, "DeTestCounter");
    void *before[RELOAD_GAME_ENTITIES];
    for (u32 i = 0; i < RELOAD_GAME_ENTITIES; ++i)
        before[i] = counter_data(f, i);
    const ecs_component_info *info = ecs_component_get(w, counter(f).id);
    void (*const old_construct)(void *) = info->hooks.construct;
    const de_type_desc *old_desc = de_type_get(f->h.engine->types, type);
    const u64 hash = de_type_schema_hash(f->h.engine->types, type);
    char old_copy[DE_FS_PATH_MAX];
    current_copy_native(f, old_copy, sizeof old_copy);

    rebuild(f, DE_HOST_RELOAD_V1); /* the same code again: the same schema */
    ASSERT_EQ(de__hot_reload_poll(&f->h), DE_OK);
    ASSERT_EQ(f->h.hot_reload.reloads, 1u);
    EXPECT_EQ(f->h.hot_reload.last_sync.relaid_out, 0u);
    EXPECT_EQ(f->h.hot_reload.last_sync.resources, 0u);
    EXPECT_GE(f->h.hot_reload.last_sync.refreshed, 2u); /* the component and the resource */
    EXPECT_EQ(de_type_schema_hash(f->h.engine->types, type), hash);
    EXPECT_TRUE(de_type_get_previous(f->h.engine->types, type) == NULL);

    /* Column memory untouched: the same rows at the same addresses with the same values. */
    for (u32 i = 0; i < RELOAD_GAME_ENTITIES; ++i)
        EXPECT_TRUE(counter_data(f, i) == before[i]);
    expect_values(f, 1);

    /* Only the hook pointers moved, to the new copy's code. */
    info = ecs_component_get(w, counter(f).id);
    const de_type_desc *new_desc = de_type_get(f->h.engine->types, type);
    EXPECT_TRUE(new_desc != old_desc);
    EXPECT_TRUE(info->layout == new_desc);
    EXPECT_TRUE(info->hooks.construct == new_desc->hooks.construct);
    EXPECT_TRUE(info->hooks.construct != old_construct);
    EXPECT_FALSE(lib_loaded(old_copy));
    frame(f);
    expect_values(f, 2);
}

TEST_F(hot_reload, bad_library_keeps_old, hr_fx) {
    game_start(f, DE_HOST_RELOAD_V1);
    frame(f);
    char v1_copy[DE_FS_PATH_MAX];
    current_copy_native(f, v1_copy, sizeof v1_copy);

    /* v3: wrong api_version. Refused before the running game is touched. */
    rebuild(f, DE_HOST_RELOAD_V3);
    ASSERT_EQ(de__hot_reload_poll(&f->h), DE_OK); /* logged, not an error of the loop */
    EXPECT_EQ(f->h.hot_reload.failures, 1u);
    EXPECT_EQ(f->h.hot_reload.reloads, 0u);
    EXPECT_FALSE(f->h.hot_reload.pending);
    EXPECT_EQ(f->h.game.generation, 1u);
    EXPECT_STREQ(f->h.api->name, "reload_game_v1");
    EXPECT_TRUE(lib_loaded(v1_copy));
    EXPECT_EQ(state(f)->unloads, 0u);
    EXPECT_EQ(state(f)->loads, 1u);
    frame(f);
    expect_values(f, 2); /* v1 keeps counting */

    /* A direct reload reports the refusal. */
    rebuild(f, DE_HOST_RELOAD_V3);
    bool fatal = true;
    EXPECT_EQ(de__hot_reload_reload(&f->h, &fatal), DE_ERR_VERSION_MISMATCH);
    EXPECT_FALSE(fatal);
    EXPECT_EQ(f->h.hot_reload.failures, 2u);
    de_error_clear();

    /* v4 (version 2 with a migrate hook) is fine: it reloads. */
    rebuild(f, DE_HOST_RELOAD_V4);
    frame(f); /* +1 by v1, then the reload */
    ASSERT_EQ(f->h.hot_reload.reloads, 1u);
    EXPECT_STREQ(f->h.api->name, "reload_game_v4");
    EXPECT_FALSE(lib_loaded(v1_copy));
    expect_values(f, 3);
    for (u32 i = 0; i < RELOAD_GAME_ENTITIES; ++i)
        EXPECT_FLOAT_EQ(scale(f, i), 0.5f, 0.0); /* set by the migrate hook */
    EXPECT_EQ(f->h.hot_reload.last_sync.relaid_out, 1u);
    EXPECT_EQ(entity_count(f), RELOAD_GAME_ENTITIES);
    frame(f);
    expect_values(f, 103);
}

TEST_F(hot_reload, bad_types_restores_old, hr_fx) {
    game_start(f, DE_HOST_RELOAD_V1);
    frame(f);
    de_world *w = f->h.engine->world;
    const de_type_id type = de_type_find(f->h.engine->types, "DeTestCounter");
    char v1_copy[DE_FS_PATH_MAX];
    current_copy_native(f, v1_copy, sizeof v1_copy);

    /* v5's register_types fails after it re-registered DeTestCounter with version 2's layout. */
    rebuild(f, DE_HOST_RELOAD_V5);
    ASSERT_EQ(de__hot_reload_poll(&f->h), DE_OK);
    EXPECT_EQ(f->h.hot_reload.failures, 1u);
    EXPECT_EQ(f->h.hot_reload.reloads, 0u);
    EXPECT_STREQ(f->h.api->name, "reload_game_v1");
    EXPECT_TRUE(lib_loaded(v1_copy));
    /* v1's types, systems and callbacks are back; nothing was re-laid out. */
    const de_type_desc *d = de_type_get(f->h.engine->types, type);
    ASSERT_TRUE(d != NULL);
    EXPECT_EQ(d->field_count, 1u);
    EXPECT_EQ(d->size, (u32)sizeof(i32));
    const ecs_component_info *info = ecs_component_get(w, counter(f).id);
    EXPECT_EQ(info->size, (u32)sizeof(i32));
    EXPECT_TRUE(info->hooks.construct == d->hooks.construct);
    EXPECT_EQ(de__world_system_count_by_owner(w, DE__HOST_GAME_SYSTEM_OWNER), 1u);
    const reload_game_state *s = state(f);
    EXPECT_EQ(s->unloads, 1u);
    EXPECT_EQ(s->loads, 2u);
    EXPECT_EQ(s->reloads, 1u);
    EXPECT_EQ(s->version, 1u);
    frame(f);
    expect_values(f, 2);

    rebuild(f, DE_HOST_RELOAD_V2);
    frame(f);
    EXPECT_EQ(f->h.hot_reload.reloads, 1u);
    frame(f);
    expect_values(f, 13);
}

TEST_F(hot_reload, string_lifetime, hr_fx) {
    game_start(f, DE_HOST_RELOAD_V1);
    frame(f);
    de_type_registry *types = f->h.engine->types;
    const de_type_id type = de_type_find(types, "DeTestCounter");
    char v1_copy[DE_FS_PATH_MAX];
    current_copy_native(f, v1_copy, sizeof v1_copy);
    char v1_vpath[DE_FS_PATH_MAX];
    memcpy(v1_vpath, f->h.hot_reload.current_copy, sizeof v1_vpath);

    rebuild(f, DE_HOST_RELOAD_V2);
    frame(f);
    ASSERT_EQ(f->h.hot_reload.reloads, 1u);
    /* v1 is closed and its copy is gone: every string below must be the registry's own copy.
     * Under ASan (ci-macos-asan, the Linux sanitizer jobs) a read through a pointer into v1's
     * read-only data fails the run. */
    EXPECT_FALSE(lib_loaded(v1_copy));
    EXPECT_FALSE(de_path_exists(v1_vpath));
    EXPECT_TRUE(de_error_last() == NULL);

    const de_type_desc *cur = de_type_get(types, type);
    const de_type_desc *prev = de_type_get_previous(types, type);
    ASSERT_TRUE(cur != NULL && prev != NULL);
    EXPECT_STREQ(cur->name, "DeTestCounter");
    EXPECT_STREQ(prev->name, "DeTestCounter");
    ASSERT_EQ(cur->field_count, 2u);
    ASSERT_EQ(prev->field_count, 1u);
    EXPECT_STREQ(cur->fields[0].name, "value");
    EXPECT_STREQ(cur->fields[0].attrs.units, "count");
    EXPECT_STREQ(cur->fields[0].attrs.tooltip, "counted by reload_game_v2");
    EXPECT_STREQ(cur->fields[1].name, "scale");
    EXPECT_STREQ(cur->fields[1].attrs.tooltip, "added in version 2");
    EXPECT_STREQ(prev->fields[0].name, "value");
    EXPECT_STREQ(prev->fields[0].attrs.units, "count");
    EXPECT_STREQ(prev->fields[0].attrs.tooltip, "counted by reload_game_v1");
    const de_enum_info *mode = de_enum_get(types, de_type_find(types, "DeTestMode"));
    ASSERT_TRUE(mode != NULL);
    EXPECT_STREQ(mode->name, "DeTestMode");
    EXPECT_STREQ(mode->values[0].name, "Idle");
    EXPECT_STREQ(mode->values[1].name, "Counting");
    /* The ECS names its component by the registry's string too. */
    EXPECT_TRUE(DE_HANDLE_VALID(de_component_find(f->h.engine->world, "DeTestCounter")));
    EXPECT_STREQ(ecs_component_get(f->h.engine->world, counter(f).id)->name, "DeTestCounter");
    /* The log thread formats v1's records (their __FILE__ strings) before v1 is closed. */
    de_log_flush();

    /* Once more, so the previous layout's strings come from a library that is closed too. */
    rebuild(f, DE_HOST_RELOAD_V1);
    frame(f);
    ASSERT_EQ(f->h.hot_reload.reloads, 2u);
    prev = de_type_get_previous(types, type);
    ASSERT_TRUE(prev != NULL);
    EXPECT_STREQ(prev->fields[1].attrs.tooltip, "added in version 2");
    expect_values(f, 12); /* +1 (v1), +1 (v1, then v2 loads), +10 (v2, then v1 loads) */
}

/* The frame-boundary test drives the real main loop: a system of frame 2 rebuilds the library
 * and tries to reload from inside the schedule; the reload must wait for the frame-end hook. */
static struct {
    de__host *h;
    const char *watched;
    u32 update_runs;
    u32 last_runs;
    i32 value[16];
    u32 generation[16];
    u32 gen_in_trigger;
    de_result poll_in_trigger;
    de_result reload_in_trigger;
    bool reload_happened_mid_frame;
    de_component counter;
} g_fb;

static void sys_trigger(de_system_ctx *ctx) {
    (void)ctx;
    if (g_fb.update_runs++ != 2)
        return;
    copy_native(DE_HOST_RELOAD_V2, g_fb.watched);
    de__hot_reload_request(g_fb.h);
    g_fb.poll_in_trigger = de__hot_reload_poll(g_fb.h);
    g_fb.reload_in_trigger = de__hot_reload_reload(g_fb.h, NULL);
    de_error_clear();
    g_fb.gen_in_trigger = g_fb.h->game.generation;
}

static void sys_probe(de_system_ctx *ctx) {
    if (g_fb.last_runs >= 16)
        return;
    const reload_game_state *st = (const reload_game_state *)g_fb.h->engine->game_state;
    const i32 *v = (const i32 *)de_entity_get_const(ctx->world, st->entities[0], g_fb.counter);
    g_fb.value[g_fb.last_runs] = v != NULL ? *v : -1;
    g_fb.generation[g_fb.last_runs] = g_fb.h->game.generation;
    if (g_fb.h->hot_reload.reloads > 0 && g_fb.last_runs <= 2)
        g_fb.reload_happened_mid_frame = true;
    g_fb.last_runs++;
}

TEST_F(hot_reload, frame_boundary, hr_fx) {
    game_start(f, DE_HOST_RELOAD_V1);
    memset(&g_fb, 0, sizeof g_fb);
    g_fb.h = &f->h;
    g_fb.watched = f->watched;
    g_fb.counter = counter(f);
    de_world *w = f->h.engine->world;

    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "test.trigger";
    sd.schedule = DE_SCHEDULE_UPDATE;
    sd.fn = sys_trigger;
    sd.flags = DE_SYSTEM_MAIN_THREAD;
    ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(w, &sd)));
    const de_query_term term = {counter(f), DE_READ};
    const de_query_desc q = {&term, 1, NULL, 0};
    memset(&sd, 0, sizeof sd);
    sd.name = "test.probe";
    sd.schedule = DE_SCHEDULE_LAST;
    sd.fn = sys_probe;
    sd.flags = DE_SYSTEM_MAIN_THREAD;
    sd.queries = &q;
    sd.query_count = 1;
    ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(w, &sd)));

    f->h.loop_max_frames = 6;
    ASSERT_EQ(de__loop_run(&f->h), DE_OK);
    ASSERT_EQ(g_fb.last_runs, 6u);

    /* Inside the schedule the poll left the reload pending and the direct call refused. */
    EXPECT_EQ(g_fb.poll_in_trigger, DE_OK);
    EXPECT_EQ(g_fb.reload_in_trigger, DE_ERR_BUSY);
    EXPECT_EQ(g_fb.gen_in_trigger, 1u);
    EXPECT_FALSE(g_fb.reload_happened_mid_frame);
    EXPECT_EQ(f->h.hot_reload.reloads, 1u);
    /* Frames 0-2 ran v1 to their end (Last saw v1's +1 each), frames 3-5 ran v2 (+10). */
    const i32 expected[6] = {1, 2, 3, 13, 23, 33};
    for (u32 i = 0; i < 6; ++i) {
        EXPECT_EQ(g_fb.value[i], expected[i]);
        EXPECT_EQ(g_fb.generation[i], i < 3 ? 1u : 2u);
    }
    /* The test's own systems (owner 0) survived the reload. */
    EXPECT_EQ(g_fb.update_runs, 6u);
}

TEST_F(hot_reload, watch_triggers_reload, hr_fx) {
    game_start_ex(f, DE_HOST_RELOAD_V1, true);
    if (f->h.hot_reload.watch == NULL)
        SKIP("no file watch on this platform");
    copy_native(DE_HOST_RELOAD_V2, f->watched); /* no request: the watch must see it */
    const u64 deadline = de_time_now_ns() + 20ull * 1000000000ull;
    while (f->h.hot_reload.reloads == 0 && de_time_now_ns() < deadline) {
        frame(f);
        de_sleep_ns(10ull * 1000000ull);
    }
    ASSERT_EQ(f->h.hot_reload.reloads, 1u);
    EXPECT_STREQ(f->h.api->name, "reload_game_v2");
}

TEST_F(hot_reload, shutdown_deletes_copy, hr_fx) {
    game_start(f, DE_HOST_RELOAD_V1);
    rebuild(f, DE_HOST_RELOAD_V2);
    frame(f);
    char vpath[DE_FS_PATH_MAX];
    memcpy(vpath, f->h.hot_reload.current_copy, sizeof vpath);
    EXPECT_TRUE(de_path_exists(vpath));
    f->h.api->shutdown(f->h.engine);
    f->started = false;
    de__game_detach(&f->h);
    de__hot_reload_shutdown(&f->h);
    EXPECT_FALSE(de_path_exists(vpath));
    EXPECT_TRUE(f->h.hot_reload.watch == NULL);
    EXPECT_FALSE(f->h.hot_reload.mounted);
    EXPECT_EQ(state(f)->shutdowns, 1u);
}

/* ---- ECS hooks without a library ------------------------------------------------------------ */

typedef struct ecs_old {
    i32 a;
    char name[8];
    u16 list[4];
    i32 gone;
} ecs_old;

typedef struct ecs_new {
    char name[4];
    u16 list[2];
    i32 a;
    i64 added;
    i64 more;
} ecs_new;

static u32 g_migrate_calls;
static bool ecs_new_migrate(void *obj, u32 from_version, const void *old_data,
                            const de_field_info *old_fields, u32 old_field_count) {
    ecs_new *n = (ecs_new *)obj;
    ecs_old o;
    memcpy(&o, old_data, sizeof o);
    g_migrate_calls++;
    (void)old_fields;
    if (from_version != 1 || old_field_count != 4)
        return false;
    n->added = (i64)o.gone * 2;
    return o.a % 2 == 0; /* odd rows are "refused" */
}

TEST(hot_reload, ecs_relayout_component) {
    de_type_registry *types = NULL;
    ASSERT_EQ(de_type_registry_create(NULL, &types), DE_OK);
    de_world_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.types = types;
    wd.chunk_bytes = DE_ECS_MIN_CHUNK_BYTES; /* small chunks: many of them, rows move between */
    de_world *w = NULL;
    ASSERT_EQ(de_world_create(&wd, &w), DE_OK);

    const de_field_info old_fields[] = {
        {.name = "a", .offset = 0, .size = 4, .kind = DE_FK_I32},
        {.name = "name", .offset = 4, .size = 8, .kind = DE_FK_STRING},
        {.name = "list",
         .offset = 12,
         .size = 8,
         .kind = DE_FK_FIXED_ARRAY,
         .attrs = {.nested_type = "u16", .array_count = 4}},
        {.name = "gone", .offset = 20, .size = 4, .kind = DE_FK_I32},
    };
    de_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = "TReload";
    d.size = sizeof(ecs_old);
    d.align = 4;
    d.version = 1;
    d.fields = old_fields;
    d.field_count = 4;
    d.is_pod = true;
    const de_type_id t = de_type_register(types, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    const de_field_info tag_fields[] = {{.name = "x", .offset = 0, .size = 4, .kind = DE_FK_I32}};
    de_type_desc td;
    memset(&td, 0, sizeof td);
    td.name = "TTag";
    td.size = 4;
    td.align = 4;
    td.fields = tag_fields;
    td.field_count = 1;
    td.is_pod = true;
    const de_type_id tt = de_type_register(types, &td);
    const de_component c = de_component_register(w, t, 0);
    const de_component tag = de_component_register(w, tt, DE_COMP_TAG);
    ASSERT_TRUE(DE_HANDLE_VALID(c) && DE_HANDLE_VALID(tag));
    const de_event_type ev = de_event_register(w, t);
    ASSERT_TRUE(DE_HANDLE_VALID(ev));

    /* Two archetypes (with and without the tag), many chunks each. */
    enum { N = 600 };
    de_entity ents[N];
    for (u32 i = 0; i < N; ++i) {
        ents[i] = de_entity_spawn(w);
        ecs_old *o = (ecs_old *)de_entity_add(w, ents[i], c);
        ASSERT_TRUE(o != NULL);
        o->a = (i32)i;
        snprintf(o->name, sizeof o->name, "e%u", i);
        for (u32 k = 0; k < 4; ++k)
            o->list[k] = (u16)(i + k);
        o->gone = (i32)i * 3;
        if (i % 3 == 0)
            (void)de_entity_add(w, ents[i], tag);
    }
    const ecs_old one = {7, "event", {1, 2, 3, 4}, 5};
    de_event_send(w, ev, &one);
    const u32 rows_before =
        ecs_archetype_at(w, ecs_entity_record(w, ents[1])->archetype)->rows_per_chunk;

    /* Version 2: reordered, `name` and `list` shrink, `gone` removed, `added` new, a migrate
     * hook. */
    const de_field_info new_fields[] = {
        {.name = "name", .offset = 0, .size = 4, .kind = DE_FK_STRING},
        {.name = "list",
         .offset = 4,
         .size = 4,
         .kind = DE_FK_FIXED_ARRAY,
         .attrs = {.nested_type = "u16", .array_count = 2}},
        {.name = "a", .offset = 8, .size = 4, .kind = DE_FK_I32},
        {.name = "added", .offset = 16, .size = 8, .kind = DE_FK_I64},
        {.name = "more", .offset = 24, .size = 8, .kind = DE_FK_I64},
    };
    d.size = sizeof(ecs_new);
    d.align = 8;
    d.version = 2;
    d.fields = new_fields;
    d.field_count = 5;
    d.hooks.migrate = ecs_new_migrate;
    ASSERT_EQ(de_type_register(types, &d).id, t.id);

    /* Another component's type cannot be taken over. */
    EXPECT_EQ(de__world_relayout_component(w, c, tt, NULL), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();

    g_migrate_calls = 0;
    de__world_sync_stats st;
    ASSERT_EQ(de__world_sync_types(w, &st), DE_OK);
    EXPECT_EQ(st.relaid_out, 1u);
    EXPECT_EQ(st.events, 1u);
    EXPECT_EQ(g_migrate_calls, (u32)N + 1u);
    EXPECT_EQ(st.migrate_refused, (u32)N / 2u + 1u); /* odd `a`, and the event's a = 7 */
    EXPECT_EQ(ecs_component_get(w, c.id)->size, (u32)sizeof(ecs_new));
    EXPECT_NE(ecs_archetype_at(w, ecs_entity_record(w, ents[1])->archetype)->rows_per_chunk,
              rows_before);

    for (u32 i = 0; i < N; ++i) {
        const ecs_new *n = (const ecs_new *)de_entity_get(w, ents[i], c);
        ASSERT_TRUE(n != NULL);
        EXPECT_EQ(n->a, (i32)i);
        char want[8];
        snprintf(want, sizeof want, "e%u", i);
        want[3] = '\0'; /* cut to the 4-byte buffer */
        EXPECT_STREQ(n->name, want);
        EXPECT_EQ(n->list[0], (u16)i);
        EXPECT_EQ(n->list[1], (u16)(i + 1));
        EXPECT_EQ(n->added, (i64)i * 6);
        EXPECT_EQ(n->more, 0); /* new and not migrated: zero */
        EXPECT_EQ(de_entity_has(w, ents[i], tag), i % 3 == 0);
    }
    de_event_reader r = de_event_reader_create(w, ev);
    const ecs_new *e1 = (const ecs_new *)de_event_next(w, &r);
    ASSERT_TRUE(e1 != NULL);
    EXPECT_EQ(e1->a, 7);
    EXPECT_STREQ(e1->name, "eve");
    EXPECT_EQ(e1->added, 10);

    /* Synced: a second sync changes nothing. */
    ASSERT_EQ(de__world_sync_types(w, &st), DE_OK);
    EXPECT_EQ(st.relaid_out + st.refreshed + st.events + st.resources, 0u);

    /* Owners: 0 is never removed by owner. */
    de__world_set_system_owner(w, 7);
    EXPECT_EQ(de__world_system_owner(w), 7u);
    de__world_set_system_owner(w, 0);
    EXPECT_EQ(de__world_remove_systems_by_owner(w, 0), 0u);

    de_world_destroy(w);
    de_type_registry_destroy(types);
}

TEST(hot_reload, ecs_relayout_refuses_bad_type) {
    de_type_registry *types = NULL;
    ASSERT_EQ(de_type_registry_create(NULL, &types), DE_OK);
    de_world_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.types = types;
    wd.chunk_bytes = DE_ECS_MIN_CHUNK_BYTES;
    de_world *w = NULL;
    ASSERT_EQ(de_world_create(&wd, &w), DE_OK);
    const de_field_info f1[] = {{.name = "v", .offset = 0, .size = 4, .kind = DE_FK_I32}};
    de_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = "TBig";
    d.size = 4;
    d.align = 4;
    d.fields = f1;
    d.field_count = 1;
    d.is_pod = true;
    const de_type_id t = de_type_register(types, &d);
    const de_component c = de_component_register(w, t, 0);
    const de_entity e = de_entity_spawn(w);
    i32 *v = (i32 *)de_entity_add(w, e, c);
    ASSERT_TRUE(v != NULL);
    *v = 42;

    /* The new layout does not fit a chunk row: refused, nothing changes. */
    const de_field_info f2[] = {{.name = "v", .offset = 0, .size = 4, .kind = DE_FK_I32},
                                {.name = "blob",
                                 .offset = 4,
                                 .size = DE_ECS_MIN_CHUNK_BYTES,
                                 .kind = DE_FK_FIXED_ARRAY,
                                 .attrs = {.array_count = DE_ECS_MIN_CHUNK_BYTES}}};
    d.size = 4 + DE_ECS_MIN_CHUNK_BYTES;
    d.fields = f2;
    d.field_count = 2;
    ASSERT_TRUE(DE_HANDLE_VALID(de_type_register(types, &d)));
    EXPECT_EQ(de__world_sync_types(w, NULL), DE_ERR_UNSUPPORTED);
    de_error_clear();
    EXPECT_EQ(ecs_component_get(w, c.id)->size, 4u);
    EXPECT_EQ(*(const i32 *)de_entity_get(w, e, c), 42);

    de_world_destroy(w);
    de_type_registry_destroy(types);
}
