/*
 * test_game_toml.c: the game.toml project file (DENG-90). Fixtures in data/, mounted as game:/;
 * error cases that need one line each are inline. Warnings are captured from the log.
 */
#include "de_test.h"

#include "host/engine_internal.h"

#include <dengine/de_debug.h>

#include <string.h>
#include <stdio.h>

#ifndef DE_HOST_TEST_DATA_DIR
#error "DE_HOST_TEST_DATA_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif

#define CAP_MAX 32
#define CAP_LEN 400

static struct {
    u32 count;
    char msgs[CAP_MAX][CAP_LEN];
} g_cap;

static void capture_sink(const de_log_record *rec, void *user) {
    (void)user;
    if (rec->level != DE_LOG_WARN || g_cap.count >= CAP_MAX)
        return;
    size_t n = rec->message.len < CAP_LEN - 1 ? rec->message.len : CAP_LEN - 1;
    memcpy(g_cap.msgs[g_cap.count], rec->message.ptr, n);
    g_cap.msgs[g_cap.count][n] = '\0';
    g_cap.count++;
}

static u32 warnings(void) {
    de_log_flush();
    return g_cap.count;
}

static bool warned(const char *needle) {
    de_log_flush();
    for (u32 i = 0; i < g_cap.count; ++i)
        if (strstr(g_cap.msgs[i], needle) != NULL)
            return true;
    return false;
}

typedef struct gt_fx {
    bool mounted;
} gt_fx;

static void gt_fx_setup(gt_fx *f) {
    memset(&g_cap, 0, sizeof g_cap);
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    de_log_add_sink(capture_sink, NULL);
    ASSERT_EQ(de_fs_mount("game", DE_HOST_TEST_DATA_DIR, DE_MOUNT_READ_ONLY, 0), DE_OK);
    f->mounted = true;
}

static void gt_fx_teardown(gt_fx *f) {
    if (f->mounted)
        (void)de_fs_unmount("game", DE_HOST_TEST_DATA_DIR);
    de_log_shutdown();
}

static bool is_defaults(const de__game_config *c) {
    de__game_config d;
    de__game_config_defaults(&d);
    return memcmp(c, &d, sizeof d) == 0;
}

/* ---- Tests ------------------------------------------------------------------------------- */

TEST_F(game_toml, full, gt_fx) {
    de__game_config c;
    ASSERT_EQ(de__game_config_load("game:/full.toml", &c), DE_OK);
    EXPECT_STREQ(c.name, "Full Test");
    EXPECT_STREQ(c.library, "libfulltest.dylib");
    EXPECT_STREQ(c.window_title, "Full Test Window");
    EXPECT_EQ(c.window_width, 1920u);
    EXPECT_EQ(c.window_height, 1080u);
    EXPECT_EQ(c.window_mode, DE_WINDOW_BORDERLESS);
    EXPECT_FALSE(c.window_high_dpi);
    EXPECT_EQ(c.timing.mode, DE_TIMING_VARIABLE);
    EXPECT_EQ(c.timing.fixed_hz, 120u);
    EXPECT_EQ(c.timing.max_catchup_steps, 8u);
    EXPECT_FLOAT_EQ(c.timing.time_scale, 0.5, 0.0);
    EXPECT_EQ(c.timing.frame_cap_hz, 144u);
    EXPECT_EQ(c.task_workers, 3u);
    EXPECT_EQ(c.frame_arena_mib, 32u);
    EXPECT_EQ(c.asset_io_threads, 4u); /* DENG-143: [assets] */
    EXPECT_FALSE(c.asset_hot_reload);
    EXPECT_STREQ(c.asset_locale, "pt-BR");
    EXPECT_EQ(c.unknown_keys, 0u);
    EXPECT_EQ(c.cvars_applied, 0u);
    EXPECT_EQ(warnings(), 0u);

    /* The other enum names, and an integer where a float is expected. */
    static const char text[] = "[window]\nmode = 'fullscreen'\n[timing]\nmode = 'fixed'\n"
                               "time_scale = 2\n";
    ASSERT_EQ(de__game_config_parse(text, sizeof text - 1, NULL, &c), DE_OK);
    EXPECT_EQ(c.window_mode, DE_WINDOW_FULLSCREEN);
    EXPECT_EQ(c.timing.mode, DE_TIMING_FIXED);
    EXPECT_FLOAT_EQ(c.timing.time_scale, 2.0, 0.0);
}
TEST_F(game_toml, window_surface_auto_and_explicit_backends, gt_fx) {
    const char *names[] = {"auto", "none", "metal", "vulkan"};
    const de_window_surface surfaces[] = {DE_WINDOW_SURFACE_NONE, DE_WINDOW_SURFACE_NONE,
                                          DE_WINDOW_SURFACE_METAL, DE_WINDOW_SURFACE_VULKAN};
    for (u32 i = 0; i < 4; ++i) {
        char source[64];
        snprintf(source, sizeof source, "[window]\nsurface='%s'\n", names[i]);
        de__game_config config;
        ASSERT_EQ(de__game_config_parse(source, strlen(source), "inline", &config), DE_OK);
        EXPECT_EQ(config.window_surface_auto, i == 0);
        EXPECT_EQ(config.window_surface, surfaces[i]);
    }
    de__game_config config;
    const char *bad = "[window]\nsurface='opengl'";
    EXPECT_EQ(de__game_config_parse(bad, strlen(bad), "inline", &config), DE_ERR_PARSE);
    EXPECT_TRUE(strstr(de_error_last()->message, "window.surface") != NULL);
}

TEST_F(game_toml, defaults, gt_fx) {
    de__game_config c;
    /* The de_timing defaults of de_game.h. */
    de__game_config_defaults(&c);
    EXPECT_EQ(c.timing.mode, DE_TIMING_FIXED);
    EXPECT_EQ(c.timing.fixed_hz, 60u);
    EXPECT_EQ(c.timing.max_catchup_steps, 4u);
    EXPECT_FLOAT_EQ(c.timing.time_scale, 1.0, 0.0);
    EXPECT_EQ(c.timing.frame_cap_hz, 0u);
    EXPECT_EQ(c.timing.fixed_hz, DE_TIMING_DEFAULT_FIXED_HZ);
    EXPECT_EQ(c.timing.max_catchup_steps, DE_TIMING_DEFAULT_MAX_CATCHUP_STEPS);
    EXPECT_FLOAT_EQ(c.timing.time_scale, DE_TIMING_DEFAULT_TIME_SCALE, 0.0);
    EXPECT_EQ(c.timing.frame_cap_hz, DE_TIMING_DEFAULT_FRAME_CAP_HZ);
    EXPECT_STREQ(c.name, "DEngine");
    EXPECT_STREQ(c.library, "");
    EXPECT_STREQ(c.window_title, "");
    EXPECT_EQ(c.window_width, 1280u);
    EXPECT_EQ(c.window_height, 720u);
    EXPECT_EQ(c.window_mode, DE_WINDOW_WINDOWED);
    EXPECT_TRUE(c.window_high_dpi);
    EXPECT_EQ(c.task_workers, 0u);
    EXPECT_EQ(c.frame_arena_mib, 64u);
    EXPECT_EQ(c.asset_io_threads, 2u); /* DENG-143 */
    EXPECT_TRUE(c.asset_hot_reload);
    EXPECT_STREQ(c.asset_locale, "");

    /* A missing file gives the defaults (and an Info message, not a warning). */
    memset(&c, 0xAB, sizeof c);
    ASSERT_EQ(de__game_config_load("game:/no_such_file.toml", &c), DE_OK);
    EXPECT_TRUE(is_defaults(&c));
    /* An empty file and empty sections too. */
    ASSERT_EQ(de__game_config_parse("", 0, NULL, &c), DE_OK);
    EXPECT_TRUE(is_defaults(&c));
    ASSERT_EQ(de__game_config_parse(NULL, 0, NULL, &c), DE_OK);
    EXPECT_TRUE(is_defaults(&c));
    static const char empty[] =
        "[game]\n[window]\n[timing]\n[tasks]\n[memory]\n[assets]\n[cvars]\n";
    ASSERT_EQ(de__game_config_parse(empty, sizeof empty - 1, NULL, &c), DE_OK);
    EXPECT_TRUE(is_defaults(&c));
    EXPECT_EQ(warnings(), 0u);

    EXPECT_EQ(de__game_config_parse("", 0, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__game_config_parse(NULL, 3, NULL, &c), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__game_config_load(NULL, &c), DE_ERR_INVALID_ARGUMENT);
}

TEST_F(game_toml, bad_types, gt_fx) {
    static const struct {
        const char *text;
        const char *key; /* the message names it */
    } cases[] = {
        {"[timing]\nmode = 'warp'\n", "timing.mode"},
        {"[timing]\nmode = 1\n", "timing.mode"},
        {"[timing]\nfixed_hz = '60'\n", "timing.fixed_hz"},
        {"[timing]\nfixed_hz = 0\n", "timing.fixed_hz"},
        {"[timing]\nfixed_hz = 60.0\n", "timing.fixed_hz"},
        {"[timing]\nmax_catchup_steps = -1\n", "timing.max_catchup_steps"},
        {"[timing]\ntime_scale = 'fast'\n", "timing.time_scale"},
        {"[timing]\ntime_scale = -1.0\n", "timing.time_scale"},
        {"[timing]\ntime_scale = nan\n", "timing.time_scale"},
        {"[timing]\nframe_cap_hz = true\n", "timing.frame_cap_hz"},
        {"[window]\nwidth = 1.5\n", "window.width"},
        {"[window]\nheight = 0\n", "window.height"},
        {"[window]\nheight = 99999999999\n", "window.height"},
        {"[window]\nmode = 'tiny'\n", "window.mode"},
        {"[window]\nhigh_dpi = 1\n", "window.high_dpi"},
        {"[window]\ntitle = 3\n", "window.title"},
        {"[game]\nname = 3\n", "game.name"},
        {"[game]\nlibrary = ['a']\n", "game.library"},
        {"[tasks]\nworkers = -1\n", "tasks.workers"},
        {"[tasks]\nworkers = 'many'\n", "tasks.workers"},
        {"[memory]\nframe_arena_mib = 0\n", "memory.frame_arena_mib"},
        {"[assets]\nio_threads = 0\n", "assets.io_threads"}, /* DENG-143 */
        {"[assets]\nio_threads = 65\n", "assets.io_threads"},
        {"[assets]\nhot_reload = 'yes'\n", "assets.hot_reload"},
        {"[assets]\nlocale = 'a-tag-that-is-much-longer-than-35-bytes'\n", "assets.locale"},
        {"assets = 3\n", "assets"},
        {"window = 3\n", "window"},
        {"timing = 'fixed'\n", "timing"},
        {"[cvars]\nr.list = [1, 2]\n", "cvars.r.list"},
        {"[cvars]\nr.when = 1979-05-27\n", "cvars.r.when"},
        {"[[cvars.r.tables]]\nx = 1\n", "cvars.r.tables"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        de__game_config c;
        de_error_clear();
        de_result r = de__game_config_parse(cases[i].text, strlen(cases[i].text), "inline", &c);
        EXPECT_EQ(r, DE_ERR_PARSE);
        const de_error *err = de_error_last();
        ASSERT_TRUE(err != NULL);
        if (strstr(err->message, cases[i].key) == NULL)
            FAIL(err->message);       /* the message must name the key */
        EXPECT_TRUE(is_defaults(&c)); /* a failed parse leaves the defaults */
    }

    /* Not TOML at all. */
    de__game_config c;
    static const char junk[] = "[game\nname = \n";
    EXPECT_EQ(de__game_config_parse(junk, sizeof junk - 1, "inline", &c), DE_ERR_PARSE);

    /* From a file: the message names timing.mode, and the file's [cvars] were not applied. */
    de_cvar not_applied =
        de_cvar_register(&(const de_cvar_desc){.name = "test.gt.not_applied", .type = DE_CVAR_INT});
    ASSERT_TRUE(DE_HANDLE_VALID(not_applied));
    EXPECT_EQ(de__game_config_load("game:/bad_timing_mode.toml", &c), DE_ERR_PARSE);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_TRUE(strstr(de_error_last()->message, "timing.mode") != NULL);
    EXPECT_TRUE(strstr(de_error_last()->message, "bad_timing_mode.toml") != NULL);
    EXPECT_EQ(de_cvar_int(not_applied), 0);
    EXPECT_TRUE(is_defaults(&c));
}

TEST_F(game_toml, unknown_key_warns, gt_fx) {
    de__game_config c;
    ASSERT_EQ(de__game_config_load("game:/unknown_keys.toml", &c), DE_OK);
    EXPECT_EQ(c.unknown_keys, 3u);
    EXPECT_EQ(c.window_width, 800u); /* known keys beside them still apply */
    EXPECT_EQ(warnings(), 3u);
    EXPECT_TRUE(warned("'bar'"));
    EXPECT_TRUE(warned("'window.foo'"));
    EXPECT_TRUE(warned("'audio'"));
    EXPECT_TRUE(warned("unknown_keys.toml"));
}

TEST_F(game_toml, cvars_applied, gt_fx) {
    de_cvar flag =
        de_cvar_register(&(const de_cvar_desc){.name = "test.gt.flag", .type = DE_CVAR_BOOL});
    de_cvar count =
        de_cvar_register(&(const de_cvar_desc){.name = "test.gt.count", .type = DE_CVAR_INT});
    de_cvar scale =
        de_cvar_register(&(const de_cvar_desc){.name = "test.gt.scale", .type = DE_CVAR_FLOAT});
    de_cvar label = de_cvar_register(&(const de_cvar_desc){
        .name = "test.gt.label", .type = DE_CVAR_STRING, .default_string = "none"});
    de_cvar ro = de_cvar_register(&(const de_cvar_desc){
        .name = "test.gt.ro", .type = DE_CVAR_INT, .flags = DE_CVAR_READONLY});
    de_cvar bad = de_cvar_register(
        &(const de_cvar_desc){.name = "test.gt.bad", .type = DE_CVAR_INT, .default_number = 1});
    ASSERT_TRUE(DE_HANDLE_VALID(flag) && DE_HANDLE_VALID(count) && DE_HANDLE_VALID(scale) &&
                DE_HANDLE_VALID(label) && DE_HANDLE_VALID(ro) && DE_HANDLE_VALID(bad));

    de__game_config c;
    ASSERT_EQ(de__game_config_load("game:/cvars.toml", &c), DE_OK);
    EXPECT_STREQ(c.name, "Cvar Test");
    EXPECT_TRUE(de_cvar_bool(flag));
    EXPECT_EQ(de_cvar_int(count), 42);
    EXPECT_FLOAT_EQ(de_cvar_float(scale), 0.25, 0.0);
    EXPECT_STREQ(de_cvar_string(label), "hello world");
    EXPECT_EQ(de_cvar_int(ro), 7);  /* a startup value: read-only cvars take it before the lock */
    EXPECT_EQ(de_cvar_int(bad), 1); /* refused: the default stays, with a warning */
    EXPECT_TRUE(warned("cvars.test.gt.bad"));
    /* Registered after the load: the value waited as pending. Nested tables join with dots. */
    de_cvar later =
        de_cvar_register(&(const de_cvar_desc){.name = "test.gt.later", .type = DE_CVAR_INT});
    EXPECT_EQ(de_cvar_int(later), 3);
    de_cvar nested =
        de_cvar_register(&(const de_cvar_desc){.name = "test.gt2.nested", .type = DE_CVAR_INT});
    EXPECT_EQ(de_cvar_int(nested), 11);
    EXPECT_EQ(c.cvars_applied, 7u); /* every value except test.gt.bad */
}
