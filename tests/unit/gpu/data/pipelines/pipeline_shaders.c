/* pipeline_shaders.c — cooks the shared S10 pipeline test shaders through the S09 shader importer
 * and holds the shared inputs and expected values (DENG-323). See pipeline_shaders.h.
 *
 * Built with DE_PIPE_TEST_COOK=1 (tests/unit/gpu/CMakeLists.txt) when the build has the shader
 * importer (DE_BUILD_TOOLS and Slang); then DE_PIPE_TEST_DATA_DIR is this directory. The sources
 * are read through a read-only mount of it; the cooker runs without a cook cache, so nothing is
 * written there.
 */
#include "pipeline_shaders.h"

#include <stdio.h>
#include <string.h>

/* ---------- Shared inputs and expected values ---------- */

const f32 de_pipe_test_triangle_pos[9][4] = {
    /* A: lower-left half, counter-clockwise (y up), z 0.5 */
    {-1.0f, -1.0f, 0.5f, 1.0f},
    {1.0f, -1.0f, 0.5f, 1.0f},
    {-1.0f, 1.0f, 0.5f, 1.0f},
    /* B: the same place at z 0.25: behind A with reverse-Z */
    {-1.0f, -1.0f, 0.25f, 1.0f},
    {1.0f, -1.0f, 0.25f, 1.0f},
    {-1.0f, 1.0f, 0.25f, 1.0f},
    /* C: upper-right half, clockwise: culled with DE_CULL_BACK */
    {-1.0f, 1.0f, 0.5f, 1.0f},
    {1.0f, 1.0f, 0.5f, 1.0f},
    {1.0f, -1.0f, 0.5f, 1.0f},
};

const f32 de_pipe_test_triangle_color[3][4] = {
    {0.0f, 1.0f, 0.0f, 1.0f},
    {1.0f, 0.0f, 0.0f, 1.0f},
    {0.0f, 0.0f, 1.0f, 1.0f},
};

const de_pipe_test_pixel de_pipe_test_triangle_expect[3] = {
    {4, 60, {0x00, 0xFF, 0x00, 0xFF}},  /* bottom-left corner: A */
    {32, 50, {0x00, 0xFF, 0x00, 0xFF}}, /* lower middle: A in front of B */
    {60, 4, {DE_PIPE_TEST_CLEAR_U8}},   /* top-right corner: C was culled */
};

#define THIRD (2.0f / 3.0f)
const f32 de_pipe_test_indirect_pos[9][4] = {
    {-1.0f, -1.0f, 0.5f, 1.0f},
    {-1.0f + THIRD, -1.0f, 0.5f, 1.0f},
    {-1.0f, 1.0f, 0.5f, 1.0f},
    {-1.0f + THIRD, -1.0f, 0.5f, 1.0f},
    {-1.0f + 2.0f * THIRD, -1.0f, 0.5f, 1.0f},
    {-1.0f + THIRD, 1.0f, 0.5f, 1.0f},
    {-1.0f + 2.0f * THIRD, -1.0f, 0.5f, 1.0f},
    {1.0f, -1.0f, 0.5f, 1.0f},
    {-1.0f + 2.0f * THIRD, 1.0f, 0.5f, 1.0f},
};

const u16 de_pipe_test_indirect_indices[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};

const de_pipe_test_pixel de_pipe_test_indirect_expect[3] = {
    {3, 60, {0x00, 0xFF, 0x00, 0xFF}},  /* draw 0 */
    {24, 60, {0x00, 0xFF, 0x00, 0xFF}}, /* draw 1 */
    {46, 60, {DE_PIPE_TEST_CLEAR_U8}},  /* draw 2: past the count */
};

/* ---------- Cooking ---------- */

#if DE_PIPE_TEST_COOK

#include <dengine/de_asset.h>
#include <dengine/de_platform.h>

#include "asset/manifest_internal.h"
#include "cook/importers/shader/shader_importer.h"
#include "cook/importers/shader/slang_compile.h"
#include "cook/lib/de_cooker.h"
#include "gpu/shader/shader_internal.h"

static const char *const k_files[] = {
    "triangle.slang",        "compute.slang", "indirect_args.slang",
    "push_limit.slang",      "mesh.slang",    "bindless_access.slang",
    "accel_query.slang", /* DENG-410 */
    "parameter_replay.slang"};
#define FILE_COUNT (sizeof k_files / sizeof k_files[0])

typedef struct cooked {
    bool tried, ok;
    char why[256];
    de_cook_result results[FILE_COUNT];
    de__shader_view views[FILE_COUNT];
    bool have[FILE_COUNT];
} cooked;

static cooked g_cooked[DE__SHADER_TARGET_COUNT_];

static const char *target_name(de__shader_target t) {
    switch (t) {
    case DE__SHADER_TARGET_SPIRV:
        return "spirv";
    case DE__SHADER_TARGET_DXIL:
        return "dxil";
    case DE__SHADER_TARGET_METALLIB:
        return "metallib";
    default:
        return "none";
    }
}

bool de_pipe_test_cook(de__shader_target target, char *why, size_t why_cap) {
    if ((u32)target == 0 || (u32)target >= DE__SHADER_TARGET_COUNT_) {
        snprintf(why, why_cap, "unknown shader target %d", (int)target);
        return false;
    }
    cooked *ck = &g_cooked[target];
    if (ck->tried) {
        snprintf(why, why_cap, "%s", ck->why);
        return ck->ok;
    }
    ck->tried = true;
    char msg[200];
    if (!de__slang_available(msg, sizeof msg)) {
        snprintf(ck->why, sizeof ck->why, "no Slang compiler for this host: %.200s", msg);
        snprintf(why, why_cap, "%s", ck->why);
        return false;
    }
    static bool mounted;
    if (!mounted) {
        if (de_fs_mount("pipeshaders", DE_PIPE_TEST_DATA_DIR, 0, 0) != DE_OK) {
            snprintf(ck->why, sizeof ck->why, "cannot mount %.200s", DE_PIPE_TEST_DATA_DIR);
            snprintf(why, why_cap, "%s", ck->why);
            return false;
        }
        mounted = true;
    }
    de_cooker *c = NULL;
    de_cooker_desc cd = {.verify_determinism = DE_COOK_VERIFY_OFF};
    static const char *const exts[] = {".slang"};
    de_asset_type_desc t;
    memset(&t, 0, sizeof t);
    t.name = "shader";
    t.extensions = exts;
    t.extension_count = 1;
    t.importer_version = de_shader_importer_version();
    t.import = de_shader_import;
    if (de_cooker_create(&cd, &c) != DE_OK || de_cooker_register_type(c, &t) != DE_OK) {
        snprintf(ck->why, sizeof ck->why, "cannot create the cooker: %.200s",
                 de_error_last() ? de_error_last()->message : "?");
        de_cooker_destroy(c);
        snprintf(why, why_cap, "%s", ck->why);
        return false;
    }
    const char *targets[1] = {target_name(target)};
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = "targets";
    kv.value.type = DE__MANIFEST_STRING_ARRAY;
    kv.value.u.arr.items = targets;
    kv.value.u.arr.count = 1;
    ck->ok = true;
    for (u32 i = 0; i < FILE_COUNT; ++i) {
        char id[64];
        snprintf(id, sizeof id, "pipelines/%s", k_files[i]);
        de__manifest_asset a;
        memset(&a, 0, sizeof a);
        a.id = id;
        a.type = "shader";
        a.source = k_files[i];
        a.file = "pipeshaders:/assets.toml";
        a.settings = &kv;
        a.setting_count = 1;
        de_cook_input in = {.asset = &a, .asset_root = "pipeshaders:/", .source = NULL};
        de_result r = de_cooker_cook(c, &in, &ck->results[i]);
        if (r == DE_OK)
            r = de__shader_view_init(ck->results[i].data, ck->results[i].size, &ck->views[i]);
        if (r != DE_OK) {
            /* The mesh shader may not cook for every target (Slang's support differs); the
             * other files must. */
            char err[160];
            snprintf(err, sizeof err, "%.150s", de_error_last() ? de_error_last()->message : "?");
            printf("  (cooking %s for %s failed: %s)\n", k_files[i], targets[0], err);
            if (strcmp(k_files[i], "mesh.slang") != 0) {
                snprintf(ck->why, sizeof ck->why, "cooking %.40s for %.10s failed: %.150s",
                         k_files[i], targets[0], err);
                ck->ok = false;
            }
            continue;
        }
        ck->have[i] = true;
    }
    de_cooker_destroy(c);
    snprintf(why, why_cap, "%s", ck->why);
    return ck->ok;
}

bool de_pipe_test_get(de__shader_target target, const char *file, const char *entry,
                      de_pipe_test_shader *out) {
    memset(out, 0, sizeof *out);
    if ((u32)target == 0 || (u32)target >= DE__SHADER_TARGET_COUNT_)
        return false;
    const cooked *ck = &g_cooked[target];
    for (u32 f = 0; f < FILE_COUNT; ++f) {
        if (strcmp(k_files[f], file) != 0 || !ck->have[f])
            continue;
        const de__shader_view *v = &ck->views[f];
        for (u32 e = 0; e < v->header.entry_count; ++e) {
            const char *name = NULL;
            (void)de__shader_view_entry(v, e, &name);
            if (strcmp(name, entry) != 0)
                continue;
            i32 ri = de__shader_view_find(v, (u32)target, 0, e);
            if (ri < 0)
                return false;
            const de__shader_record rec = de__shader_view_record(v, (u32)ri);
            out->blob.code = v->base + rec.code_offset;
            out->blob.size = rec.code_size;
            out->blob.entry = name;
            out->stage = rec.stage;
            memcpy(out->threadgroup, rec.threadgroup, sizeof out->threadgroup);
            out->push_constant_bytes = rec.push_constant_bytes;
            return true;
        }
    }
    return false;
}

#else /* !DE_PIPE_TEST_COOK */

bool de_pipe_test_cook(de__shader_target target, char *why, size_t why_cap) {
    (void)target;
    snprintf(why, why_cap, "this build has no shader importer (DE_BUILD_TOOLS off or no Slang)");
    return false;
}

bool de_pipe_test_get(de__shader_target target, const char *file, const char *entry,
                      de_pipe_test_shader *out) {
    (void)target;
    (void)file;
    (void)entry;
    memset(out, 0, sizeof *out);
    return false;
}

#endif
