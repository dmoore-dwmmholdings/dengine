/*
 * test_nav_settings.c: the project nav settings file nav.toml (DENG-833;
 * src/gameplay/nav/nav_settings.c). CTest names nav_settings.<test>.
 *
 *   settings_default_roundtrip   the defaults format and parse back to the same settings
 *   settings_custom_roundtrip    agents, areas, bounds, tile size and auto rebake survive a file
 *   settings_rejects_bad         out-of-range and malformed files are refused, `out` untouched
 *   settings_runtime_load        the runtime path: load a file, take each agent's build desc
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "de_test.h"

#include <dengine/de_gameplay.h>
#include <dengine/de_platform.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

static void same(const de_nav_settings *a, const de_nav_settings *b) {
    ASSERT_EQ(a->agent_count, b->agent_count);
    for (u32 i = 0; i < a->agent_count; ++i) {
        EXPECT_EQ(strcmp(a->agents[i].name, b->agents[i].name), 0);
        EXPECT_TRUE(a->agents[i].radius == b->agents[i].radius);
        EXPECT_TRUE(a->agents[i].height == b->agents[i].height);
        EXPECT_TRUE(a->agents[i].max_slope_deg == b->agents[i].max_slope_deg);
        EXPECT_TRUE(a->agents[i].step_height == b->agents[i].step_height);
        EXPECT_TRUE(a->agents[i].cell_size == b->agents[i].cell_size);
        EXPECT_TRUE(a->agents[i].cell_height == b->agents[i].cell_height);
        EXPECT_EQ(a->agents[i].collision_mask, b->agents[i].collision_mask);
    }
    ASSERT_EQ(a->area_count, b->area_count);
    for (u32 i = 0; i < a->area_count; ++i) {
        EXPECT_EQ(a->areas[i].id, b->areas[i].id);
        EXPECT_EQ(strcmp(a->areas[i].name, b->areas[i].name), 0);
        EXPECT_TRUE(a->areas[i].cost == b->areas[i].cost);
    }
    for (u32 i = 0; i < 3; ++i) {
        EXPECT_TRUE(a->bounds_min[i] == b->bounds_min[i]);
        EXPECT_TRUE(a->bounds_max[i] == b->bounds_max[i]);
    }
    EXPECT_TRUE(a->tile_size == b->tile_size);
    EXPECT_EQ(a->auto_rebake, b->auto_rebake);
}

static void round_trip(const de_nav_settings *s) {
    static char buf[16384];
    size_t len = 0;
    ASSERT_EQ(de_nav_settings_format(s, buf, sizeof buf, &len), DE_OK);
    de_nav_settings back;
    ASSERT_EQ(de_nav_settings_parse(buf, len, &back), DE_OK);
    same(s, &back);
}

static de_nav_settings custom(void) {
    de_nav_settings s;
    de_nav_settings_default(&s);
    s.agent_count = 2;
    snprintf(s.agents[0].name, sizeof s.agents[0].name, "tank \"mk2\"\\x");
    s.agents[0].radius = 1.3f;
    s.agents[0].height = 2.6f;
    s.agents[0].max_slope_deg = 30.5f;
    s.agents[0].collision_mask = 0xA5u;
    snprintf(s.agents[1].name, sizeof s.agents[1].name, "rat");
    s.agents[1].radius = 0.15f;
    s.agents[1].height = 0.3f;
    s.agents[1].step_height = 0.05f;
    s.agents[1].cell_size = 0.1f;
    s.agents[1].cell_height = 0.05f;
    s.area_count = 3;
    s.areas[0] = (de_nav_area_settings){0, "ground", 1.0f};
    s.areas[1] = (de_nav_area_settings){7, "lava", 99.5f};
    s.areas[2] = (de_nav_area_settings){15, "door", 3.25f};
    s.bounds_min[0] = -64, s.bounds_min[1] = -16, s.bounds_min[2] = -5.5;
    s.bounds_max[0] = 128, s.bounds_max[1] = 48, s.bounds_max[2] = 20.25;
    s.tile_size = 16.0f;
    s.auto_rebake = false;
    return s;
}

TEST(nav_settings, settings_default_roundtrip) {
    de_nav_settings s;
    de_nav_settings_default(&s);
    ASSERT_EQ(de_nav_settings_validate(&s), DE_OK);
    round_trip(&s);
    /* An empty file is the defaults. */
    de_nav_settings e;
    ASSERT_EQ(de_nav_settings_parse("", 0, &e), DE_OK);
    same(&s, &e);
}

TEST(nav_settings, settings_custom_roundtrip) {
    const de_nav_settings s = custom();
    ASSERT_EQ(de_nav_settings_validate(&s), DE_OK);
    round_trip(&s);

    /* Through a real file. */
    char dir[] = "/tmp/de_nav_settings_XXXXXX";
    ASSERT_TRUE(mkdtemp(dir) != NULL);
    ASSERT_EQ(de_fs_mount("navset", dir, DE_MOUNT_WRITABLE, 0), DE_OK);
    de_nav_settings back;
    EXPECT_EQ(de_nav_settings_load("navset:/nav.toml", &back), DE_ERR_NOT_FOUND);
    de_error_clear();
    ASSERT_EQ(de_nav_settings_save("navset:/nav.toml", &s), DE_OK);
    ASSERT_EQ(de_nav_settings_load("navset:/nav.toml", &back), DE_OK);
    same(&s, &back);
    ASSERT_EQ(de_file_delete("navset:/nav.toml"), DE_OK);
}

TEST(nav_settings, settings_rejects_bad) {
    de_nav_settings out = custom();
    const de_nav_settings keep = out;
    static const char *const bad[] = {
        "tile_size = 7\n",                                  /* does not divide 256 */
        "[bounds]\nmin = [0, 0, 0]\nmax = [0, 5, 5]\n",     /* max not above min */
        "[bounds]\nmin = [1, 0, 0]\nmax = [9, 8, 5]\n",     /* min not on the tile grid */
        "[[agent]]\nname = \"\"\n",                         /* empty name */
        "[[agent]]\nname = \"a\"\n[[agent]]\nname = \"a\"\n", /* twice */
        "[[agent]]\nname = \"a\"\nradius = -1\n",           /* out of range */
        "[[area]]\nid = 1\nname = \"x\"\n",                 /* no area 0 */
        "[[area]]\nid = 0\nname = \"x\"\ncost = 0.5\n",     /* cost below 1 */
        "auto_rebake = 3\n",                                /* wrong type */
        "tile_size = = 3\n",                                /* not TOML */
    };
    for (u32 i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        EXPECT_TRUE(de_nav_settings_parse(bad[i], strlen(bad[i]), &out) != DE_OK);
        de_error_clear();
        same(&keep, &out);
    }
    de_nav_settings s = custom();
    s.tile_size = 5.0f;
    char buf[8192];
    EXPECT_EQ(de_nav_settings_format(&s, buf, sizeof buf, NULL), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    s = custom();
    EXPECT_EQ(de_nav_settings_format(&s, buf, 40, NULL), DE_ERR_OUT_OF_MEMORY);
    de_error_clear();
}

TEST(nav_settings, settings_runtime_load) {
    const de_nav_settings s = custom();
    char dir[] = "/tmp/de_nav_settings_XXXXXX";
    ASSERT_TRUE(mkdtemp(dir) != NULL);
    ASSERT_EQ(de_fs_mount("navrt", dir, DE_MOUNT_WRITABLE, 0), DE_OK);
    ASSERT_EQ(de_nav_settings_save("navrt:/nav.toml", &s), DE_OK);

    de_nav_settings rt;
    ASSERT_EQ(de_nav_settings_load("navrt:/nav.toml", &rt), DE_OK);
    de_navmesh_build_desc d;
    ASSERT_EQ(de_nav_settings_build_desc(&rt, 1, &d), DE_OK);
    EXPECT_TRUE(d.agent_radius == 0.15f);
    EXPECT_TRUE(d.agent_max_climb == 0.05f);
    EXPECT_TRUE(d.tile_size == 16.0f);
    ASSERT_EQ(de_nav_settings_build_desc(&rt, 0, &d), DE_OK);
    EXPECT_EQ(d.collision_mask, 0xA5u);
    EXPECT_TRUE(fabsf(d.agent_max_slope_rad - 30.5f * 3.14159265f / 180.0f) < 1e-5f);
    EXPECT_EQ(de_nav_settings_build_desc(&rt, 2, &d), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    ASSERT_EQ(de_file_delete("navrt:/nav.toml"), DE_OK);
}
