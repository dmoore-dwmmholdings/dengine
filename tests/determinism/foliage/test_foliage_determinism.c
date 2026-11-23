/* DENG-581: a fixed script of foliage paints, erases and scatters on the CPU foliage model, over a
 * shaped terrain and a plane; the FNV-1a hash of the instance store after each op must match the
 * golden file on every platform. Regenerate the golden with DE_DET_UPDATE_FOLIAGE_GOLDEN=1, only
 * for an intended change of the model. */
#include "de_det_harness.h"
#include "de_test.h"
#include "dengine/de_memory.h"
#include "world/foliage/foliage.h"
#include "world/terrain/terrain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEED  0x581u
#define STEPS 10u

static const char k_grass[] = "mesh = \"mesh:grass\"\n"
                              "density = 4.0\n"
                              "scale = [0.8, 1.2]\n"
                              "rotation = [0.0, 360.0]\n"
                              "alignToNormal = true\n"
                              "slope = [0.0, 40.0]\n"
                              "height = [0.0, 0.0]\n"
                              "minSpacing = 0.2\n"
                              "collision = false\n"
                              "cullDistance = 60.0\n";
static const char k_pine[] = "mesh = \"mesh:pine\"\n"
                             "impostor = \"tex:pine_impostor\"\n"
                             "impostorDistance = 80.0\n"
                             "density = 0.05\n"
                             "scale = [0.9, 1.4]\n"
                             "rotation = [0.0, 360.0]\n"
                             "alignToNormal = false\n"
                             "slope = [0.0, 30.0]\n"
                             "height = [0.0, 0.0]\n"
                             "minSpacing = 3.0\n"
                             "collision = true\n"
                             "cullDistance = 400.0\n";

static de_foliage_type g_types[2];
static DeTerrain g_terrain;

static de_foliage_error type_of(void *user, u32 slot, de_foliage_type *out, char *msg,
                                size_t cap) {
    (void)user;
    if (slot >= 2) {
        snprintf(msg, cap, "no type in slot %u", (unsigned)slot);
        return DE_FOLIAGE_NOT_FOUND;
    }
    *out = g_types[slot];
    return DE_FOLIAGE_OK;
}

static de_foliage_error surface_of(void *user, de_entity e, de_foliage_surface *out, char *msg,
                                   size_t cap) {
    (void)user;
    (void)msg;
    (void)cap;
    memset(out, 0, sizeof *out);
    if (e.bits == 1u)
        out->terrain = &g_terrain;
    return DE_FOLIAGE_OK;
}

static u64 mix(u64 h, const void *p, size_t n) {
    const u8 *b = (const u8 *)p;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

/* Field by field: the structs' padding bytes are not part of the model. DENG-823: the same bytes as before the paged store (cells in z, x order, then every instance in
 * cell order), so the golden did not change. */
static u64 hash_of(const de_foliage_store *fo) {
    u64 h = 0xcbf29ce484222325ull;
    h = mix(h, &fo->cell_count, sizeof fo->cell_count);
    h = mix(h, &fo->instance_count, sizeof fo->instance_count);
    for (u32 c = 0; c < fo->cell_count; ++c) {
        h = mix(h, &fo->cells[c].x, 4);
        h = mix(h, &fo->cells[c].z, 4);
        h = mix(h, &fo->cells[c].count, 4);
    }
    for (u32 c = 0; c < fo->cell_count; ++c) {
        for (u32 i = 0; i < fo->cells[c].count; ++i) {
            const de_foliage_instance *in = &fo->cells[c].inst[i];
            h = mix(h, &in->x, 4);
            h = mix(h, &in->y, 4);
            h = mix(h, &in->z, 4);
            h = mix(h, &in->yaw, 4);
            h = mix(h, &in->scale, 4);
            h = mix(h, &in->rule, 4);
            h = mix(h, &in->type, 1);
        }
    }
    return h;
}

static void init_terrain(void) {
    DeTerrain *t = &g_terrain;
    memset(t, 0, sizeof *t);
    t->resolution = DE_TERRAIN_RES;
    t->size = 32.0f;
    t->height = 32.0f;
    for (u32 i = 0; i < DE_TERRAIN_SAMPLES; ++i)
        t->splat[i * DE_TERRAIN_LAYERS] = 255;
    for (u32 l = 0; l < DE_TERRAIN_LAYERS; ++l)
        t->order[l] = (u8)l;
    /* A hill, so slopes and heights vary. */
    const f32 points[4] = {16.0f, 0, 16.0f, 1.0f};
    de_terrain_stroke s;
    memset(&s, 0, sizeof s);
    s.tool = "raise";
    s.tool_len = 5;
    s.points = points;
    s.point_count = 4;
    s.radius = 10.0f;
    s.strength = 1.0f;
    char err[128] = "";
    ASSERT_TRUE(de__terrain_stroke(t, 0.0, 0.0, &s, NULL, err, sizeof err));
}

static void run(u64 *out) {
    char msg[256] = "";
    ASSERT_TRUE(de__foliage_parse_type("foliage/grass", k_grass, sizeof k_grass - 1, &g_types[0],
                                       msg, sizeof msg));
    ASSERT_TRUE(de__foliage_parse_type("foliage/pine", k_pine, sizeof k_pine - 1, &g_types[1],
                                       msg, sizeof msg));
    init_terrain();
    static DeFoliage fo;
    memset(&fo, 0, sizeof fo);
    de_foliage_store st;
    de__foliage_store_init(&st, de_heap_allocator());
    const de_foliage_env env = {NULL, type_of, surface_of};
    const de_foliage_surface surfaces[2] = {{&g_terrain, 0.0, 0.0, 0.0}, {NULL, 0.0, 1.5, 0.0}};
    static const f32 falloff[] = {1.0f, 0.7f, 0.2f, 0.0f};
    u32 step = 0;

    /* Paints over the hill and across a cell edge, one with each type. */
    for (u32 k = 0; k < 4; ++k) {
        const f32 cx = 6.0f + 5.5f * (f32)k + 0.37f, cz = 9.0f + 3.0f * (f32)k + 0.61f;
        const f32 points[8] = {cx, 0, cz, 1.0f, cx + 1.7f, 0, cz + 0.9f, 0.6f};
        const de_foliage_stroke s = {.has_type = true,
                                     .type = k == 3 ? 1u : 0u,
                                     .points = points,
                                     .point_count = 8,
                                     .radius = 2.5f + 0.5f * (f32)k,
                                     .strength = 0.6f,
                                     .falloff = k % 2 ? falloff : NULL,
                                     .falloff_count = k % 2 ? 4u : 0u,
                                     .has_density = k >= 2,
                                     .density = 1.5f,
                                     .seed = (u32)SEED + k};
        ASSERT_EQ(de__foliage_paint(&st, &env, &s, surfaces, 2, msg, sizeof msg), DE_FOLIAGE_OK);
        out[step++] = hash_of(&st);
    }
    /* Erases: partial with a type, then any type. */
    for (u32 k = 0; k < 2; ++k) {
        const f32 points[4] = {12.0f + 5.5f * (f32)k, 0, 12.0f + 3.5f * (f32)k, 1.0f};
        const de_foliage_stroke s = {.has_type = k == 0,
                                     .type = 0,
                                     .points = points,
                                     .point_count = 4,
                                     .radius = 3.0f,
                                     .strength = k == 0 ? 0.5f : 1.0f,
                                     .seed = (u32)SEED};
        ASSERT_EQ(de__foliage_erase(&st, &s, msg, sizeof msg), DE_FOLIAGE_OK);
        out[step++] = hash_of(&st);
    }
    /* Scatter rules on the terrain; re-running them; clearing one. */
    for (u32 r = 0; r < 2; ++r) {
        de_foliage_rule *rule = &fo.rules[r];
        memset(rule, 0, sizeof *rule);
        rule->id = r + 1u;
        rule->enabled = true;
        rule->type = (u8)r;
        rule->terrain = (de_entity){1u};
        rule->layer = -1;
        rule->min_weight = 0.5f;
        rule->max_slope = r == 0 ? 35.0f : 0.0f;
        rule->noise_scale = r == 0 ? 6.0f : 0.0f;
        rule->noise_threshold = 0.4f;
        rule->density = r == 0 ? 0.3f : 0.02f;
        rule->seed = (u32)SEED * 7u + r;
        rule->area_min = (de_vec2){2.0f, 2.0f};
        rule->area_max = (de_vec2){30.0f, 30.0f};
    }
    ASSERT_EQ(de__foliage_scatter(&st, fo.rules, &env, NULL, 0, false, msg, sizeof msg), DE_FOLIAGE_OK);
    out[step++] = hash_of(&st);
    const u32 one = 1u;
    ASSERT_EQ(de__foliage_scatter(&st, fo.rules, &env, &one, 1, false, msg, sizeof msg), DE_FOLIAGE_OK);
    out[step++] = hash_of(&st);
    const u32 two = 2u;
    ASSERT_EQ(de__foliage_scatter(&st, fo.rules, &env, &two, 1, true, msg, sizeof msg), DE_FOLIAGE_OK);
    out[step++] = hash_of(&st);
    ASSERT_EQ(de__foliage_scatter(&st, fo.rules, &env, NULL, 0, false, msg, sizeof msg), DE_FOLIAGE_OK);
    out[step++] = hash_of(&st);
    ASSERT_TRUE(de__foliage_valid(&st));
    de__foliage_store_free(&st);
    ASSERT_EQ(step, STEPS);
}

TEST(foliage_determinism, repeat_runs_match) {
    u64 a[STEPS], b[STEPS];
    run(a);
    run(b);
    EXPECT_EQ(de_det_report("foliage run 1 vs 2", a, b, STEPS), DE_DET_NO_DIVERGENCE);
}

TEST(foliage_determinism, ops_match_golden) {
    u64 actual[STEPS];
    run(actual);
    const char *update = getenv("DE_DET_UPDATE_FOLIAGE_GOLDEN");
    if (update != NULL && update[0] == '1')
        ASSERT_EQ(de_det_write_checksums(DE_FOLIAGE_DET_GOLDEN, "foliage", SEED, actual, STEPS),
                  DE_OK);
    ASSERT_EQ(de_det_write_checksums(DE_DET_OUT_DIR "/foliage.checksums", "foliage", SEED, actual,
                                     STEPS),
              DE_OK);
    u64 golden[STEPS];
    u32 count = 0;
    ASSERT_EQ(de_det_read_checksums(DE_FOLIAGE_DET_GOLDEN, golden, STEPS, &count), DE_OK);
    ASSERT_EQ(count, STEPS);
    EXPECT_EQ(de_det_report("foliage vs golden", golden, actual, STEPS), DE_DET_NO_DIVERGENCE);
}
