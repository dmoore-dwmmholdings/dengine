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
 *
 * Runs without libFuzzer, in every test build. Inputs are small; the whole test takes well under a
 * second natively (GCC TSan timed out on slow tests before, DENG-391).
 */
#include "de_test.h"

#include "fuzz_import_harness.h"

#include <dengine/de_base.h>
#include <dengine/de_platform.h>

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
