/*
 * test_import_fuzz_regressions.c: CTest import_fuzz_regressions (DENG-106, SPEC §19.1).
 *
 *   - replay.<target>: every file in regressions/<target>/ (inputs a fuzzer crashed on, kept after
 *     the fix) goes through the fuzz harness of that target (fuzz_import_harness.c), as the fuzzer
 *     runs it: the import may fail, but must not crash, and every output must parse. The seed
 *     corpora replay as fuzz.<target> already.
 *   - limits.*: inputs that ask for more memory than the importer allows fail with a de_result
 *     and a message, before the importer allocates: an image header over the decoded-bytes budget
 *     (`max_decoded_mib`) or over 16384, a KTX2 header that claims huge levels, a glTF accessor of
 *     2^25 zero vertices. (FBX: ufbx's own memory limit is set from the same setting; fuzz_fbx
 *     runs under libFuzzer's allocation limit.)
 *   - prefab.cooked_to_dscn (DENG-400): a glTF and an FBX seed with a node hierarchy cook their
 *     `#prefab` sub-asset to DSCN through the harness (the `prefab` importer is registered), so the
 *     harness's DSCN check runs. Where de_asset has no cooker (Release), it stays TOML.
 *
 * Runs without libFuzzer, in every test build. Inputs are small; the whole test takes well under a
 * second natively (GCC TSan timed out on slow tests before, DENG-391).
 */
#include "de_test.h"

#include "fuzz_import_harness.h"

#include <dengine/de_base.h>
#include <dengine/de_platform.h>

#include "asset/manifest_internal.h"
#include "asset/scene/scene_format.h"
#include "cook/importers/texture/texture_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FZ_DIR DE_FUZZ_IMPORT_DIR

/* ---- Files ----------------------------------------------------------------------------------- */

static void fz_mount(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("fzimp", FZ_DIR, 0, 0), DE_OK);
    mounted = true;
}

typedef struct fz_names {
    char names[256][128];
    u32 count;
} fz_names;

static bool collect(const char *name, bool is_dir, void *user) {
    fz_names *n = (fz_names *)user;
    if (is_dir || name[0] == '.' || n->count == 256u || strlen(name) >= 128u)
        return true;
    memcpy(n->names[n->count++], name, strlen(name) + 1u);
    return true;
}

static int cmp_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* Replays regressions/<kind>/ in name order; returns the number of files. */
static u32 replay(fz_import_kind kind) {
    fz_mount();
    static fz_names n;
    memset(&n, 0, sizeof n);
    char dir[128];
    snprintf(dir, sizeof dir, "fzimp:/regressions/%s", fz_import_kind_name(kind));
    if (!de_path_exists(dir))
        return 0;
    EXPECT_EQ(de_dir_list(dir, collect, &n), DE_OK);
    qsort(n.names, n.count, sizeof n.names[0], cmp_names);
    for (u32 i = 0; i < n.count; ++i) {
        char path[256];
        snprintf(path, sizeof path, "%s/%s", dir, n.names[i]);
        void *data = NULL;
        u64 size = 0;
        ASSERT_EQ(de_file_read_all(path, NULL, &data, &size), DE_OK);
        char msg[512];
        de_result r = fz_import_run(kind, (const u8 *)data, (size_t)size, msg, sizeof msg);
        printf("  %s: %s%s%s\n", path, de_result_str(r), r != DE_OK ? ": " : "", msg);
        DE_FREE(de_heap_allocator(), data, (size_t)size + 1u);
    }
    return n.count;
}

TEST(replay, gltf) {
    (void)replay(FZ_IMPORT_GLTF);
}

TEST(replay, fbx) {
    (void)replay(FZ_IMPORT_FBX);
}

TEST(replay, image) {
    (void)replay(FZ_IMPORT_IMAGE);
}

TEST(replay, ktx2) {
    (void)replay(FZ_IMPORT_KTX2);
}

/* ---- Limits ---------------------------------------------------------------------------------- */

static void put_be32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static void put_le32(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/* CRC-32 of PNG chunks (stb_image does not check it, but the header should be valid anyway). */
static u32 crc32_png(const u8 *p, size_t n) {
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (u32 k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

/* A PNG of only a signature, an IHDR of `w` x `h` RGBA8 and an IEND: the header says the size, no
 * pixel follows. */
static size_t png_header(u8 *out, u32 w, u32 h) {
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    memcpy(out, sig, 8);
    put_be32(out + 8, 13);
    memcpy(out + 12, "IHDR", 4);
    put_be32(out + 16, w);
    put_be32(out + 20, h);
    out[24] = 8; /* bit depth */
    out[25] = 6; /* RGBA */
    out[26] = out[27] = out[28] = 0;
    put_be32(out + 29, crc32_png(out + 12, 17));
    put_be32(out + 33, 0);
    memcpy(out + 37, "IEND", 4);
    put_be32(out + 41, crc32_png(out + 37, 4));
    return 45;
}

static void expect_refused(fz_import_kind kind, const u8 *data, size_t size, de_result want,
                           const char *in_msg) {
    char msg[512];
    de_result r = fz_import_run(kind, data, size, msg, sizeof msg);
    printf("  %s: %s: %s\n", fz_import_kind_name(kind), de_result_str(r), msg);
    EXPECT_EQ(r, want);
    EXPECT_TRUE(strstr(msg, in_msg) != NULL);
}

TEST(limits, image_over_budget) {
    u8 png[64];
    /* 4096 x 4096 RGBA: 256 MiB as f32 RGBA, over the harness's max_decoded_mib. */
    size_t n = png_header(png, 4096, 4096);
    expect_refused(FZ_IMPORT_IMAGE, png, n, DE_ERR_UNSUPPORTED, "max_decoded_mib");
    /* Over 16384: refused by the size limit first. */
    n = png_header(png, 16385, 4);
    expect_refused(FZ_IMPORT_IMAGE, png, n, DE_ERR_UNSUPPORTED, "16384");
}

/* A KTX2 file of one RGBA32F level of `w` x `h` x `layers` whose level index claims the matching
 * length, but the bytes are not there. */
static size_t ktx2_header(u8 *out, u32 vk, u32 w, u32 h, u32 layers, u64 level_length) {
    static const u8 id[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    memset(out, 0, 104);
    memcpy(out, id, 12);
    put_le32(out + 12, vk);
    put_le32(out + 16, 1); /* typeSize */
    put_le32(out + 20, w);
    put_le32(out + 24, h);
    put_le32(out + 32, layers);
    put_le32(out + 36, 1);   /* faces */
    put_le32(out + 40, 1);   /* levels */
    put_le32(out + 80, 104); /* level 0 offset (low word) */
    put_le32(out + 88, (u32)level_length);
    put_le32(out + 92, (u32)(level_length >> 32));
    return 104;
}

TEST(limits, ktx2_over_budget) {
    u8 k[104];
    /* RGBA32F 2048 x 2048 x 64 layers: 4 GiB claimed. */
    size_t n = ktx2_header(k, 109, 2048, 2048, 64, (u64)2048 * 2048 * 16 * 64);
    expect_refused(FZ_IMPORT_KTX2, k, n, DE_ERR_UNSUPPORTED, "max_decoded_mib");
    /* BC7 16384 x 16384: 256 MiB of blocks claimed, not in the file. */
    n = ktx2_header(k, 145, 16384, 16384, 1, (u64)16384 * 16384);
    expect_refused(FZ_IMPORT_KTX2, k, n, DE_ERR_UNSUPPORTED, "max_decoded_mib");
}

TEST(limits, gltf_accessor_over_budget) {
    /* 2^25 VEC3 float positions with no buffer view: all zeros, 384 MiB decoded. */
    static const char doc[] =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0}}]}],\"accessors\":[{\"componentType\":5126,\"count\":33554432,"
        "\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[0,0,0]}]}";
    expect_refused(FZ_IMPORT_GLTF, (const u8 *)doc, sizeof doc - 1u, DE_ERR_UNSUPPORTED,
                   "max_decoded_mib");
}

/* ---- Prefab (DENG-400) ----------------------------------------------------------------------- */

typedef struct fz_prefab_seen {
    u32 count;
    u64 size;
    u32 entities;
    char magic[5];
    bool parsed;
} fz_prefab_seen;

static void prefab_output(const char *name, const char *type, const void *data, u64 size,
                          void *user) {
    fz_prefab_seen *p = (fz_prefab_seen *)user;
    if (name == NULL || strcmp(type, "prefab") != 0)
        return;
    ++p->count;
    p->size = size;
    memset(p->magic, 0, sizeof p->magic);
    if (size >= 4u)
        memcpy(p->magic, data, 4);
    de__scene_view v;
    p->parsed = de__scene_view_parse(data, size, &v, NULL, 0) == DE_OK;
    p->entities = p->parsed ? v.entity_count : 0u;
}

static void expect_prefab(fz_import_kind kind, const char *file) {
    fz_mount();
    char path[256];
    snprintf(path, sizeof path, "fzimp:/corpus/%s/%s", fz_import_kind_name(kind), file);
    void *data = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all(path, NULL, &data, &size), DE_OK);
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = "max_decoded_mib";
    kv.value.type = DE__MANIFEST_INT;
    kv.value.u.i = FZ_IMPORT_MAX_DECODED_MIB;
    fz_prefab_seen seen;
    memset(&seen, 0, sizeof seen);
    char msg[512];
    de_result r = fz_import_cook(kind, (const u8 *)data, (size_t)size, &kv, 1, prefab_output, &seen,
                                 msg, sizeof msg);
    DE_FREE(de_heap_allocator(), data, (size_t)size + 1u);
    printf("  %s: %s%s%s; prefab \"%s\", %llu bytes, %u entities\n", path, de_result_str(r),
           r != DE_OK ? ": " : "", msg, seen.magic, (unsigned long long)seen.size, seen.entities);
    ASSERT_EQ(r, DE_OK);
    ASSERT_EQ(seen.count, 1u);
#if DE_HAS_COOKER
    /* The harness checked the DSCN already (fz_check_prefab); this checks that it ran. */
    EXPECT_STREQ(seen.magic, DE__SCENE_MAGIC);
    EXPECT_TRUE(seen.parsed);
    EXPECT_GT(seen.entities, 1u); /* the model's root and the nodes under it */
#else
    EXPECT_FALSE(seen.parsed); /* no prefab importer: the TOML as it is */
#endif
}

TEST(prefab, cooked_to_dscn) {
    expect_prefab(FZ_IMPORT_GLTF, "hierarchy.gltf");
    expect_prefab(FZ_IMPORT_FBX, "skinned.fbx");
}

/* DENG-435: PNG 3 permits consecutive empty IDAT chunks. Each fixture stores the same
 * filter-0 RGBA scanline in a zlib stored block, with correct chunk CRCs. Splitting the
 * stream inside its header also proves that IDAT boundaries do not delimit zlib data. */
typedef struct png_cooked_output {
    u8 bytes[4096];
    u64 size;
    u32 count;
} png_cooked_output;

static void png_output(const char *name, const char *type, const void *data, u64 size, void *user) {
    (void)name;
    if (strcmp(type, "texture") != 0)
        return;
    png_cooked_output *out = (png_cooked_output *)user;
    ASSERT_TRUE(size <= sizeof out->bytes);
    memcpy(out->bytes, data, (size_t)size);
    out->size = size;
    ++out->count;
}

static void png_empty_idat(u32 bits, fz_import_kind kind) {
    static const char *variants[] = {"plain", "leading", "middle", "trailing", "all"};
    png_cooked_output baseline = {0};
    fz_mount();
    for (u32 i = 0; i < 5u; ++i) {
        char path[256];
        snprintf(path, sizeof path, "fzimp:/regressions/%s/png_empty_idat_%u_%s.%s",
                 fz_import_kind_name(kind), bits, variants[i],
                 kind == FZ_IMPORT_IMAGE ? "png" : "gltf");
        void *data = NULL;
        u64 size = 0;
        ASSERT_EQ(de_file_read_all(path, NULL, &data, &size), DE_OK);
        if (kind == FZ_IMPORT_IMAGE) {
            de_tex_image img;
            ASSERT_EQ(de_tex_decode_stb(de_heap_allocator(), data, size, &img), DE_OK);
            EXPECT_EQ(img.width, 1u);
            EXPECT_EQ(img.height, 1u);
            EXPECT_EQ(img.channels, 4u);
            EXPECT_TRUE(img.rgba[0] == (bits == 8u ? 17.0f / 255.0f : 4660.0f / 65535.0f));
            EXPECT_TRUE(img.rgba[1] == (bits == 8u ? 85.0f / 255.0f : 21845.0f / 65535.0f));
            EXPECT_TRUE(img.rgba[2] == (bits == 8u ? 170.0f / 255.0f : 43690.0f / 65535.0f));
            EXPECT_TRUE(img.rgba[3] == 1.0f);
            de_tex_image_free(de_heap_allocator(), &img);
        }
        png_cooked_output output = {0};
        char msg[512];
        ASSERT_EQ(
            fz_import_cook(kind, data, (size_t)size, NULL, 0, png_output, &output, msg, sizeof msg),
            DE_OK);
        DE_FREE(de_heap_allocator(), data, (size_t)size + 1u);
        ASSERT_EQ(output.count, 1u);
        ASSERT_GT(output.size, 0u);
        if (i == 0u)
            baseline = output;
        else {
            ASSERT_EQ(output.size, baseline.size);
            EXPECT_EQ(memcmp(output.bytes, baseline.bytes, (size_t)output.size), 0);
        }
    }
}

TEST(png, empty_idat_rgba8) {
    png_empty_idat(8, FZ_IMPORT_IMAGE);
}

TEST(png, empty_idat_rgba16) {
    png_empty_idat(16, FZ_IMPORT_IMAGE);
}

TEST(png, empty_idat_gltf_rgba8) {
    png_empty_idat(8, FZ_IMPORT_GLTF);
}

TEST(png, empty_idat_gltf_rgba16) {
    png_empty_idat(16, FZ_IMPORT_GLTF);
}
