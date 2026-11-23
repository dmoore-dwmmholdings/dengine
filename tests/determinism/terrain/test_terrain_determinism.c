/* DENG-580: a fixed script of terrain strokes (every tool), a bake and a heightmap import on the
 * CPU terrain model; the FNV-1a hash of the component after each step must match the golden file
 * on every platform. Regenerate the golden with DE_DET_UPDATE_TERRAIN_GOLDEN=1, only for an
 * intended change of the model. */
#include "de_det_harness.h"
#include "de_test.h"
#include "world/terrain/terrain.h"

#include <stdlib.h>
#include <string.h>

#define SEED  0x580u
#define STEPS 16u

static u64 hash_of(const DeTerrain *t) {
    u64 h = 0xcbf29ce484222325ull;
    /* The grid arrays only: the struct's padding bytes are not part of the model. */
    const u8 *parts[] = {(const u8 *)t->heights, t->splat, t->holes};
    const size_t sizes[] = {sizeof t->heights, sizeof t->splat, sizeof t->holes};
    for (u32 k = 0; k < 3; ++k)
        for (size_t i = 0; i < sizes[k]; ++i)
            h = (h ^ parts[k][i]) * 0x100000001b3ull;
    for (u32 r = 0; r < DE_TERRAIN_RULES; ++r)
        h = (h ^ (u64)t->rules[r].enabled) * 0x100000001b3ull;
    return h;
}

static void init(DeTerrain *t) {
    memset(t, 0, sizeof *t);
    t->resolution = DE_TERRAIN_RES;
    t->size = 32.0f;
    t->height = 32.0f;
    for (u32 i = 0; i < DE_TERRAIN_SAMPLES; ++i)
        t->splat[i * DE_TERRAIN_LAYERS] = 255;
    for (u32 l = 0; l < DE_TERRAIN_LAYERS; ++l)
        t->order[l] = (u8)l;
}

static void run(u64 *out) {
    static DeTerrain t;
    init(&t);
    static const f32 falloff[] = {1.0f, 0.8f, 0.3f, 0.0f};
    static const char *const tools[] = {"raise",  "raise", "lower",          "smooth",
                                        "flatten", "ramp",  "noise",          "terrace",
                                        "erodeHydraulic", "erodeThermal", "stamp", "paint",
                                        "paint",  "hole"};
    f32 stamp_v[64];
    for (u32 i = 0; i < 64; ++i)
        stamp_v[i] = (f32)((i * 37u) % 64u) / 63.0f;
    const de_terrain_grid stamp = {8, 8, stamp_v};
    u32 step = 0;
    for (u32 k = 0; k < sizeof tools / sizeof tools[0]; ++k) {
        /* Overlapping dabs, so each tool works on the terrain the earlier ones shaped. */
        const f32 cx = 12.0f + (f32)((k * 3u) % 8u) + 0.37f, cz = 11.0f + (f32)((k * 5u) % 8u) + 0.61f;
        const f32 points[12] = {cx, 0, cz, 1.0f, cx + 1.3f, 0, cz + 0.7f, 0.75f, cx + 2.9f, 0,
                                cz + 1.1f, 0.5f};
        de_terrain_stroke s;
        memset(&s, 0, sizeof s);
        s.tool = tools[k];
        s.tool_len = strlen(tools[k]);
        s.points = points;
        s.point_count = 12;
        s.radius = 3.5f + 0.25f * (f32)k;
        s.strength = 0.8f;
        if (k % 2 == 0) {
            s.falloff = falloff;
            s.falloff_count = 4;
        }
        s.has_target = true;
        s.target = 4.5f;
        s.has_step = true;
        s.step = 1.25f;
        s.has_seed = true;
        s.seed = (u32)SEED + k;
        s.has_scale = true;
        s.scale = 6.0f;
        s.has_rotation = true;
        s.rotation = 30.0f;
        s.has_iterations = true;
        s.iterations = 8;
        s.has_layer = true;
        s.layer = k == 12 ? 2u : 1u;
        s.area_tile = k == 8;
        char err[128] = "";
        ASSERT_TRUE(de__terrain_stroke(&t, 0.0, 0.0, &s, k == 10 ? &stamp : NULL, err, sizeof err));
        out[step++] = hash_of(&t);
    }
    t.rules[0] = (de_terrain_rule){.enabled = true, .layer = 3, .min_slope = 10, .max_slope = 90};
    de__terrain_bake(&t);
    out[step++] = hash_of(&t);
    de__terrain_import(&t, &stamp, 1, 1, false);
    out[step++] = hash_of(&t);
    ASSERT_EQ(step, STEPS);
}

TEST(terrain_determinism, repeat_runs_match) {
    u64 a[STEPS], b[STEPS];
    run(a);
    run(b);
    EXPECT_EQ(de_det_report("terrain run 1 vs 2", a, b, STEPS), DE_DET_NO_DIVERGENCE);
}

TEST(terrain_determinism, strokes_match_golden) {
    u64 actual[STEPS];
    run(actual);
    const char *update = getenv("DE_DET_UPDATE_TERRAIN_GOLDEN");
    if (update != NULL && update[0] == '1')
        ASSERT_EQ(de_det_write_checksums(DE_TERRAIN_DET_GOLDEN, "terrain", SEED, actual, STEPS),
                  DE_OK);
    ASSERT_EQ(de_det_write_checksums(DE_DET_OUT_DIR "/terrain.checksums", "terrain", SEED, actual,
                                     STEPS),
              DE_OK);
    u64 golden[STEPS];
    u32 count = 0;
    ASSERT_EQ(de_det_read_checksums(DE_TERRAIN_DET_GOLDEN, golden, STEPS, &count), DE_OK);
    ASSERT_EQ(count, STEPS);
    EXPECT_EQ(de_det_report("terrain vs golden", golden, actual, STEPS), DE_DET_NO_DIVERGENCE);
}
