/*
 * test_nav_settings.c: the project nav settings file nav.toml (DENG-833;
 * src/gameplay/nav/nav_settings.c). CTest names nav_settings.<test>.
 *
 *   settings_default_roundtrip   the defaults format and parse back to the same settings
 *   settings_custom_roundtrip    agents, areas, bounds, tile size and auto rebake survive a file
 *   settings_rejects_bad         out-of-range and malformed files are refused, `out` untouched
 *   settings_runtime_load        the runtime path: load a file, take each agent's build desc
 *   tiles_format                 DENG-864: nav.tiles round-trips; malformed files are refused
 *   tiles_runtime_load           DENG-864: de_nav_settings_load_tiles by agent type name
 *   tiles_scene_path             DENG-877: <scene>.nav.tiles beside a scene file
 *   tiles_fp_seed_compat         DENG-877: the shared fingerprint seed hashes DENG-864's bytes
 *   tiles_runtime_check          DENG-877: de_nav_settings_check_tiles: match, stale, other grid
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "de_test.h"

#include "gameplay/nav/nav_build.h"

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

/* ---- DENG-864: nav.tiles ---- */

/* A one-agent file: three records and a DNAV container with no tile (valid, header only). */
static void tiles_file(const char *name, f32 tile_size, u8 **out, u64 *size, u8 **dnav,
                       u64 *dnav_size) {
    de_nav_settings s;
    de_nav_settings_default(&s);
    s.tile_size = tile_size;
    de__nav_container_info info;
    memset(&info, 0, sizeof info);
    ASSERT_EQ(de_nav_settings_build_desc(&s, 0, &info.desc), DE_OK);
    ASSERT_EQ(de__nav_container_write(de_heap_allocator(), &info, NULL, 0, dnav, dnav_size), DE_OK);
    static const de__nav_tile_record recs[3] = {
        {1, 10, 0, 0x1111}, {5, 20, 2, 0x2222}, {15, 0, 0, 0xFFFFFFFFFFFFFFFFull}};
    de__nav_tiles_agent g;
    memset(&g, 0, sizeof g);
    snprintf(g.name, sizeof g.name, "%s", name);
    g.bmin_x = -16, g.bmin_y = -16;
    g.tile_size = tile_size;
    g.tiles_x = 4, g.tiles_y = 4;
    g.record_count = 3;
    g.records = recs;
    g.dnav = *dnav;
    g.dnav_size = *dnav_size;
    ASSERT_EQ(de__nav_tiles_write(de_heap_allocator(), &g, 1, out, size), DE_OK);
}

TEST(nav_settings, tiles_format) {
    u8 *file = NULL, *dnav = NULL;
    u64 size = 0, dsize = 0;
    tiles_file("humanoid", 8.0f, &file, &size, &dnav, &dsize);
    EXPECT_EQ(size, 16u + 80u + 3u * 24u + ((dsize + 7u) & ~(u64)7u));
    u32 count = 0;
    const char *why = NULL;
    ASSERT_TRUE(de__nav_tiles_read(file, size, &count, &why));
    ASSERT_EQ(count, 1u);
    de__nav_tiles_agent g;
    ASSERT_TRUE(de__nav_tiles_agent_at(file, size, 0, &g));
    EXPECT_EQ(strcmp(g.name, "humanoid"), 0);
    EXPECT_TRUE(g.bmin_x == -16.0 && g.bmin_y == -16.0 && g.tile_size == 8.0f);
    EXPECT_EQ(g.tiles_x, 4u);
    EXPECT_EQ(g.record_count, 3u);
    EXPECT_EQ(g.dnav_size, dsize);
    de__nav_tile_record r;
    de__nav_tiles_record(&g, 1, &r);
    EXPECT_EQ(r.tile, 5u);
    EXPECT_EQ(r.polys, 20u);
    EXPECT_EQ(r.links, 2u);
    EXPECT_EQ(r.fp, 0x2222u);
    de__nav_tiles_record(&g, 2, &r);
    EXPECT_EQ(r.fp, 0xFFFFFFFFFFFFFFFFull);
    EXPECT_FALSE(de__nav_tiles_agent_at(file, size, 1, &g));

    /* Malformed copies are refused. */
    u8 *bad = (u8 *)malloc((size_t)size + 8);
    ASSERT_TRUE(bad != NULL);
    memcpy(bad, file, (size_t)size);
    EXPECT_FALSE(de__nav_tiles_read(bad, 8, &count, &why));        /* truncated header */
    EXPECT_FALSE(de__nav_tiles_read(bad, size - 8, &count, &why)); /* truncated agent */
    memset(bad + size, 0, 8);
    EXPECT_FALSE(de__nav_tiles_read(bad, size + 8, &count, &why)); /* trailing bytes */
    EXPECT_EQ(strcmp(why, "nav.tiles: trailing bytes"), 0);
    bad[0] ^= 1u; /* magic */
    EXPECT_FALSE(de__nav_tiles_read(bad, size, &count, &why));
    bad[0] ^= 1u;
    bad[4] = 2; /* version */
    EXPECT_FALSE(de__nav_tiles_read(bad, size, &count, &why));
    bad[4] = 1;
    bad[16 + 80 + 24] = 0; /* record 1 tile 5 -> 0: below record 0 */
    EXPECT_FALSE(de__nav_tiles_read(bad, size, &count, &why));
    EXPECT_EQ(strcmp(why, "nav.tiles: bad tile record"), 0);
    bad[16 + 80 + 24] = 16; /* tile 16 of 16 */
    EXPECT_FALSE(de__nav_tiles_read(bad, size, &count, &why));
    bad[16 + 80 + 24] = 5;
    bad[16] = 0; /* empty name */
    EXPECT_FALSE(de__nav_tiles_read(bad, size, &count, &why));
    bad[16] = 'h';
    bad[16 + 60] = 0; /* tiles_x 0 */
    EXPECT_FALSE(de__nav_tiles_read(bad, size, &count, &why));
    bad[16 + 60] = 4;
    bad[16 + 80 + 72] = 0xFF; /* the DNAV magic */
    EXPECT_FALSE(de__nav_tiles_read(bad, size, &count, &why));
    memcpy(bad, file, (size_t)size);
    ASSERT_TRUE(de__nav_tiles_read(bad, size, &count, &why));
    free(bad);

    /* Two agents of one name are refused. */
    de__nav_tiles_agent two[2];
    ASSERT_TRUE(de__nav_tiles_agent_at(file, size, 0, &two[0]));
    two[0].records = NULL;
    two[0].record_count = 0;
    two[1] = two[0];
    u8 *dup = NULL;
    u64 dup_size = 0;
    ASSERT_EQ(de__nav_tiles_write(de_heap_allocator(), two, 2, &dup, &dup_size), DE_OK);
    EXPECT_FALSE(de__nav_tiles_read(dup, dup_size, &count, &why));
    EXPECT_EQ(strcmp(why, "nav.tiles: duplicate agent name"), 0);
    DE_FREE(de_heap_allocator(), dup, (size_t)dup_size);

    /* The path beside nav.toml. */
    char path[64];
    ASSERT_TRUE(de__nav_tiles_path("project:/nav.toml", path, sizeof path));
    EXPECT_EQ(strcmp(path, "project:/nav.tiles"), 0);
    ASSERT_TRUE(de__nav_tiles_path("x:/navsettings", path, sizeof path));
    EXPECT_EQ(strcmp(path, "x:/navsettings.tiles"), 0);
    EXPECT_FALSE(de__nav_tiles_path("project:/nav.toml", path, 8));

    DE_FREE(de_heap_allocator(), file, (size_t)size);
    DE_FREE(de_heap_allocator(), dnav, (size_t)dsize);
}

TEST(nav_settings, tiles_runtime_load) {
    char dir[] = "/tmp/de_nav_tiles_XXXXXX";
    ASSERT_TRUE(mkdtemp(dir) != NULL);
    ASSERT_EQ(de_fs_mount("navtl", dir, DE_MOUNT_WRITABLE, 0), DE_OK);
    u8 *file = NULL, *dnav = NULL;
    u64 size = 0, dsize = 0;
    tiles_file("humanoid", 8.0f, &file, &size, &dnav, &dsize);
    ASSERT_EQ(de_file_write_all_atomic("navtl:/nav.tiles", file, size), DE_OK);
    DE_FREE(de_heap_allocator(), file, (size_t)size);
    DE_FREE(de_heap_allocator(), dnav, (size_t)dsize);

    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(de_heap_allocator(), NULL, &n), DE_OK);
    EXPECT_EQ(de_nav_settings_load_tiles(n, "navtl:/nav.tiles", "humanoid"), DE_OK);
    EXPECT_EQ(de_nav_settings_load_tiles(n, "navtl:/nav.tiles", "tank"), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_nav_settings_load_tiles(n, "navtl:/none.tiles", "humanoid"), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_nav_settings_load_tiles(NULL, "navtl:/nav.tiles", "humanoid"),
              DE_ERR_INVALID_ARGUMENT);
    /* A file baked with another tile size does not fit this navmesh. */
    tiles_file("humanoid", 16.0f, &file, &size, &dnav, &dsize);
    ASSERT_EQ(de_file_write_all_atomic("navtl:/nav16.tiles", file, size), DE_OK);
    DE_FREE(de_heap_allocator(), file, (size_t)size);
    DE_FREE(de_heap_allocator(), dnav, (size_t)dsize);
    EXPECT_EQ(de_nav_settings_load_tiles(n, "navtl:/nav16.tiles", "humanoid"),
              DE_ERR_INVALID_ARGUMENT);
    /* A malformed file. */
    ASSERT_EQ(de_file_write_all_atomic("navtl:/bad.tiles", "DNVT", 4), DE_OK);
    EXPECT_EQ(de_nav_settings_load_tiles(n, "navtl:/bad.tiles", "humanoid"), DE_ERR_PARSE);
    de_error_clear();
    de_nav_destroy(n);
    ASSERT_EQ(de_file_delete("navtl:/nav.tiles"), DE_OK);
    ASSERT_EQ(de_file_delete("navtl:/nav16.tiles"), DE_OK);
    ASSERT_EQ(de_file_delete("navtl:/bad.tiles"), DE_OK);
}

/* ---- DENG-877: per-scene files and the runtime check ---- */

TEST(nav_settings, tiles_scene_path) {
    char p[64];
    ASSERT_TRUE(de_nav_scene_tiles_path("project:/scenes/main.scene", p, sizeof p));
    EXPECT_EQ(strcmp(p, "project:/scenes/main.nav.tiles"), 0);
    ASSERT_TRUE(de_nav_scene_tiles_path("project:/level", p, sizeof p));
    EXPECT_EQ(strcmp(p, "project:/level.nav.tiles"), 0);
    ASSERT_TRUE(de_nav_scene_tiles_path("a.scene", p, sizeof p));
    EXPECT_EQ(strcmp(p, "a.nav.tiles"), 0);
    EXPECT_FALSE(de_nav_scene_tiles_path("project:/scenes/main.scene", p, 12));
    EXPECT_FALSE(de_nav_scene_tiles_path(NULL, p, sizeof p));
    EXPECT_FALSE(de_nav_scene_tiles_path("", p, sizeof p));
}

static u64 fnv_ref(u64 h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

/* The seed hashes the bytes DENG-864's editor hashed, so files saved then keep matching. */
TEST(nav_settings, tiles_fp_seed_compat) {
    de_nav_settings s;
    de_nav_settings_default(&s);
    const de_nav_agent_settings *a = &s.agents[0];
    u64 h = 0xcbf29ce484222325ull;
    h = fnv_ref(h, &a->radius, sizeof(f32) * 6);
    h = fnv_ref(h, &a->collision_mask, sizeof(u32));
    h = fnv_ref(h, &s.tile_size, sizeof(f32));
    for (u32 i = 0; i < s.area_count; ++i) {
        h = fnv_ref(h, &s.areas[i].id, sizeof(u32));
        h = fnv_ref(h, &s.areas[i].cost, sizeof(f32));
    }
    EXPECT_EQ(de__nav_fp_seed(a, s.tile_size, s.areas, s.area_count), h);
    de__nav_fp_grid g;
    de__nav_fp_grid_of(&s, &g);
    EXPECT_EQ(g.tiles_x, 8u);
    EXPECT_EQ(g.tiles_y, 8u);
    s.bounds_max[0] = 33; /* a partial tile counts */
    de__nav_fp_grid_of(&s, &g);
    EXPECT_EQ(g.tiles_x, 9u);
}

/* A default-settings file whose records carry fingerprints `fp` for tiles `tiles`. */
static void check_file(const char *vpath, const de_nav_settings *s, const u32 *tiles,
                       const u64 *fp, u32 count) {
    de__nav_container_info info;
    memset(&info, 0, sizeof info);
    u8 *dnav = NULL, *file = NULL;
    u64 dsize = 0, size = 0;
    ASSERT_EQ(de_nav_settings_build_desc(s, 0, &info.desc), DE_OK);
    ASSERT_EQ(de__nav_container_write(de_heap_allocator(), &info, NULL, 0, &dnav, &dsize), DE_OK);
    de__nav_tile_record recs[8];
    for (u32 i = 0; i < count; ++i)
        recs[i] = (de__nav_tile_record){tiles[i], 1, 0, fp[i]};
    de__nav_fp_grid grid;
    de__nav_fp_grid_of(s, &grid);
    de__nav_tiles_agent g;
    memset(&g, 0, sizeof g);
    snprintf(g.name, sizeof g.name, "%s", s->agents[0].name);
    g.bmin_x = grid.bmin_x, g.bmin_y = grid.bmin_y;
    g.tile_size = grid.tile_size;
    g.tiles_x = grid.tiles_x, g.tiles_y = grid.tiles_y;
    g.record_count = count;
    g.records = recs;
    g.dnav = dnav;
    g.dnav_size = dsize;
    ASSERT_EQ(de__nav_tiles_write(de_heap_allocator(), &g, 1, &file, &size), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic(vpath, file, size), DE_OK);
    DE_FREE(de_heap_allocator(), file, (size_t)size);
    DE_FREE(de_heap_allocator(), dnav, (size_t)dsize);
}

TEST(nav_settings, tiles_runtime_check) {
    char dir[] = "/tmp/de_nav_check_XXXXXX";
    ASSERT_TRUE(mkdtemp(dir) != NULL);
    ASSERT_EQ(de_fs_mount("navck", dir, DE_MOUNT_WRITABLE, 0), DE_OK);
    de_nav_settings s;
    de_nav_settings_default(&s);
    const u64 seed = de__nav_fp_seed(&s.agents[0], s.tile_size, s.areas, s.area_count);
    /* Tile 27 (x 3, y 3) holds the world origin; tile 0 is a corner. */
    const u32 tiles[2] = {0, 27};
    const u64 fps[2] = {seed, seed};
    check_file("navck:/main.nav.tiles", &s, tiles, fps, 2);

    de_nav *n = NULL;
    ASSERT_EQ(de_nav_create(de_heap_allocator(), NULL, &n), DE_OK);
    const char *agent = s.agents[0].name;
    de_nav_tiles_check c;
    ASSERT_EQ(de_nav_settings_check_tiles(n, &s, "navck:/main.nav.tiles", agent, &c), DE_OK);
    EXPECT_TRUE(c.grid_match);
    EXPECT_EQ(c.tiles, 64u);
    EXPECT_EQ(c.saved, 2u);
    EXPECT_EQ(c.stale, 0u);
    EXPECT_EQ(c.missing, 62u);

    /* A link at the origin changes the inputs of the tiles around it, not of the corner. */
    const de_dvec3 from = {0, 0, 0}, to = {1, 0, 0};
    ASSERT_EQ(de_nav_add_offmesh_link(n, from, to, 0.5f, true, 0), DE_OK);
    ASSERT_EQ(de_nav_settings_check_tiles(n, &s, "navck:/main.nav.tiles", agent, &c), DE_OK);
    EXPECT_TRUE(c.grid_match);
    EXPECT_EQ(c.stale, 1u);

    /* Other settings: another grid, or the same grid with other agent settings. */
    de_nav_settings t = s;
    t.tile_size = 16.0f;
    ASSERT_EQ(de_nav_settings_check_tiles(n, &t, "navck:/main.nav.tiles", agent, &c), DE_OK);
    EXPECT_FALSE(c.grid_match);
    EXPECT_EQ(c.stale, 2u);
    EXPECT_EQ(c.missing, 16u);
    t = s;
    t.agents[0].radius = 0.6f;
    ASSERT_EQ(de_nav_settings_check_tiles(n, &t, "navck:/main.nav.tiles", agent, &c), DE_OK);
    EXPECT_FALSE(c.grid_match);

    /* Errors. */
    EXPECT_EQ(de_nav_settings_check_tiles(n, &s, "navck:/main.nav.tiles", "tank", &c),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_nav_settings_check_tiles(n, &s, "navck:/none.nav.tiles", agent, &c),
              DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_nav_settings_check_tiles(NULL, &s, "navck:/main.nav.tiles", agent, &c),
              DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    de_nav_destroy(n);
    ASSERT_EQ(de_file_delete("navck:/main.nav.tiles"), DE_OK);
}
