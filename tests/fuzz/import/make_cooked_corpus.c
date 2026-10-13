/*
 * make_cooked_corpus.c: CTest cooked_corpus (DENG-106): the seed corpora of fuzz_cooked_render
 * (tests/fuzz/render/corpus/cooked_render/) and fuzz_cooked_anim
 * (tests/fuzz/anim/corpus/cooked_anim/).
 *
 * The seeds are blobs the importers cook from the importer seed corpora (corpus/gltf/,
 * corpus/ktx2/, corpus/image/ next to this file), through the same in-memory harness as the fuzz
 * targets:
 *   - render: `texture_*` (DTEX), `env_*` (DTEX + ENVS, DENG-100), `mesh_*` (DMSH v2),
 *     `material_*` (DMAT), `model_*` (DMDL);
 *   - anim: `pair_*`, a u32 skeleton length, a DSKL blob and a DCLP blob of that skeleton (the
 *     input layout of fuzz_cooked_anim.c).
 *
 * cooked_corpus.seeds.parse checks that every seed is still accepted by its parser (the name
 * prefix says which; `regress_*` files are crash inputs kept after a parser fix, replayed by the
 * fuzz targets and not checked here) and that each kind has one: a format version bump fails it
 * until the seeds are written again with
 *
 *     DE_COOKED_CORPUS_UPDATE=1 ctest --preset dev -R cooked_corpus
 *
 * Cooks are byte-identical on one host; meshoptimizer may round differently on another, which only
 * changes seed bytes, never whether they parse, so the check is "parses", not "equal".
 */
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#include "de_test.h"

#include "fuzz_cooked_walk.h"
#include "fuzz_import_harness.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/manifest_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CC_IMPORT DE_FUZZ_IMPORT_DIR
#define CC_RENDER DE_FUZZ_IMPORT_DIR "/../render/corpus/cooked_render"
#define CC_ANIM   DE_FUZZ_IMPORT_DIR "/../anim/corpus/cooked_anim"

static void cc_mount(bool writable) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("ccin", CC_IMPORT, 0, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("ccrender", CC_RENDER, writable ? DE_MOUNT_WRITABLE : 0, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("ccanim", CC_ANIM, writable ? DE_MOUNT_WRITABLE : 0, 0), DE_OK);
    mounted = true;
}

/* ---- Writing (DE_COOKED_CORPUS_UPDATE=1) ----------------------------------------------------- */

typedef struct cc_cook {
    const char *stem; /* seed name stem, from the source file */
    bool env;
    /* The anim pair: the first skeleton and the first clip of the import. */
    u8 *skel;
    u64 skel_size;
    u8 *clip;
    u64 clip_size;
    u32 written;
} cc_cook;

static void cc_write(const char *root, const char *prefix, const char *stem, const char *name,
                     const void *data, u64 size) {
    char path[256];
    char clean[64];
    size_t n = 0;
    for (const char *p = name; *p != '\0' && n + 1u < sizeof clean; ++p)
        clean[n++] =
            (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ? *p
                                                                                             : '_';
    clean[n] = '\0';
    snprintf(path, sizeof path, "%s:/%s_%s%s%s", root, prefix, stem, n != 0 ? "_" : "", clean);
    ASSERT_EQ(de_file_write_all_atomic(path, data, size), DE_OK);
}

static u8 *cc_dup(const void *data, u64 size) {
    u8 *p = (u8 *)malloc((size_t)size + 1u);
    if (p != NULL && size != 0)
        memcpy(p, data, (size_t)size);
    return p;
}

static void cc_output(const char *name, const char *type, const void *data, u64 size, void *user) {
    cc_cook *c = (cc_cook *)user;
    const char *prefix = NULL;
    if (strcmp(type, "texture") == 0)
        prefix = c->env && name == NULL ? "env" : "texture";
    else if (strcmp(type, "mesh") == 0)
        prefix = "mesh";
    else if (strcmp(type, "material") == 0)
        prefix = "material";
    else if (strcmp(type, "model") == 0)
        prefix = "model";
    /* Seeds stay small: libFuzzer mutates short inputs faster (a model's 256 x 256 texture is
     * left out; the KTX2 and PNG cooks give texture seeds). */
    if (prefix != NULL && size <= 32u * 1024u) {
        cc_write("ccrender", prefix, c->stem, name != NULL ? name : "", data, size);
        ++c->written;
    }
    if (strcmp(type, "skeleton") == 0 && c->skel == NULL) {
        c->skel = cc_dup(data, size);
        c->skel_size = size;
    }
    if (strcmp(type, "anim_clip") == 0 && c->clip == NULL) {
        c->clip = cc_dup(data, size);
        c->clip_size = size;
    }
}

static void cc_cook_one(fz_import_kind kind, const char *src, const char *stem, bool env) {
    char path[256];
    snprintf(path, sizeof path, "ccin:/%s", src);
    void *data = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all(path, NULL, &data, &size), DE_OK);
    de__manifest_kv kv[3];
    memset(kv, 0, sizeof kv);
    u32 n = 0;
    if (env) {
        kv[n].key = "usage";
        kv[n].value.type = DE__MANIFEST_STRING;
        kv[n++].value.u.s = "environment";
        kv[n].key = "env_size";
        kv[n].value.type = DE__MANIFEST_INT;
        kv[n++].value.u.i = 8;
        kv[n].key = "env_samples";
        kv[n].value.type = DE__MANIFEST_INT;
        kv[n++].value.u.i = 16;
    }
    cc_cook c;
    memset(&c, 0, sizeof c);
    c.stem = stem;
    c.env = env;
    char msg[512];
    de_result r =
        fz_import_cook(kind, (const u8 *)data, (size_t)size, kv, n, cc_output, &c, msg, sizeof msg);
    printf("  %s: %s %s (%u render seeds)\n", src, de_result_str(r), msg, c.written);
    EXPECT_EQ(r, DE_OK);
    if (c.skel != NULL && c.clip != NULL) {
        u64 total = 4u + c.skel_size + c.clip_size;
        u8 *pair = (u8 *)malloc((size_t)total);
        ASSERT_TRUE(pair != NULL);
        pair[0] = (u8)c.skel_size;
        pair[1] = (u8)(c.skel_size >> 8);
        pair[2] = (u8)(c.skel_size >> 16);
        pair[3] = (u8)(c.skel_size >> 24);
        memcpy(pair + 4, c.skel, (size_t)c.skel_size);
        memcpy(pair + 4 + c.skel_size, c.clip, (size_t)c.clip_size);
        cc_write("ccanim", "pair", stem, "", pair, total);
        free(pair);
    }
    free(c.skel);
    free(c.clip);
    DE_FREE(de_heap_allocator(), data, (size_t)size + 1u);
}

static void cc_update(void) {
    cc_cook_one(FZ_IMPORT_GLTF, "corpus/gltf/skinned.gltf", "skinned", false);
    cc_cook_one(FZ_IMPORT_GLTF, "corpus/gltf/BoxTextured.glb", "boxtextured", false);
    cc_cook_one(FZ_IMPORT_GLTF, "corpus/gltf/hierarchy.gltf", "hierarchy", false);
    cc_cook_one(FZ_IMPORT_GLTF, "corpus/gltf/quantized.gltf", "quantized", false);
    cc_cook_one(FZ_IMPORT_FBX, "corpus/fbx/skinned.fbx", "skinnedfbx", false);
    cc_cook_one(FZ_IMPORT_KTX2, "corpus/ktx2/bc7.ktx2", "bc7", false);
    cc_cook_one(FZ_IMPORT_KTX2, "corpus/ktx2/cube_rgba8.ktx2", "cube", false);
    cc_cook_one(FZ_IMPORT_KTX2, "corpus/ktx2/cube_rgba8.ktx2", "cube", true);
    cc_cook_one(FZ_IMPORT_IMAGE, "corpus/image/equirect_16x8.png", "equirect", true);
}

/* ---- Checking -------------------------------------------------------------------------------- */

typedef struct cc_list {
    char names[128][96];
    u32 count;
} cc_list;

static bool cc_collect(const char *name, bool is_dir, void *user) {
    cc_list *l = (cc_list *)user;
    if (!is_dir && name[0] != '.' && l->count < 128u && strlen(name) < 96u)
        memcpy(l->names[l->count++], name, strlen(name) + 1u);
    return true;
}

static bool starts(const char *s, const char *p) {
    return strncmp(s, p, strlen(p)) == 0;
}

TEST(seeds, parse) {
    const char *update = getenv("DE_COOKED_CORPUS_UPDATE");
    bool writing = update != NULL && strcmp(update, "1") == 0;
    cc_mount(writing);
    if (writing)
        cc_update();

    u32 kinds[7] = {0, 0, 0, 0, 0, 0, 0}; /* texture env mesh material model pair */
    static cc_list l;
    const char *roots[2] = {"ccrender:/", "ccanim:/"};
    for (u32 ri = 0; ri < 2u; ++ri) {
        memset(&l, 0, sizeof l);
        ASSERT_EQ(de_dir_list(roots[ri], cc_collect, &l), DE_OK);
        for (u32 i = 0; i < l.count; ++i) {
            char path[160];
            snprintf(path, sizeof path, "%s%s", roots[ri], l.names[i]);
            void *data = NULL;
            u64 size = 0;
            ASSERT_EQ(de_file_read_all(path, NULL, &data, &size), DE_OK);
            const char *why = NULL;
            const char *n = l.names[i];
            bool ok = false;
            if (starts(n, "texture_")) {
                ok = fz_walk_texture(data, size, &why);
                ++kinds[0];
            } else if (starts(n, "env_")) {
                ok = fz_walk_env(data, size, &why);
                ++kinds[1];
            } else if (starts(n, "mesh_")) {
                ok = fz_walk_mesh(data, size, &why);
                ++kinds[2];
            } else if (starts(n, "material_")) {
                ok = fz_walk_material(data, size, &why);
                ++kinds[3];
            } else if (starts(n, "model_")) {
                ok = fz_walk_model(data, size, &why);
                ++kinds[4];
            } else if (starts(n, "pair_") && size >= 4u) {
                const u8 *p = (const u8 *)data;
                u64 split = (u64)p[0] | ((u64)p[1] << 8) | ((u64)p[2] << 16) | ((u64)p[3] << 24);
                ok = split <= size - 4u && fz_walk_skeleton(p + 4, split, &why) &&
                     fz_walk_clip(p + 4 + split, size - 4u - split, p + 4, split, &why);
                ++kinds[5];
            } else if (starts(n, "regress_")) {
                /* An input a fuzzer crashed the parser with (docs/ci.md): replayed by the
                 * fuzz target's CTest entry; it need not parse. */
                ok = true;
            } else {
                why = "unknown seed name prefix";
            }
            if (!ok)
                printf("  %s: %s\n", path, why != NULL ? why : "?");
            EXPECT_TRUE(ok);
            DE_FREE(de_heap_allocator(), data, (size_t)size + 1u);
        }
    }
    for (u32 k = 0; k < 6u; ++k)
        EXPECT_GT(kinds[k], 0u);
}
