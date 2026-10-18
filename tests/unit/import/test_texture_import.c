/*
 * test_texture_import.c: the texture importer (DENG-98, SPEC §11.6, §11.5): PNG, JPEG, EXR and
 * KTX2 (raw, Basis UASTC and ETC1S, BC pass-through) to DTEX, sRGB-correct linear mips, NPOT
 * chains, normal maps, alpha coverage, the upload layout, determinism, rejection of oversize and
 * corrupt files, the settings hash, model textures through the registry, the Dev in-process cook
 * and `de-cook cook`. CTest names import.texture.<test>.
 *
 * Fixtures are in data/texture/ (mounted read-only as texd:/), written by gen_fixtures.py. The
 * glTF fixtures of DENG-101 are mounted as texg:/. Scratch files go to ttmp:/tex/.
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_task.h>

#include "asset/asset_internal.h"
#include "asset/dpak/dpak_format.h"
#include "asset/dpak/dpak_reader.h"
#include "asset/manifest_internal.h"
#include "cook/cook_cmd.h"
#include "cook/importers/model/model_import.h"
#include "cook/importers/texture/texture_import.h"
#include "cook/importers/texture/texture_internal.h"
#include "cook/lib/de_cooker.h"
#include "cook/lib/decache.h"
#include "render/formats/de_cooked_texture.h"
#include "render/formats/de_cooked_sprite_proxy.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEX_COUNT(a) ((u32)(sizeof(a) / sizeof((a)[0])))

/* ---- Files ----------------------------------------------------------------------------------- */

static void tex_mount(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("texd", DE_IMPORT_TEST_DATA_DIR "/texture", 0, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("texg", DE_IMPORT_TEST_DATA_DIR "/gltf", 0, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("ttmp", DE_IMPORT_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    if (!de_path_exists("ttmp:/tex"))
        ASSERT_EQ(de_dir_create("ttmp:/tex"), DE_OK);
    mounted = true;
}

static void mkdirs(const char *vpath) {
    if (!de_path_exists(vpath))
        ASSERT_EQ(de_dir_create(vpath), DE_OK);
}

/* ---- Fixture --------------------------------------------------------------------------------- */

typedef struct tex_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_cooker *c;
    de_task_system *tasks;
    de_type_registry *types;
    de_world *w;
    de_asset_system *s;
} tex_fx;

static void tex_fx_setup(tex_fx *f) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    tex_mount();
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
}

static void tex_fx_teardown(tex_fx *f) {
    if (f->c != NULL)
        de_cooker_destroy(f->c);
    if (f->s != NULL)
        de_asset_system_destroy(f->s);
    if (f->w != NULL)
        de_world_destroy(f->w);
    if (f->tasks != NULL)
        de_task_system_destroy(f->tasks);
    if (f->types != NULL)
        de_type_registry_destroy(f->types);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    de_log_shutdown();
}

/* A cooker with the texture importer (verify_determinism on: every import runs twice). */
static void make_cooker(tex_fx *f, const char *cache_dir) {
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.allocator = &f->alloc,
                                                 .cache_dir = cache_dir,
                                                 .verify_determinism = DE_COOK_VERIFY_ON},
                               &f->c),
              DE_OK);
    de_asset_type_desc t = de_texture_import_type_desc();
    ASSERT_EQ(de_cooker_register_type(f->c, &t), DE_OK);
}

static de__manifest_kv kv_bool(const char *k, bool v) {
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = k;
    kv.value.type = DE__MANIFEST_BOOL;
    kv.value.u.b = v;
    return kv;
}

static de__manifest_kv kv_str(const char *k, const char *v) {
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = k;
    kv.value.type = DE__MANIFEST_STRING;
    kv.value.u.s = v;
    return kv;
}

static de__manifest_kv kv_int(const char *k, i64 v) {
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = k;
    kv.value.type = DE__MANIFEST_INT;
    kv.value.u.i = v;
    return kv;
}

static de__manifest_kv kv_float(const char *k, f64 v) {
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = k;
    kv.value.type = DE__MANIFEST_FLOAT;
    kv.value.u.f = v;
    return kv;
}

/* Cooks `root`/`source` as texture asset "tex/test" with the given settings. These tests check
 * decoded texels, so `compression = "none"` is added unless the settings name a compression
 * (DENG-99; the BC formats are tested in test_texture_bc.c). */
static de_result cook_at(tex_fx *f, const char *root, const char *source, const de__manifest_kv *kv,
                         u32 n, de_cook_result *out) {
    de__manifest_kv all[16];
    bool has_compression = false;
    ASSERT_TRUE(n < TEX_COUNT(all));
    for (u32 i = 0; i < n; ++i) {
        all[i] = kv[i];
        has_compression = has_compression || strcmp(kv[i].key, "compression") == 0;
    }
    if (!has_compression)
        all[n++] = kv_str("compression", "none");
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = "tex/test";
    a.type = "texture";
    a.source = source;
    a.file = "texd:/assets.toml";
    a.settings = all;
    a.setting_count = n;
    de_error_clear();
    return de_cooker_cook(f->c, &(de_cook_input){.asset = &a, .asset_root = root}, out);
}

static de_result cook(tex_fx *f, const char *source, const de__manifest_kv *kv, u32 n,
                      de_cook_result *out) {
    return cook_at(f, "texd:/", source, kv, n, out);
}

static bool error_says(const char *needle) {
    const de_error *e = de_error_last();
    if (e == NULL || strstr(e->message, needle) == NULL) {
        printf("  error: %s (wanted \"%s\")\n", e != NULL ? e->message : "(none)", needle);
        return false;
    }
    return true;
}

static bool view_of(const void *data, u64 size, de_cooked_texture_view *v) {
    const char *why = NULL;
    if (!de_cooked_texture_parse(data, size, v, &why)) {
        printf("  DTEX rejected: %s\n", why != NULL ? why : "?");
        return false;
    }
    return true;
}

static de_cooked_texture_subresource sub_of(const de_cooked_texture_view *v, u32 layer, u32 mip) {
    de_cooked_texture_subresource s;
    memset(&s, 0, sizeof s);
    (void)de_cooked_texture_subresource_at(v, layer * v->mip_count + mip, &s);
    return s;
}

/* The bytes of texel (x, y) (or block (x, y)) of a subresource. */
static const u8 *texel_at(const de_cooked_texture_view *v, u32 layer, u32 mip, u32 x, u32 y) {
    de_cooked_texture_subresource s = sub_of(v, layer, mip);
    return v->blob + s.offset + (u64)y * s.row_pitch + (u64)x * v->block.bytes;
}

static f32 half_at(const u8 *p, u32 c) {
    return de_tex_f16_to_f32((u16)(p[2 * c] | (p[2 * c + 1] << 8)));
}

/* Checks the upload layout: 512-aligned offsets, 256-aligned pitches, and zero bytes everywhere
 * outside the rows (between the table and the first subresource, past each row, between
 * subresources). */
static bool layout_ok(const de_cooked_texture_view *v) {
    u64 table_end = DE_COOKED_TEXTURE_HEADER_SIZE +
                    (u64)v->subresource_count * DE_COOKED_TEXTURE_SUBRESOURCE_SIZE;
    u64 pos = table_end;
    for (u32 i = 0; i < v->subresource_count; ++i) {
        de_cooked_texture_subresource s;
        if (!de_cooked_texture_subresource_at(v, i, &s))
            return false;
        if (s.offset % 512u != 0 || s.row_pitch % 256u != 0) {
            printf("  subresource %u: offset %llu pitch %u\n", i, (unsigned long long)s.offset,
                   s.row_pitch);
            return false;
        }
        for (u64 b = pos; b < s.offset; ++b)
            if (v->blob[b] != 0) {
                printf("  non-zero gap byte at %llu\n", (unsigned long long)b);
                return false;
            }
        u64 row = de_cooked_texture_row_bytes(&v->block, s.width);
        for (u32 y = 0; y < s.row_count; ++y)
            for (u64 b = row; b < s.row_pitch; ++b)
                if (v->blob[s.offset + (u64)y * s.row_pitch + b] != 0) {
                    printf("  non-zero row padding in subresource %u row %u\n", i, y);
                    return false;
                }
        pos = s.offset + s.size;
    }
    return pos == v->size;
}

/* Reads a fixture into a block of the fixture's allocator (free with free_file). */
static void *read_file(tex_fx *f, const char *vpath, u64 *size) {
    void *p = NULL;
    if (de_file_read_all(vpath, &f->alloc, &p, size) != DE_OK)
        return NULL;
    return p;
}

static void free_file(tex_fx *f, void *p, u64 size) {
    DE_FREE(&f->alloc, p, (size_t)size + 1u);
}

/* ---- import.texture.import_png_rgba8 --------------------------------------------------------- */

TEST_F(texture, import_png_rgba8, tex_fx) {
    make_cooker(f, NULL);
    de_cook_result r;
    ASSERT_EQ(cook(f, "rgba8.png", NULL, 0, &r), DE_OK);
    de_cooked_texture_view v;
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.dim, (u32)DE_TEX_2D);
    EXPECT_EQ(v.format, (u32)DE_FMT_RGBA8_SRGB); /* colour: sRGB by default */
    EXPECT_EQ(v.width, 16u);
    EXPECT_EQ(v.height, 16u);
    EXPECT_EQ(v.depth_or_layers, 1u);
    EXPECT_EQ(v.mip_count, 5u); /* 16, 8, 4, 2, 1 */
    EXPECT_EQ(v.flags, (u32)(DE_COOKED_TEXTURE_SRGB | DE_COOKED_TEXTURE_HAS_ALPHA));
    EXPECT_EQ(v.usage, (u32)DE_COOKED_TEXTURE_USAGE_COLOR);
    /* Mip 0 is the source exactly: sRGB decode and encode round-trip every byte. */
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x) {
            const u8 *t = texel_at(&v, 0, 0, x, y);
            u8 want[4] = {(u8)(x * 17), (u8)(y * 17), 128, (u8)((x < 4 && y < 4) ? 0 : 255)};
            if (memcmp(t, want, 4) != 0) {
                printf("  texel %u,%u: %u %u %u %u\n", x, y, t[0], t[1], t[2], t[3]);
                EXPECT_TRUE(false);
                y = 16;
                break;
            }
        }
    EXPECT_TRUE(layout_ok(&v));
    de_cook_result_free(&r);

    /* srgb = false: UNORM, same bytes at mip 0; mips = false: one mip. */
    de__manifest_kv kv[2] = {kv_bool("srgb", false), kv_bool("mips", false)};
    ASSERT_EQ(cook(f, "rgba8.png", kv, 2, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.format, (u32)DE_FMT_RGBA8_UNORM);
    EXPECT_EQ(v.mip_count, 1u);
    EXPECT_EQ(v.flags, (u32)DE_COOKED_TEXTURE_HAS_ALPHA);
    EXPECT_MEMEQ(texel_at(&v, 0, 0, 5, 7), ((const u8[]){85, 119, 128, 255}), 4);
    de_cook_result_free(&r);

    /* pixel_art: no mips, the flag. max_size 4: 4x4 with the full chain of 3 mips. */
    de__manifest_kv pa = kv_bool("pixel_art", true);
    ASSERT_EQ(cook(f, "rgba8.png", &pa, 1, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.mip_count, 1u);
    EXPECT_TRUE((v.flags & DE_COOKED_TEXTURE_PIXEL_ART) != 0);
    de_cook_result_free(&r);
    de__manifest_kv ms = kv_int("max_size", 4);
    ASSERT_EQ(cook(f, "rgba8.png", &ms, 1, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.width, 4u);
    EXPECT_EQ(v.height, 4u);
    EXPECT_EQ(v.mip_count, 3u);
    de_cook_result_free(&r);
}

/* ---- import.texture.import_png_formats ------------------------------------------------------- */

TEST_F(texture, import_png_formats, tex_fx) {
    make_cooker(f, NULL);
    de_cook_result r;
    de_cooked_texture_view v;
    de__manifest_kv lin = kv_bool("srgb", false);
    /* Grey 8-bit: grey in rgb, alpha 255. */
    ASSERT_EQ(cook(f, "gray8.png", &lin, 1, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.width, 8u);
    EXPECT_EQ(v.flags & DE_COOKED_TEXTURE_HAS_ALPHA, 0u);
    EXPECT_MEMEQ(texel_at(&v, 0, 0, 3, 2), ((const u8[]){76, 76, 76, 255}), 4);
    de_cook_result_free(&r);
    /* Grey + alpha. */
    ASSERT_EQ(cook(f, "gray_alpha8.png", &lin, 1, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_MEMEQ(texel_at(&v, 0, 0, 2, 5), ((const u8[]){64, 64, 64, 160}), 4);
    EXPECT_TRUE((v.flags & DE_COOKED_TEXTURE_HAS_ALPHA) != 0);
    de_cook_result_free(&r);
    /* RGB 16-bit: x * 9362 / 65535 * 255. */
    ASSERT_EQ(cook(f, "rgb16.png", &lin, 1, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_MEMEQ(texel_at(&v, 0, 0, 7, 0), ((const u8[]){255, 0, 255, 255}), 4);
    EXPECT_MEMEQ(texel_at(&v, 0, 0, 3, 4), ((const u8[]){109, 146, 255, 255}), 4);
    de_cook_result_free(&r);
    /* RGBA 16-bit: alpha 32768 / 65535. */
    ASSERT_EQ(cook(f, "rgba16.png", &lin, 1, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_MEMEQ(texel_at(&v, 0, 0, 1, 2), ((const u8[]){36, 0, 73, 128}), 4);
    de_cook_result_free(&r);
    /* Palette. */
    ASSERT_EQ(cook(f, "palette.png", &lin, 1, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_MEMEQ(texel_at(&v, 0, 0, 1, 1), ((const u8[]){0, 0, 255, 255}), 4);
    EXPECT_MEMEQ(texel_at(&v, 0, 0, 3, 0), ((const u8[]){255, 255, 255, 255}), 4);
    de_cook_result_free(&r);
}

/* ---- import.texture.import_jpg --------------------------------------------------------------- */

TEST_F(texture, import_jpg, tex_fx) {
    make_cooker(f, NULL);
    de_cook_result r;
    ASSERT_EQ(cook(f, "photo.jpg", NULL, 0, &r), DE_OK);
    de_cooked_texture_view v;
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.format, (u32)DE_FMT_RGBA8_SRGB);
    EXPECT_EQ(v.width, 16u);
    EXPECT_EQ(v.height, 16u);
    EXPECT_EQ(v.mip_count, 5u);
    EXPECT_EQ(v.flags & DE_COOKED_TEXTURE_HAS_ALPHA, 0u);
    /* Quality 95 without chroma subsampling: within a few steps of (x * 16, y * 16, 96). */
    for (u32 y = 0; y < 16; y += 5)
        for (u32 x = 0; x < 16; x += 5) {
            const u8 *t = texel_at(&v, 0, 0, x, y);
            EXPECT_TRUE(abs((int)t[0] - (int)(x * 16)) <= 8);
            EXPECT_TRUE(abs((int)t[1] - (int)(y * 16)) <= 8);
            EXPECT_TRUE(abs((int)t[2] - 96) <= 8);
            EXPECT_EQ(t[3], 255);
        }
    EXPECT_TRUE(layout_ok(&v));
    de_cook_result_free(&r);
}

/* ---- import.texture.import_exr_half ---------------------------------------------------------- */

TEST_F(texture, import_exr_half, tex_fx) {
    make_cooker(f, NULL);
    de_cook_result r;
    de_cooked_texture_view v;
    /* Half RGB, uncompressed: RGBA16F, linear, values above 1 kept, alpha 1. */
    ASSERT_EQ(cook(f, "half_rgb.exr", NULL, 0, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.format, (u32)DE_FMT_RGBA16_FLOAT);
    EXPECT_EQ(v.width, 8u);
    EXPECT_EQ(v.mip_count, 4u);
    EXPECT_EQ(v.flags, 0u); /* not sRGB, no alpha */
    const u8 *t = texel_at(&v, 0, 0, 3, 2);
    EXPECT_FLOAT_EQ(half_at(t, 0), 0.75f, 0.0f);
    EXPECT_FLOAT_EQ(half_at(t, 1), 0.5f, 0.0f);
    EXPECT_FLOAT_EQ(half_at(t, 2), 2.5f, 0.0f);
    EXPECT_FLOAT_EQ(half_at(t, 3), 1.0f, 0.0f);
    /* The 1x1 mip averages B = 2.5 everywhere. */
    EXPECT_FLOAT_EQ(half_at(texel_at(&v, 0, 3, 0, 0), 2), 2.5f, 0.01f);
    EXPECT_TRUE(layout_ok(&v));
    de_cook_result_free(&r);
    /* Float RGBA, ZIP compression: negative and large values, alpha 0.5. */
    ASSERT_EQ(cook(f, "float_rgba.exr", NULL, 0, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.format, (u32)DE_FMT_RGBA16_FLOAT);
    EXPECT_EQ(v.flags, (u32)DE_COOKED_TEXTURE_HAS_ALPHA);
    t = texel_at(&v, 0, 0, 3, 7);
    EXPECT_FLOAT_EQ(half_at(t, 0), 3.25f, 0.0f);
    EXPECT_FLOAT_EQ(half_at(t, 1), -7.0f, 0.0f);
    EXPECT_FLOAT_EQ(half_at(t, 2), 1000.0f, 0.0f);
    EXPECT_FLOAT_EQ(half_at(t, 3), 0.5f, 0.0f);
    de_cook_result_free(&r);
    /* srgb = true is ignored for a float source (a warning). */
    de__manifest_kv s = kv_bool("srgb", true);
    ASSERT_EQ(cook(f, "half_rgb.exr", &s, 1, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.format, (u32)DE_FMT_RGBA16_FLOAT);
    de_cook_result_free(&r);
}

/* ---- import.texture.import_ktx2_uastc -------------------------------------------------------- */

/* The largest difference of mip 0 from rgba8.png. */
static int max_diff_from_rgba8(const de_cooked_texture_view *v) {
    int worst = 0;
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x) {
            const u8 *t = texel_at(v, 0, 0, x, y);
            int want[4] = {(int)x * 17, (int)y * 17, 128, (x < 4 && y < 4) ? 0 : 255};
            for (u32 c = 0; c < 4; ++c) {
                int d = abs((int)t[c] - want[c]);
                worst = d > worst ? d : worst;
            }
        }
    return worst;
}

TEST_F(texture, import_ktx2_uastc, tex_fx) {
    make_cooker(f, NULL);
    de_cook_result r;
    de_cooked_texture_view v;
    /* UASTC LDR 4x4 with zstd, encoded from rgba8.png: transcoded at cook time (RGBA8 until
     * DENG-99), sRGB from the file's transfer function, full chain regenerated. */
    ASSERT_EQ(cook(f, "uastc.ktx2", NULL, 0, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.format, (u32)DE_FMT_RGBA8_SRGB);
    EXPECT_EQ(v.width, 16u);
    EXPECT_EQ(v.mip_count, 5u);
    EXPECT_TRUE((v.flags & DE_COOKED_TEXTURE_HAS_ALPHA) != 0);
    int d = max_diff_from_rgba8(&v);
    printf("  UASTC max difference: %d\n", d);
    EXPECT_TRUE(d <= 12);
    EXPECT_TRUE(layout_ok(&v));
    de_cook_result_free(&r);
}

/* ---- import.texture.import_ktx2_etc1s -------------------------------------------------------- */

TEST_F(texture, import_ktx2_etc1s, tex_fx) {
    make_cooker(f, NULL);
    de_cook_result r;
    de_cooked_texture_view v;
    ASSERT_EQ(cook(f, "etc1s.ktx2", NULL, 0, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.format, (u32)DE_FMT_RGBA8_SRGB);
    EXPECT_EQ(v.width, 16u);
    EXPECT_EQ(v.mip_count, 5u);
    int d = max_diff_from_rgba8(&v);
    printf("  ETC1S max difference: %d\n", d);
    EXPECT_TRUE(d <= 48); /* ETC1S is the low-bitrate codec */
    de_cook_result_free(&r);
}

/* ---- import.texture.import_ktx2_bc_passthrough ---------------------------------------------- */

TEST_F(texture, import_ktx2_bc_passthrough, tex_fx) {
    make_cooker(f, NULL);
    de_cook_result r;
    de_cooked_texture_view v;
    /* BC7 sRGB, 16x16, 5 levels: every block byte as stored, (i * 7 + m) & 255. */
    ASSERT_EQ(cook(f, "bc7.ktx2", NULL, 0, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.format, (u32)DE_FMT_BC7_SRGB);
    EXPECT_EQ(v.width, 16u);
    EXPECT_EQ(v.mip_count, 5u);
    EXPECT_EQ(v.block.width, 4u);
    EXPECT_TRUE((v.flags & DE_COOKED_TEXTURE_SRGB) != 0);
    bool same = true;
    for (u32 m = 0; m < 5; ++m) {
        de_cooked_texture_subresource s = sub_of(&v, 0, m);
        u32 bx = (s.width + 3u) / 4u, by = (s.height + 3u) / 4u;
        EXPECT_EQ(s.row_count, by);
        for (u32 y = 0; y < by; ++y)
            for (u32 b = 0; b < bx * 16u; ++b) {
                u32 i = y * bx * 16u + b;
                if (v.blob[s.offset + (u64)y * s.row_pitch + b] != (u8)((i * 7u + m) & 255u))
                    same = false;
            }
    }
    EXPECT_TRUE(same);
    EXPECT_TRUE(layout_ok(&v));
    de_cook_result_free(&r);

    /* BC4 with zstd supercompression, 8x8, 4 levels; mips = false keeps level 0 only. */
    ASSERT_EQ(cook(f, "bc4_zstd.ktx2", NULL, 0, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.format, (u32)DE_FMT_BC4_UNORM);
    EXPECT_EQ(v.mip_count, 4u);
    de_cooked_texture_subresource s1 = sub_of(&v, 0, 1); /* 4x4: one block of 8 bytes */
    for (u32 b = 0; b < 8u; ++b)
        EXPECT_EQ(v.blob[s1.offset + b], (u8)((b * 13u + 5u) & 255u));
    de_cook_result_free(&r);
    de__manifest_kv nm = kv_bool("mips", false);
    ASSERT_EQ(cook(f, "bc4_zstd.ktx2", &nm, 1, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.mip_count, 1u);
    de_cook_result_free(&r);
    /* max_size 8 on the 16x16 BC7: levels from 8x8 on. */
    de__manifest_kv ms = kv_int("max_size", 8);
    ASSERT_EQ(cook(f, "bc7.ktx2", &ms, 1, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.width, 8u);
    EXPECT_EQ(v.mip_count, 4u);
    EXPECT_EQ(v.blob[sub_of(&v, 0, 0).offset], (u8)1u); /* level 1 byte 0: (0 * 7 + 1) */
    de_cook_result_free(&r);
}

/* ---- import.texture.import_ktx2_raw ---------------------------------------------------------- */

TEST_F(texture, import_ktx2_raw, tex_fx) {
    make_cooker(f, NULL);
    de_cook_result r;
    de_cooked_texture_view v;
    ASSERT_EQ(cook(f, "rgba8_srgb.ktx2", NULL, 0, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.format, (u32)DE_FMT_RGBA8_SRGB);
    EXPECT_EQ(v.mip_count, 3u);
    EXPECT_MEMEQ(texel_at(&v, 0, 0, 2, 3), ((const u8[]){170, 255, 0, 255}), 4);
    de_cook_result_free(&r);
    ASSERT_EQ(cook(f, "rgba16f.ktx2", NULL, 0, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.format, (u32)DE_FMT_RGBA16_FLOAT);
    EXPECT_FLOAT_EQ(half_at(texel_at(&v, 0, 0, 3, 1), 0), 3.0f, 0.0f);
    EXPECT_FLOAT_EQ(half_at(texel_at(&v, 0, 0, 3, 1), 1), 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(half_at(texel_at(&v, 0, 0, 3, 1), 2), 4.0f, 0.0f);
    de_cook_result_free(&r);
    /* A cube map: six faces, each with its mips, face f filled with f * 40. */
    de__manifest_kv lin = kv_bool("srgb", false);
    ASSERT_EQ(cook(f, "cube_rgba8.ktx2", &lin, 1, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.dim, (u32)DE_TEX_CUBE);
    EXPECT_EQ(v.depth_or_layers, 6u);
    EXPECT_EQ(v.mip_count, 3u);
    EXPECT_EQ(v.subresource_count, 18u);
    EXPECT_TRUE((v.flags & DE_COOKED_TEXTURE_CUBE) != 0);
    for (u32 face = 0; face < 6; ++face) {
        EXPECT_EQ(texel_at(&v, face, 0, 1, 1)[0], (u8)(face * 40u));
        EXPECT_EQ(texel_at(&v, face, 2, 0, 0)[1], (u8)(face * 40u));
    }
    EXPECT_TRUE(layout_ok(&v));
    de_cook_result_free(&r);
}

/* ---- import.texture.mips_srgb_linear_filter -------------------------------------------------- */

TEST_F(texture, mips_srgb_linear_filter, tex_fx) {
    /* The transfer functions. */
    EXPECT_FLOAT_EQ(de_tex_srgb_to_linear(0.0f), 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(de_tex_srgb_to_linear(1.0f), 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(de_tex_linear_to_srgb(0.5f) * 255.0f, 187.5f, 0.1f);
    EXPECT_FLOAT_EQ(de_tex_srgb_to_linear(de_tex_linear_to_srgb(0.2f)), 0.2f, 1e-5f);

    make_cooker(f, NULL);
    de_cook_result r;
    de_cooked_texture_view v;
    /* A 2x2 checker of sRGB 0 and 255 filters to linear 0.5: sRGB 188, not 128. */
    ASSERT_EQ(cook(f, "checker.png", NULL, 0, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    ASSERT_EQ(v.mip_count, 2u);
    EXPECT_MEMEQ(texel_at(&v, 0, 1, 0, 0), ((const u8[]){188, 188, 188, 255}), 4);
    de_cook_result_free(&r);
    /* Linear data (srgb = false) averages the stored values: 128. */
    de__manifest_kv lin = kv_bool("srgb", false);
    ASSERT_EQ(cook(f, "checker.png", &lin, 1, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_MEMEQ(texel_at(&v, 0, 1, 0, 0), ((const u8[]){128, 128, 128, 255}), 4);
    de_cook_result_free(&r);
    /* Clamp addressing gives the same average for the checker. */
    de__manifest_kv clamp = kv_str("address", "clamp");
    ASSERT_EQ(cook(f, "checker.png", &clamp, 1, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_MEMEQ(texel_at(&v, 0, 1, 0, 0), ((const u8[]){188, 188, 188, 255}), 4);
    de_cook_result_free(&r);
}

/* ---- import.texture.mips_npot_chain ---------------------------------------------------------- */

TEST_F(texture, mips_npot_chain, tex_fx) {
    make_cooker(f, NULL);
    de_cook_result r;
    de_cooked_texture_view v;
    /* 5x3: 5x3, 2x1, 1x1 (floor sizes down to 1x1). */
    ASSERT_EQ(cook(f, "npot.png", NULL, 0, &r), DE_OK);
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    ASSERT_EQ(v.mip_count, 3u);
    de_cooked_texture_subresource s;
    s = sub_of(&v, 0, 1);
    EXPECT_EQ(s.width, 2u);
    EXPECT_EQ(s.height, 1u);
    EXPECT_EQ(s.row_count, 1u);
    s = sub_of(&v, 0, 2);
    EXPECT_EQ(s.width, 1u);
    EXPECT_EQ(s.height, 1u);
    EXPECT_TRUE(layout_ok(&v));
    de_cook_result_free(&r);

    /* The chain directly: 7x5 -> 3x2 -> 1x1; a constant image stays constant (weights sum to 1)
     * with both address modes. */
    de_tex_image img;
    memset(&img, 0, sizeof img);
    img.width = 7;
    img.height = 5;
    img.layers = 1;
    ASSERT_EQ(de_tex_image_alloc(&f->alloc, &img), DE_OK);
    for (u32 i = 0; i < 7u * 5u * 4u; ++i)
        img.rgba[i] = 0.25f;
    for (u32 mode = 0; mode < 2; ++mode) {
        de_tex_mip_params p = {.address = (de_tex_address)mode, .full_chain = true};
        de_tex_chain c;
        ASSERT_EQ(de_tex_mips_build(&f->alloc, &img, &p, &c), DE_OK);
        ASSERT_EQ(c.mip_count, 3u);
        EXPECT_EQ(c.width[1], 3u);
        EXPECT_EQ(c.height[1], 2u);
        EXPECT_EQ(c.width[2], 1u);
        EXPECT_EQ(c.height[2], 1u);
        for (u32 m = 0; m < 3; ++m)
            for (u32 i = 0; i < c.width[m] * c.height[m] * 4u; ++i)
                EXPECT_FLOAT_EQ(c.rgba[m][i], 0.25f, 1e-6f);
        de_tex_chain_free(&f->alloc, &c);
    }
    de_tex_image_free(&f->alloc, &img);
}

/* ---- import.texture.mips_normal_renormalize -------------------------------------------------- */

TEST_F(texture, mips_normal_renormalize, tex_fx) {
    u64 n = 0;
    void *png = read_file(f, "texd:/normal.png", &n);
    ASSERT_TRUE(png != NULL);
    de_tex_image img;
    ASSERT_EQ(de_tex_decode_stb(&f->alloc, png, n, &img), DE_OK);
    free_file(f, png, n);
    for (u32 i = 0; i < img.width * img.height; ++i)
        for (u32 c = 0; c < 3; ++c)
            img.rgba[i * 4u + c] = img.rgba[i * 4u + c] * 2.0f - 1.0f;
    de_tex_mip_params p = {.address = DE_TEX_ADDRESS_WRAP, .normal_map = true, .full_chain = true};
    de_tex_chain c;
    ASSERT_EQ(de_tex_mips_build(&f->alloc, &img, &p, &c), DE_OK);
    ASSERT_EQ(c.mip_count, 6u);
    f32 worst = 0.0f;
    for (u32 m = 0; m < c.mip_count; ++m)
        for (u32 i = 0; i < c.width[m] * c.height[m]; ++i) {
            const f32 *t = c.rgba[m] + i * 4u;
            f32 len = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            f32 d = fabsf(len - 1.0f);
            worst = d > worst ? d : worst;
        }
    EXPECT_TRUE(worst <= 1e-3f);
    /* The bump flattens in the small mips: the 1x1 normal points up. */
    EXPECT_TRUE(c.rgba[5][2] > 0.99f);
    de_tex_chain_free(&f->alloc, &c);
    de_tex_image_free(&f->alloc, &img);

    /* Cooked with usage = "normal": UNORM (never sRGB, even when asked), unit length after the
     * [0, 1] decode within 8-bit quantization. */
    make_cooker(f, NULL);
    de__manifest_kv kv[2] = {kv_str("usage", "normal"), kv_bool("srgb", true)};
    de_cook_result r;
    ASSERT_EQ(cook(f, "normal.png", kv, 2, &r), DE_OK);
    de_cooked_texture_view v;
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    EXPECT_EQ(v.format, (u32)DE_FMT_RGBA8_UNORM);
    EXPECT_EQ(v.usage, (u32)DE_COOKED_TEXTURE_USAGE_NORMAL);
    worst = 0.0f;
    for (u32 m = 0; m < v.mip_count; ++m) {
        de_cooked_texture_subresource s = sub_of(&v, 0, m);
        for (u32 y = 0; y < s.height; ++y)
            for (u32 x = 0; x < s.width; ++x) {
                const u8 *t = texel_at(&v, 0, m, x, y);
                f32 nx = t[0] / 255.0f * 2.0f - 1.0f, ny = t[1] / 255.0f * 2.0f - 1.0f,
                    nz = t[2] / 255.0f * 2.0f - 1.0f;
                f32 d = fabsf(sqrtf(nx * nx + ny * ny + nz * nz) - 1.0f);
                worst = d > worst ? d : worst;
            }
    }
    EXPECT_TRUE(worst <= 0.015f);
    de_cook_result_free(&r);
}

/* ---- import.texture.mips_alpha_coverage ------------------------------------------------------ */

/* The largest coverage difference from mip 0 over the mips with at least `min_texels` texels. */
static f32 coverage_drift(const de_tex_chain *c, f32 cutoff, u32 min_texels) {
    f32 c0 = de_tex_alpha_coverage(c->rgba[0], c->width[0], c->height[0], cutoff), worst = 0.0f;
    for (u32 m = 1; m < c->mip_count; ++m) {
        if (c->width[m] * c->height[m] < min_texels)
            break;
        f32 d = fabsf(de_tex_alpha_coverage(c->rgba[m], c->width[m], c->height[m], cutoff) - c0);
        worst = d > worst ? d : worst;
    }
    return worst;
}

TEST_F(texture, mips_alpha_coverage, tex_fx) {
    u64 n = 0;
    void *png = read_file(f, "texd:/cutout.png", &n);
    ASSERT_TRUE(png != NULL);
    de_tex_image img;
    ASSERT_EQ(de_tex_decode_stb(&f->alloc, png, n, &img), DE_OK);
    free_file(f, png, n);
    const f32 cutoff = 0.8f;
    /* Without the setting, filtering shrinks the area above a high cutoff. */
    de_tex_mip_params p = {.address = DE_TEX_ADDRESS_CLAMP, .full_chain = true};
    de_tex_chain c;
    ASSERT_EQ(de_tex_mips_build(&f->alloc, &img, &p, &c), DE_OK);
    f32 plain = coverage_drift(&c, cutoff, 64);
    de_tex_chain_free(&f->alloc, &c);
    /* With it, every mip of 64 texels or more keeps mip 0's coverage within 2 %. */
    p.alpha_coverage = cutoff;
    ASSERT_EQ(de_tex_mips_build(&f->alloc, &img, &p, &c), DE_OK);
    f32 kept = coverage_drift(&c, cutoff, 64);
    de_tex_chain_free(&f->alloc, &c);
    de_tex_image_free(&f->alloc, &img);
    printf("  coverage drift at %.2f: %.4f plain, %.4f kept\n", (double)cutoff, (double)plain,
           (double)kept);
    EXPECT_TRUE(plain > 0.02f);
    EXPECT_TRUE(kept <= 0.02f);

    /* Through the importer: the cooked alpha bytes keep it too (mips of 256 texels or more,
     * where one 8-bit rounding flip is under 0.5 %). */
    make_cooker(f, NULL);
    de__manifest_kv kv = kv_float("alpha_coverage", cutoff);
    de_cook_result r;
    ASSERT_EQ(cook(f, "cutout.png", &kv, 1, &r), DE_OK);
    de_cooked_texture_view v;
    ASSERT_TRUE(view_of(r.data, r.size, &v));
    f32 cov[DE_COOKED_TEXTURE_MAX_MIPS];
    for (u32 m = 0; m < v.mip_count; ++m) {
        de_cooked_texture_subresource s = sub_of(&v, 0, m);
        u32 hit = 0;
        for (u32 y = 0; y < s.height; ++y)
            for (u32 x = 0; x < s.width; ++x)
                hit += texel_at(&v, 0, m, x, y)[3] / 255.0f >= cutoff ? 1u : 0u;
        cov[m] = (f32)hit / (f32)(s.width * s.height);
        if (s.width * s.height >= 256u)
            EXPECT_TRUE(fabsf(cov[m] - cov[0]) <= 0.02f);
    }
    de_cook_result_free(&r);
}

/* ---- import.texture.layout_alignment --------------------------------------------------------- */

TEST_F(texture, layout_alignment, tex_fx) {
    make_cooker(f, NULL);
    static const char *const sources[] = {"rgba8.png", "npot.png",        "half_rgb.exr",
                                          "bc7.ktx2",  "cube_rgba8.ktx2", "photo.jpg"};
    for (u32 i = 0; i < TEX_COUNT(sources); ++i) {
        de_cook_result r;
        ASSERT_EQ(cook(f, sources[i], NULL, 0, &r), DE_OK);
        de_cooked_texture_view v;
        ASSERT_TRUE(view_of(r.data, r.size, &v));
        if (!layout_ok(&v))
            printf("  layout of %s\n", sources[i]);
        EXPECT_TRUE(layout_ok(&v));
        de_cook_result_free(&r);
    }

    /* The reader helper: the header round-trips, and damaged blobs are refused. */
    de_tex_dtex_desc d = {.dim = DE_TEX_2D_ARRAY,
                          .format = DE_FMT_RGBA8_UNORM,
                          .width = 300,
                          .height = 7,
                          .layers = 3,
                          .mip_count = 4,
                          .flags = DE_COOKED_TEXTURE_HAS_ALPHA,
                          .usage = DE_COOKED_TEXTURE_USAGE_MASK};
    de_tex_dtex t;
    ASSERT_EQ(de_tex_dtex_begin(&f->alloc, &d, &t), DE_OK);
    de_cooked_texture_view v;
    ASSERT_TRUE(view_of(t.data, t.size, &v));
    EXPECT_EQ(v.dim, (u32)DE_TEX_2D_ARRAY);
    EXPECT_EQ(v.format, (u32)DE_FMT_RGBA8_UNORM);
    EXPECT_EQ(v.width, 300u);
    EXPECT_EQ(v.height, 7u);
    EXPECT_EQ(v.depth_or_layers, 3u);
    EXPECT_EQ(v.mip_count, 4u);
    EXPECT_EQ(v.flags, (u32)DE_COOKED_TEXTURE_HAS_ALPHA);
    EXPECT_EQ(v.usage, (u32)DE_COOKED_TEXTURE_USAGE_MASK);
    EXPECT_EQ(v.subresource_count, 12u);
    de_cooked_texture_subresource s = sub_of(&v, 0, 0);
    EXPECT_EQ(s.row_pitch, 1280u); /* 1200 bytes rounded up to 256 */
    EXPECT_EQ(s.row_count, 7u);
    s = sub_of(&v, 2, 3); /* layer 2, mip 3: 37x1 */
    EXPECT_EQ(s.width, 37u);
    EXPECT_EQ(s.height, 1u);
    EXPECT_EQ(s.row_pitch, 256u);
    EXPECT_TRUE(layout_ok(&v));
    u8 *p = t.data;
    const char *why = NULL;
    p[0] = 'X';
    EXPECT_FALSE(de_cooked_texture_parse(p, t.size, &v, &why));
    p[0] = 'D';
    EXPECT_FALSE(de_cooked_texture_parse(p, t.size - 1u, &v, &why)); /* total size */
    u8 *e = p + DE_COOKED_TEXTURE_HEADER_SIZE + DE_COOKED_TEXTURE_SUBRESOURCE_SIZE;
    u8 keep = e[0];
    e[0] = (u8)(keep + 16u); /* subresource 1 offset no longer 512-aligned */
    EXPECT_FALSE(de_cooked_texture_parse(p, t.size, &v, &why));
    e[0] = keep;
    keep = e[16];
    e[16] = (u8)(keep + 4u); /* row pitch no longer 256-aligned */
    EXPECT_FALSE(de_cooked_texture_parse(p, t.size, &v, &why));
    e[16] = keep;
    de_cooked_put_u32(p + 28, 10u); /* more mips than 300x7 has (9) */
    EXPECT_FALSE(de_cooked_texture_parse(p, t.size, &v, &why));
    de_cooked_put_u32(p + 28, 4u);
    EXPECT_TRUE(de_cooked_texture_parse(p, t.size, &v, &why));
    de_tex_dtex_free(&f->alloc, &t);
}

/* ---- import.texture.cook_deterministic ------------------------------------------------------- */

TEST_F(texture, cook_deterministic, tex_fx) {
    /* Each cook already runs the importer twice (verify_determinism); a second cooker must give
     * the same bytes too. */
    static const char *const sources[] = {"rgba8.png",  "rgb16.png",      "photo.jpg",
                                          "cutout.png", "float_rgba.exr", "uastc.ktx2",
                                          "etc1s.ktx2", "bc4_zstd.ktx2",  "cube_rgba8.ktx2"};
    de__manifest_kv kv[2] = {kv_float("alpha_coverage", 0.5), kv_int("max_size", 32)};
    make_cooker(f, NULL);
    de_cooker *c1 = f->c;
    f->c = NULL;
    make_cooker(f, NULL);
    for (u32 i = 0; i < TEX_COUNT(sources); ++i) {
        de_cook_result a, b;
        ASSERT_EQ(cook(f, sources[i], kv, 2, &a), DE_OK);
        de_cooker *c2 = f->c;
        f->c = c1;
        ASSERT_EQ(cook(f, sources[i], kv, 2, &b), DE_OK);
        f->c = c2;
        EXPECT_EQ(a.size, b.size);
        EXPECT_TRUE(a.size == b.size && memcmp(a.data, b.data, (size_t)a.size) == 0);
        EXPECT_EQ(a.output_hash, b.output_hash);
        de_cook_result_free(&a);
        de_cook_result_free(&b);
    }
    de_cooker_destroy(c1);
}

/* ---- import.texture.reject_oversize ---------------------------------------------------------- */

static u32 crc32_of(const u8 *p, size_t n) {
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (u32 k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void put_be32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

TEST_F(texture, reject_oversize, tex_fx) {
    mkdirs("ttmp:/tex/big");
    /* A PNG header of 16385 x 1: refused from the header, before any pixel is decoded. */
    u8 png[8 + 25 + 12];
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    memcpy(png, sig, 8);
    put_be32(png + 8, 13);
    memcpy(png + 12, "IHDR", 4);
    put_be32(png + 16, 16385);
    put_be32(png + 20, 1);
    png[24] = 8; /* depth */
    png[25] = 2; /* RGB */
    png[26] = png[27] = png[28] = 0;
    put_be32(png + 29, crc32_of(png + 12, 17));
    put_be32(png + 33, 0);
    memcpy(png + 37, "IEND", 4);
    put_be32(png + 41, crc32_of(png + 37, 4));
    ASSERT_EQ(de_file_write_all_atomic("ttmp:/tex/big/wide.png", png, sizeof png), DE_OK);
    /* A KTX2 header of 4 x 16400, and a 3D one. */
    u8 ktx[80 + 24];
    memset(ktx, 0, sizeof ktx);
    static const u8 id[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    memcpy(ktx, id, 12);
    de_cooked_put_u32(ktx + 12, 37); /* R8G8B8A8_UNORM */
    de_cooked_put_u32(ktx + 16, 1);
    de_cooked_put_u32(ktx + 20, 4);
    de_cooked_put_u32(ktx + 24, 16400);
    de_cooked_put_u32(ktx + 36, 1); /* faces */
    de_cooked_put_u32(ktx + 40, 1); /* levels */
    ASSERT_EQ(de_file_write_all_atomic("ttmp:/tex/big/tall.ktx2", ktx, sizeof ktx), DE_OK);
    de_cooked_put_u32(ktx + 24, 4);
    de_cooked_put_u32(ktx + 28, 4); /* depth */
    ASSERT_EQ(de_file_write_all_atomic("ttmp:/tex/big/volume.ktx2", ktx, sizeof ktx), DE_OK);

    make_cooker(f, NULL);
    de_cook_result r;
    EXPECT_EQ(cook_at(f, "ttmp:/tex/big", "wide.png", NULL, 0, &r), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(error_says("16385x1 is larger than the limit of 16384"));
    EXPECT_EQ(cook_at(f, "ttmp:/tex/big", "tall.ktx2", NULL, 0, &r), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(error_says("4x16400 is larger than the limit of 16384"));
    EXPECT_EQ(cook_at(f, "ttmp:/tex/big", "volume.ktx2", NULL, 0, &r), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(error_says("3D textures"));
    /* The DTEX reader refuses such sizes too. */
    EXPECT_EQ(de_tex_check_size(16384, 16384, "t"), DE_OK);
    EXPECT_EQ(de_tex_check_size(16384, 16385, "t"), DE_ERR_UNSUPPORTED);
}

/* ---- import.texture.reject_corrupt ----------------------------------------------------------- */

TEST_F(texture, reject_corrupt, tex_fx) {
    mkdirs("ttmp:/tex/bad");
    make_cooker(f, NULL);
    de_cook_result r;
    /* Truncated copies of real fixtures. */
    static const struct {
        const char *src, *dst;
        u64 keep;
    } cuts[] = {{"texd:/rgba8.png", "ttmp:/tex/bad/cut.png", 60},
                {"texd:/float_rgba.exr", "ttmp:/tex/bad/cut.exr", 200},
                {"texd:/half_rgb.exr", "ttmp:/tex/bad/head.exr", 40},
                {"texd:/bc7.ktx2", "ttmp:/tex/bad/cut.ktx2", 300},
                {"texd:/uastc.ktx2", "ttmp:/tex/bad/cut_uastc.ktx2", 200}};
    for (u32 i = 0; i < TEX_COUNT(cuts); ++i) {
        u64 n = 0;
        void *p = read_file(f, cuts[i].src, &n);
        ASSERT_TRUE(p != NULL && n > cuts[i].keep);
        ASSERT_EQ(de_file_write_all_atomic(cuts[i].dst, p, cuts[i].keep), DE_OK);
        free_file(f, p, n);
        const char *name = strrchr(cuts[i].dst, '/') + 1;
        de_result res = cook_at(f, "ttmp:/tex/bad", name, NULL, 0, &r);
        if (res != DE_ERR_PARSE)
            printf("  %s: %d (%s)\n", name, (int)res,
                   de_error_last() != NULL ? de_error_last()->message : "");
        EXPECT_EQ(res, DE_ERR_PARSE);
    }
    /* Not an image at all. */
    ASSERT_EQ(de_file_write_all_atomic("ttmp:/tex/bad/junk.png", "hello, texture", 14), DE_OK);
    EXPECT_EQ(cook_at(f, "ttmp:/tex/bad", "junk.png", NULL, 0, &r), DE_ERR_PARSE);
    EXPECT_TRUE(error_says("is not a PNG, JPEG, EXR or KTX2 file"));
    /* A KTX2 file whose level points past its end. */
    u64 n = 0;
    u8 *k = (u8 *)read_file(f, "texd:/bc7.ktx2", &n);
    ASSERT_TRUE(k != NULL);
    de_cooked_put_u64(k + 80, n + 100u);
    ASSERT_EQ(de_file_write_all_atomic("ttmp:/tex/bad/far.ktx2", k, n), DE_OK);
    EXPECT_EQ(cook_at(f, "ttmp:/tex/bad", "far.ktx2", NULL, 0, &r), DE_ERR_PARSE);
    EXPECT_TRUE(error_says("lies outside the file"));
    /* An unsupported VkFormat (BC2). */
    de_cooked_put_u64(k + 80, 0);
    free_file(f, k, n);
    k = (u8 *)read_file(f, "texd:/bc7.ktx2", &n);
    ASSERT_TRUE(k != NULL);
    de_cooked_put_u32(k + 12, 135);
    ASSERT_EQ(de_file_write_all_atomic("ttmp:/tex/bad/bc2.ktx2", k, n), DE_OK);
    free_file(f, k, n);
    EXPECT_EQ(cook_at(f, "ttmp:/tex/bad", "bc2.ktx2", NULL, 0, &r), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(error_says("VkFormat 135 is not supported"));

    /* Bad settings. */
    static const struct {
        de__manifest_kv kv;
        const char *needle;
    } bad[] = {
        {{"usage", {DE__MANIFEST_STRING, {.s = "albedo"}}}, "usage \"albedo\""},
        {{"alpha_coverage", {DE__MANIFEST_FLOAT, {.f = 1.5}}}, "alpha_coverage"},
        {{"max_size", {DE__MANIFEST_INT, {.i = 0}}}, "max_size"},
        {{"max_size", {DE__MANIFEST_INT, {.i = 20000}}}, "max_size"},
        {{"address", {DE__MANIFEST_STRING, {.s = "mirror"}}}, "address \"mirror\""},
    };
    for (u32 i = 0; i < TEX_COUNT(bad); ++i) {
        EXPECT_EQ(cook(f, "rgba8.png", &bad[i].kv, 1, &r), DE_ERR_INVALID_ARGUMENT);
        EXPECT_TRUE(error_says(bad[i].needle));
    }
}

/* ---- import.texture.settings_hash ------------------------------------------------------------ */

TEST_F(texture, settings_hash, tex_fx) {
    /* Every setting is in the settings hash: srgb and usage change it. */
    de__manifest_kv a[2] = {kv_bool("srgb", true), kv_str("usage", "color")};
    de__manifest_kv b[2] = {kv_bool("srgb", false), kv_str("usage", "color")};
    de__manifest_kv c[2] = {kv_bool("srgb", true), kv_str("usage", "mask")};
    u64 ha = de_cook_settings_hash(a, 2), hb = de_cook_settings_hash(b, 2),
        hc = de_cook_settings_hash(c, 2);
    EXPECT_TRUE(ha != hb && ha != hc && hb != hc);

    /* And a changed setting misses the `.decache`, an unchanged one hits. */
    mkdirs("ttmp:/tex/cache");
    make_cooker(f, "ttmp:/tex/cache/run");
    de_cooker_cache_stats st0, st;
    de_cook_result r;
    ASSERT_EQ(cook(f, "rgba8.png", a, 2, &r), DE_OK); /* may hit a cache left by an earlier run */
    de_cook_result_free(&r);
    de_cooker_get_cache_stats(f->c, &st0);
    ASSERT_EQ(cook(f, "rgba8.png", a, 2, &r), DE_OK);
    de_cook_result_free(&r);
    de_cooker_get_cache_stats(f->c, &st);
    EXPECT_EQ(st.hits, st0.hits + 1u);
    /* Settings that were never cooked in this directory: a new nonce makes them new. */
    char nonce[32];
    snprintf(nonce, sizeof nonce, "%llu", (unsigned long long)de_time_now_ns());
    de__manifest_kv b3[3] = {b[0], b[1], kv_str("note", nonce)};
    de__manifest_kv c3[3] = {c[0], c[1], kv_str("note", nonce)};
    de_cooker_get_cache_stats(f->c, &st0);
    ASSERT_EQ(cook(f, "rgba8.png", b3, 3, &r), DE_OK);
    de_cook_result_free(&r);
    ASSERT_EQ(cook(f, "rgba8.png", c3, 3, &r), DE_OK);
    de_cook_result_free(&r);
    de_cooker_get_cache_stats(f->c, &st);
    EXPECT_EQ(st.misses, st0.misses + 2u);
    ASSERT_EQ(cook(f, "rgba8.png", b3, 3, &r), DE_OK);
    de_cooker_get_cache_stats(f->c, &st);
    EXPECT_EQ(st.misses, st0.misses + 2u);
    EXPECT_EQ(st.hits, st0.hits + 1u);
    de_cook_result_free(&r);
}

/* ---- import.texture.model_textures_cook_to_dtex ---------------------------------------------- */

TEST_F(texture, model_textures_cook_to_dtex, tex_fx) {
    /* With the texture importer registered, the glTF model importer's texture sub-assets are
     * DTEX blobs (DENG-101 cooks them through the registry). */
    make_cooker(f, NULL);
    de_asset_type_desc model = {
        .name = "model", .importer_version = DE_MODEL_IMPORTER_VERSION, .import = de_model_import};
    ASSERT_EQ(de_cooker_register_type(f->c, &model), DE_OK);
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = "props/boxtex";
    a.type = "model";
    a.source = "BoxTextured.gltf";
    a.file = "texg:/assets.toml";
    de_cook_result r;
    ASSERT_EQ(de_cooker_cook(f->c, &(de_cook_input){.asset = &a, .asset_root = "texg:/"}, &r),
              DE_OK);
    const de_cook_subasset *t = de_cook_result_subasset(&r, "texture:0");
    ASSERT_TRUE(t != NULL);
    EXPECT_STREQ(t->type, "texture");
    de_cooked_texture_view v;
    ASSERT_TRUE(view_of(t->data, t->size, &v));
    /* Base colour: srgb = true, usage = color, compression = "auto" (DENG-99): BC7 sRGB. */
    EXPECT_EQ(v.format, (u32)DE_FMT_BC7_SRGB);
    EXPECT_EQ(v.usage, (u32)DE_COOKED_TEXTURE_USAGE_COLOR);
    EXPECT_EQ(v.width, 256u);
    EXPECT_EQ(v.mip_count, 9u);
    de_cook_result_free(&r);

    /* A normal map and a linear mask (data URIs inside the glTF). */
    a.id = "props/cc";
    a.source = "khr_clearcoat.gltf";
    ASSERT_EQ(de_cooker_cook(f->c, &(de_cook_input){.asset = &a, .asset_root = "texg:/"}, &r),
              DE_OK);
    const de_cook_subasset *nrm = de_cook_result_subasset(&r, "texture:nrm");
    const de_cook_subasset *lin = de_cook_result_subasset(&r, "texture:lin");
    ASSERT_TRUE(nrm != NULL && lin != NULL);
    /* Both images are smaller than a 4x4 block, so compression = "auto" keeps them uncompressed
     * (DENG-99: BC needs mip 0 to be a multiple of 4). */
    ASSERT_TRUE(view_of(nrm->data, nrm->size, &v));
    EXPECT_TRUE(v.width % 4u != 0 || v.height % 4u != 0);
    EXPECT_EQ(v.format, (u32)DE_FMT_RGBA8_UNORM);
    EXPECT_EQ(v.usage, (u32)DE_COOKED_TEXTURE_USAGE_NORMAL);
    ASSERT_TRUE(view_of(lin->data, lin->size, &v));
    EXPECT_EQ(v.format, (u32)DE_FMT_RGBA8_UNORM);
    EXPECT_EQ(v.usage, (u32)DE_COOKED_TEXTURE_USAGE_MASK);
    de_cook_result_free(&r);
}

/* ---- import.texture.runtime_in_process ------------------------------------------------------- *
 *
 * Dev in-process cooking (the cook stage, DENG-209): a runtime `texture` type with this importer
 * cooks a PNG on load, and the loader gets the DTEX bytes. The loader is a stand-in; DENG-105
 * writes the real one. */

#if DE_HAS_COOKER
typedef struct rt_state {
    const de_allocator *alloc;
    u32 format, mips;
    bool parsed;
} rt_state;

static de_result rt_load(de_asset_load_ctx *ctx) {
    rt_state *st = (rt_state *)de_asset_load_ctx_user(ctx);
    u64 n = 0;
    const void *p = de_asset_load_ctx_data(ctx, &n);
    de_cooked_texture_view v;
    st->parsed = de_cooked_texture_parse(p, n, &v, NULL);
    st->format = v.format;
    st->mips = v.mip_count;
    u64 *o = (u64 *)DE_ALLOC(st->alloc, sizeof(u64), DE_ALIGNOF(u64), DE_MEM_ASSET);
    if (o == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    *o = n;
    de_asset_load_ctx_set_object(ctx, o, sizeof *o);
    return DE_OK;
}

static void rt_unload(void *object, void *user) {
    DE_FREE(((rt_state *)user)->alloc, object, sizeof(u64));
}
#endif

TEST_F(texture, runtime_in_process, tex_fx) {
#if !DE_HAS_COOKER
    SKIP("this build does not link the cooker into the runtime (DE_HAS_COOKER)");
#else
    mkdirs("ttmp:/tex/rt");
    mkdirs("ttmp:/tex/rt/assets");
    u64 n = 0;
    void *png = read_file(f, "texd:/rgba8.png", &n);
    ASSERT_TRUE(png != NULL);
    ASSERT_EQ(de_file_write_all_atomic("ttmp:/tex/rt/assets/rgba8.png", png, n), DE_OK);
    free_file(f, png, n);
    static const char manifest[] = "[project]\nasset_root = \"assets\"\n"
                                   "[defaults.texture]\nmips = true\n[assets]\n"
                                   "\"ui/logo\" = { type = \"texture\", source = \"rgba8.png\" }\n";
    ASSERT_EQ(de_file_write_all_atomic("ttmp:/tex/rt/assets.toml", manifest, sizeof manifest - 1u),
              DE_OK);
    rt_state st;
    memset(&st, 0, sizeof st);
    st.alloc = &f->alloc;
    de_fp_env_init_thread();
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &f->tasks), DE_OK);
    de_asset_system_desc d = {.allocator = &f->alloc,
                              .tasks = f->tasks,
                              .io_threads = 1,
                              .cache_dir = "nocache:/decache"};
    ASSERT_EQ(de_asset_system_create(&d, &f->s), DE_OK);
    de_asset_type_desc t = de_texture_import_type_desc();
    t.load = rt_load;
    t.unload = rt_unload;
    t.user = &st;
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->s, &t)));
    ASSERT_EQ(de_asset_manifest_load(f->s, "ttmp:/tex/rt/assets.toml"), DE_OK);
    de_load_handle h = de_asset_load(f->s, de_asset_id_from("ui/logo"), 0);
    de_load_state ls = de_asset_wait(f->s, h, (u64)60 * 1000000000u);
    de_load_info info;
    (void)de_asset_status(f->s, h, &info);
    if (ls != DE_LOAD_READY)
        printf("  %s\n", info.first_error_msg);
    EXPECT_EQ((int)ls, (int)DE_LOAD_READY);
    EXPECT_TRUE(st.parsed);
    EXPECT_EQ(st.format, (u32)DE_FMT_BC7_SRGB); /* DENG-99: compression = "auto" */
    EXPECT_EQ(st.mips, 5u);
    de_asset_release(f->s, h);
    de_asset_system_destroy(f->s);
    f->s = NULL;
#endif
}

/* ---- import.texture.de_cook_registers_texture ------------------------------------------------ */

#if DE_HAS_COOKER
static void dc_write(void *user, de_cook_stream stream, const char *text, size_t len) {
    (void)user;
    if (stream == DE_COOK_STDERR)
        fwrite(text, 1, len, stdout);
}
#endif

TEST_F(texture, de_cook_registers_texture, tex_fx) {
#if !DE_HAS_COOKER
    SKIP("`de-cook cook` needs the runtime importers (DE_HAS_COOKER)");
#else
    /* `de-cook cook` registers the cooker library's texture importer: a project with a PNG and
     * a KTX2 texture cooks to a pack whose entries are DTEX. */
    mkdirs("ttmp:/tex/dc");
    mkdirs("ttmp:/tex/dc/assets");
    static const char *const files[] = {"rgba8.png", "bc7.ktx2"};
    for (u32 i = 0; i < TEX_COUNT(files); ++i) {
        char from[64], to[64];
        snprintf(from, sizeof from, "texd:/%s", files[i]);
        snprintf(to, sizeof to, "ttmp:/tex/dc/assets/%s", files[i]);
        u64 n = 0;
        void *p = read_file(f, from, &n);
        ASSERT_TRUE(p != NULL);
        ASSERT_EQ(de_file_write_all_atomic(to, p, n), DE_OK);
        free_file(f, p, n);
    }
    static const char manifest[] =
        "[project]\nasset_root = \"assets\"\n[assets]\n"
        "\"ui/logo\" = { type = \"texture\", source = \"rgba8.png\", srgb = false }\n"
        "\"fx/bc\" = { type = \"texture\", source = \"bc7.ktx2\" }\n";
    ASSERT_EQ(de_file_write_all_atomic("ttmp:/tex/dc/assets.toml", manifest, sizeof manifest - 1u),
              DE_OK);
    const char *args[] = {DE_IMPORT_TEST_TMP_DIR "/tex/dc/assets.toml",
                          "--platform",
                          "linux",
                          "--out",
                          DE_IMPORT_TEST_TMP_DIR "/tex/dc/out",
                          "--no-cache",
                          "--jobs",
                          "1",
                          "--quiet"};
    de_cook_io io = {dc_write, NULL};
    ASSERT_EQ(de_cook_cook_run((int)TEX_COUNT(args), args, &io, NULL), DE_COOK_EXIT_OK);
    dpak_reader *pk = NULL;
    ASSERT_EQ(dpak_open("ttmp:/tex/dc/out/game.dpak",
                        &(dpak_open_desc){.allocator = &f->alloc, .verify = true}, &pk),
              DE_OK);
    static const struct {
        const char *id;
        u32 format;
    } want[] = {{"ui/logo", DE_FMT_BC7_UNORM}, /* DENG-99: auto, srgb = false */
                {"fx/bc", DE_FMT_BC7_SRGB}};
    for (u32 i = 0; i < TEX_COUNT(want); ++i) {
        u32 idx = dpak_find(pk, de_asset_id_from(want[i].id).hash);
        ASSERT_TRUE(idx != DPAK_NOT_FOUND);
        dpak_toc_entry e;
        dpak_entry(pk, idx, &e);
        EXPECT_EQ(e.type_hash, de_hash64_str("texture"));
        void *buf = DE_ALLOC(&f->alloc, (size_t)e.raw_size, 16, DE_MEM_ASSET);
        ASSERT_TRUE(buf != NULL);
        ASSERT_EQ(dpak_read_entry(pk, idx, NULL, buf, e.raw_size), DE_OK);
        de_cooked_texture_view v;
        EXPECT_TRUE(view_of(buf, e.raw_size, &v));
        EXPECT_EQ(v.format, want[i].format);
        DE_FREE(&f->alloc, buf, (size_t)e.raw_size);
    }
    dpak_close(pk);
#endif
}

TEST_F(texture, sprite_rt_proxy_cooked_trailer, tex_fx) {
    make_cooker(f, NULL);
    de_cook_result result;
    const de__manifest_kv settings[] = {kv_bool("rt_proxy", true), kv_bool("pixel_art", true)};
    ASSERT_EQ(cook(f, "cutout.png", settings, 2, &result), DE_OK);
    de_cooked_texture_view texture;
    ASSERT_TRUE(view_of(result.data, result.size, &texture));
    de_cooked_sprite_proxy proxy;
    ASSERT_TRUE(result.size >= DE_SPRITE_PROXY_BYTES);
    ASSERT_TRUE(de_sprite_proxy_parse((const u8 *)result.data + result.size - DE_SPRITE_PROXY_BYTES,
                                      DE_SPRITE_PROXY_BYTES, &proxy));
    ASSERT_LE(proxy.index_count / 3, 64u);
    ASSERT_GT(proxy.index_count, 0u);
    de_cook_result_free(&result);
}

TEST_F(texture, sprite_rt_proxy_atlas_region, tex_fx) {
    make_cooker(f, NULL);
    de_cook_result result;
    const de__manifest_kv settings[] = {kv_bool("rt_proxy", true), kv_bool("pixel_art", true),
                                        kv_float("rt_proxy_u", .5), kv_float("rt_proxy_width", .5)};
    ASSERT_EQ(cook(f, "cutout.png", settings, 4, &result), DE_OK);
    de_cooked_sprite_proxy proxy;
    ASSERT_TRUE(de_sprite_proxy_parse((const u8 *)result.data + result.size - DE_SPRITE_PROXY_BYTES,
                                      DE_SPRITE_PROXY_BYTES, &proxy));
    ASSERT_TRUE(proxy.region[0] == .5f);
    ASSERT_TRUE(proxy.region[2] == .5f);
    ASSERT_LE(proxy.index_count / 3, 64u);
    de_cook_result_free(&result);
}
