/*
 * test_texture_bc.c: block compression of the texture importer (DENG-99, SPEC §11.6, §11.2): the
 * BC1/BC3/BC4/BC5/BC6H/BC7 encoders against a reference decoder (bcdec, test-only), the
 * `compression = "auto"` rule table, padding of mips that end inside a block, output identical
 * for every worker count, KTX2 Basis payloads to BC, BC6H environment cubes (DENG-100), and the
 * settings. CTest names import.texture_bc.<test>.
 *
 * Fixtures: data/texture_bc/ (gen_bc_fixtures.py; mounted read-only as bcd:/) and the DENG-98
 * fixtures in data/texture/ (bct:/). Quality is measured on mip 0 against the same source cooked
 * with compression = "none", so both sides went through the same decode, sRGB and filter steps.
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "asset/manifest_internal.h"
#include "cook/importers/texture/texture_bc.h"
#include "cook/importers/texture/texture_import.h"
#include "cook/importers/texture/texture_internal.h"
#include "cook/lib/de_cooker.h"
#include "render/formats/de_cooked_env.h"
#include "render/formats/de_cooked_texture.h"

#include <bcdec.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define BC_COUNT(a) ((u32)(sizeof(a) / sizeof((a)[0])))

/* ---- Fixture --------------------------------------------------------------------------------- */

typedef struct bc_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_cooker *c;
} bc_fx;

/* Warnings of the `cook` category seen by the log sink (texture_bc_alpha_warning). */
static struct {
    u32 alpha_dropped;
    u32 total;
} g_warn;

static void bc_sink(const de_log_record *rec, void *user) {
    (void)user;
    if (rec->level != DE_LOG_WARN)
        return;
    ++g_warn.total;
    char text[512]; /* the message is a view, not NUL-terminated */
    size_t n = rec->message.len < sizeof text - 1u ? rec->message.len : sizeof text - 1u;
    memcpy(text, rec->message.ptr, n);
    text[n] = '\0';
    if (strstr(text, "alpha is dropped") != NULL)
        ++g_warn.alpha_dropped;
}

static void bc_mount(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("bcd", DE_IMPORT_TEST_DATA_DIR "/texture_bc", 0, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("bct", DE_IMPORT_TEST_DATA_DIR "/texture", 0, 0), DE_OK);
    mounted = true;
}

static void bc_fx_setup(bc_fx *f) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    de_log_add_sink(bc_sink, NULL);
    memset(&g_warn, 0, sizeof g_warn);
    bc_mount();
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
}

static void bc_fx_teardown(bc_fx *f) {
    if (f->c != NULL)
        de_cooker_destroy(f->c);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    de_log_shutdown();
}

/* A cooker with the texture importer; verify_determinism on (each import runs twice). */
static void make_cooker(bc_fx *f, de_task_system *tasks) {
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.allocator = &f->alloc,
                                                 .tasks = tasks,
                                                 .verify_determinism = DE_COOK_VERIFY_ON},
                               &f->c),
              DE_OK);
    de_asset_type_desc t = de_texture_import_type_desc();
    ASSERT_EQ(de_cooker_register_type(f->c, &t), DE_OK);
}

static de__manifest_kv kv_str(const char *k, const char *v) {
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = k;
    kv.value.type = DE__MANIFEST_STRING;
    kv.value.u.s = v;
    return kv;
}

static de__manifest_kv kv_bool(const char *k, bool v) {
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = k;
    kv.value.type = DE__MANIFEST_BOOL;
    kv.value.u.b = v;
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

/* Cooks `source` (a virtual path under bcd:/ or bct:/) as texture "tex/bc". */
static de_result cook(bc_fx *f, const char *root, const char *source, const de__manifest_kv *kv,
                      u32 n, de_cook_result *out) {
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = "tex/bc";
    a.type = "texture";
    a.source = source;
    a.file = "bcd:/assets.toml";
    a.settings = kv;
    a.setting_count = n;
    de_error_clear();
    return de_cooker_cook(f->c, &(de_cook_input){.asset = &a, .asset_root = root}, out);
}

static bool view_of(const de_cook_result *r, de_cooked_texture_view *v) {
    const char *why = NULL;
    if (!de_cooked_texture_parse(r->data, r->size, v, &why)) {
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

/* ---- Decoding -------------------------------------------------------------------------------- */

/* A decoded mip: RGBA8 texels (LDR formats) or RGB floats (BC6H, RGBA16F), w x h. */
typedef struct decoded {
    u32 w, h;
    u8 rgba8[64 * 64 * 4];
    f32 rgbf[64 * 64 * 3];
} decoded;

/* Decodes mip `mip` of layer `layer` into `d` (at most 64x64): BC formats through bcdec, RGBA8
 * and RGBA16F by reading the rows. Texels of the padding (past the mip's edge) are dropped. */
static bool decode_sub(const de_cooked_texture_view *v, u32 layer, u32 mip, decoded *d) {
    de_cooked_texture_subresource s = sub_of(v, layer, mip);
    if (s.width > 64u || s.height > 64u)
        return false;
    memset(d, 0, sizeof *d);
    d->w = s.width;
    d->h = s.height;
    const u8 *base = v->blob + s.offset;
    if (v->block.width == 1u) {
        for (u32 y = 0; y < s.height; ++y)
            for (u32 x = 0; x < s.width; ++x) {
                const u8 *t = base + (u64)y * s.row_pitch + (u64)x * v->block.bytes;
                u32 i = y * s.width + x;
                if (v->format == DE_FMT_RGBA16_FLOAT) {
                    for (u32 c = 0; c < 3; ++c)
                        d->rgbf[i * 3u + c] =
                            de_tex_f16_to_f32((u16)(t[2u * c] | (t[2u * c + 1u] << 8)));
                } else {
                    memcpy(d->rgba8 + i * 4u, t, 4);
                }
            }
        return true;
    }
    u32 bx_count = (s.width + 3u) / 4u, by_count = (s.height + 3u) / 4u;
    for (u32 by = 0; by < by_count; ++by)
        for (u32 bx = 0; bx < bx_count; ++bx) {
            const u8 *blk = base + (u64)by * s.row_pitch + (u64)bx * v->block.bytes;
            u8 px[16 * 4];
            f32 pf[16 * 3];
            memset(px, 0, sizeof px);
            memset(pf, 0, sizeof pf);
            switch (v->format) {
            case DE_FMT_BC1_SRGB:
                bcdec_bc1(blk, px, 16);
                break;
            case DE_FMT_BC3_SRGB:
                bcdec_bc3(blk, px, 16);
                break;
            case DE_FMT_BC4_UNORM: {
                u8 r[16];
                bcdec_bc4(blk, r, 4);
                for (u32 i = 0; i < 16u; ++i)
                    px[i * 4u] = r[i];
                break;
            }
            case DE_FMT_BC5_UNORM: {
                u8 rg[32];
                bcdec_bc5(blk, rg, 8);
                for (u32 i = 0; i < 16u; ++i) {
                    px[i * 4u] = rg[i * 2u];
                    px[i * 4u + 1u] = rg[i * 2u + 1u];
                }
                break;
            }
            case DE_FMT_BC6H_UFLOAT:
                bcdec_bc6h_float(blk, pf, 12, 0);
                break;
            case DE_FMT_BC7_UNORM:
            case DE_FMT_BC7_SRGB:
                bcdec_bc7(blk, px, 16);
                break;
            default:
                return false;
            }
            for (u32 ty = 0; ty < 4u; ++ty)
                for (u32 tx = 0; tx < 4u; ++tx) {
                    u32 x = bx * 4u + tx, y = by * 4u + ty;
                    if (x >= s.width || y >= s.height)
                        continue;
                    u32 i = y * s.width + x, t = ty * 4u + tx;
                    memcpy(d->rgba8 + i * 4u, px + t * 4u, 4);
                    memcpy(d->rgbf + i * 3u, pf + t * 3u, 3 * sizeof(f32));
                }
        }
    return true;
}

static bool decode_mip(const de_cooked_texture_view *v, u32 mip, decoded *d) {
    return decode_sub(v, 0, mip, d);
}

/* PSNR in dB of channel `c` (or the mean squared error of channels [c0, c1)) of two decoded
 * RGBA8 mips. Identical: 99. */
static f64 psnr8(const decoded *a, const decoded *b, u32 c0, u32 c1) {
    f64 se = 0.0;
    u64 n = 0;
    for (u32 i = 0; i < a->w * a->h; ++i)
        for (u32 c = c0; c < c1; ++c) {
            f64 d = (f64)a->rgba8[i * 4u + c] - (f64)b->rgba8[i * 4u + c];
            se += d * d;
            ++n;
        }
    if (se == 0.0)
        return 99.0;
    return 10.0 * log10(255.0 * 255.0 / (se / (f64)n));
}

/* HDR error in log space: e = log2(1 + v) per channel, PSNR = 20 log10(peak / RMSE) with the
 * peak the largest e of the reference. Errors in dark and bright areas count alike, as they do
 * to the eye after exposure. */
static f64 psnr_log(const decoded *ref, const decoded *got) {
    f64 se = 0.0, peak = 0.0;
    u64 n = (u64)ref->w * ref->h * 3u;
    for (u64 i = 0; i < n; ++i) {
        f64 r = log2(1.0 + (f64)(ref->rgbf[i] > 0.0f ? ref->rgbf[i] : 0.0f));
        f64 g = log2(1.0 + (f64)(got->rgbf[i] > 0.0f ? got->rgbf[i] : 0.0f));
        se += (r - g) * (r - g);
        peak = r > peak ? r : peak;
    }
    if (se == 0.0)
        return 99.0;
    return 20.0 * log10(peak / sqrt(se / (f64)n));
}

/* Cooks `source` twice, with `kv` and with compression = "none", decodes mip 0 of both. */
static bool cook_pair(bc_fx *f, const char *root, const char *source, const de__manifest_kv *kv,
                      u32 n, u32 want_format, decoded *got, decoded *ref) {
    de__manifest_kv none[8];
    u32 m = 0;
    for (u32 i = 0; i < n; ++i)
        if (strcmp(kv[i].key, "compression") != 0)
            none[m++] = kv[i];
    none[m++] = kv_str("compression", "none");
    de_cook_result r;
    de_cooked_texture_view v;
    memset(&v, 0, sizeof v);
    bool ok = cook(f, root, source, kv, n, &r) == DE_OK && view_of(&r, &v);
    if (ok) {
        if (v.format != want_format) {
            printf("  format %u, wanted %u\n", v.format, want_format);
            ok = false;
        }
        ok = ok && decode_mip(&v, 0, got);
        de_cook_result_free(&r);
    }
    if (!ok)
        return false;
    ok = cook(f, root, source, none, m, &r) == DE_OK && view_of(&r, &v) && decode_mip(&v, 0, ref);
    de_cook_result_free(&r);
    return ok;
}

/* ---- import.texture_bc.texture_bc7_psnr ------------------------------------------------------ */

TEST_F(texture_bc, texture_bc7_psnr, bc_fx) {
    make_cooker(f, NULL);
    static decoded got, ref;
    /* compression = "auto", colour, sRGB by default: BC7 sRGB. */
    ASSERT_TRUE(cook_pair(f, "bcd:/", "photo.png", NULL, 0, DE_FMT_BC7_SRGB, &got, &ref));
    f64 p = psnr8(&got, &ref, 0, 3);
    printf("  BC7 photo RGB PSNR: %.2f dB\n", p);
    EXPECT_TRUE(p >= 40.0);

    /* quality = "fast" still decodes close; rdo_quality trades a little PSNR. */
    de__manifest_kv fast[2] = {kv_str("compression", "bc7"), kv_str("quality", "fast")};
    ASSERT_TRUE(cook_pair(f, "bcd:/", "photo.png", fast, 2, DE_FMT_BC7_SRGB, &got, &ref));
    f64 pf = psnr8(&got, &ref, 0, 3);
    printf("  BC7 photo RGB PSNR, fast: %.2f dB\n", pf);
    EXPECT_TRUE(pf >= 38.0);
    de__manifest_kv rdo[1] = {kv_float("rdo_quality", 1.0)};
    ASSERT_TRUE(cook_pair(f, "bcd:/", "photo.png", rdo, 1, DE_FMT_BC7_SRGB, &got, &ref));
    f64 pr = psnr8(&got, &ref, 0, 3);
    printf("  BC7 photo RGB PSNR, rdo_quality 1: %.2f dB\n", pr);
    EXPECT_TRUE(pr >= 34.0);
    EXPECT_TRUE(pr <= p + 0.01);

    /* Alpha: the cutout keeps its alpha in BC7 (RGBA). */
    ASSERT_TRUE(cook_pair(f, "bcd:/", "cutout.png", NULL, 0, DE_FMT_BC7_SRGB, &got, &ref));
    f64 pa = psnr8(&got, &ref, 3, 4);
    printf("  BC7 cutout alpha PSNR: %.2f dB\n", pa);
    EXPECT_TRUE(pa >= 36.0);

    /* srgb = false: BC7 UNORM. */
    de__manifest_kv lin = kv_bool("srgb", false);
    ASSERT_TRUE(cook_pair(f, "bcd:/", "photo.png", &lin, 1, DE_FMT_BC7_UNORM, &got, &ref));
    EXPECT_TRUE(psnr8(&got, &ref, 0, 3) >= 40.0);
}

/* ---- import.texture_bc.texture_bc1_psnr ------------------------------------------------------ */

TEST_F(texture_bc, texture_bc1_psnr, bc_fx) {
    make_cooker(f, NULL);
    static decoded got, ref;
    de__manifest_kv bc1 = kv_str("compression", "bc1");
    ASSERT_TRUE(cook_pair(f, "bcd:/", "photo.png", &bc1, 1, DE_FMT_BC1_SRGB, &got, &ref));
    f64 p = psnr8(&got, &ref, 0, 3);
    printf("  BC1 photo RGB PSNR: %.2f dB\n", p);
    EXPECT_TRUE(p >= 32.0);
    /* BC1 blocks are always 4-colour: every decoded texel is opaque. */
    bool opaque = true;
    for (u32 i = 0; i < got.w * got.h; ++i)
        opaque = opaque && got.rgba8[i * 4u + 3u] == 255u;
    EXPECT_TRUE(opaque);

    /* BC3: BC1 colour plus a BC4 alpha block. */
    de__manifest_kv bc3 = kv_str("compression", "bc3");
    ASSERT_TRUE(cook_pair(f, "bcd:/", "cutout.png", &bc3, 1, DE_FMT_BC3_SRGB, &got, &ref));
    f64 pc = psnr8(&got, &ref, 0, 3), pa = psnr8(&got, &ref, 3, 4);
    printf("  BC3 cutout RGB PSNR: %.2f dB, alpha: %.2f dB\n", pc, pa);
    EXPECT_TRUE(pc >= 32.0);
    EXPECT_TRUE(pa >= 40.0);
}

/* ---- import.texture_bc.texture_bc4_bc5_psnr -------------------------------------------------- */

TEST_F(texture_bc, texture_bc4_bc5_psnr, bc_fx) {
    make_cooker(f, NULL);
    static decoded got, ref;
    /* A one-channel mask: BC4. */
    de__manifest_kv mask = kv_str("usage", "mask");
    ASSERT_TRUE(cook_pair(f, "bcd:/", "mask.png", &mask, 1, DE_FMT_BC4_UNORM, &got, &ref));
    f64 p4 = psnr8(&got, &ref, 0, 1);
    printf("  BC4 mask PSNR: %.2f dB\n", p4);
    EXPECT_TRUE(p4 >= 42.0);
    /* A normal map: BC5, x and y. */
    de__manifest_kv nrm = kv_str("usage", "normal");
    ASSERT_TRUE(cook_pair(f, "bcd:/", "normal.png", &nrm, 1, DE_FMT_BC5_UNORM, &got, &ref));
    f64 px = psnr8(&got, &ref, 0, 1), py = psnr8(&got, &ref, 1, 2);
    printf("  BC5 normal PSNR: x %.2f dB, y %.2f dB\n", px, py);
    EXPECT_TRUE(px >= 42.0);
    EXPECT_TRUE(py >= 42.0);
}

/* ---- import.texture_bc.texture_bc6h_hdr_error ------------------------------------------------ */

TEST_F(texture_bc, texture_bc6h_hdr_error, bc_fx) {
    make_cooker(f, NULL);
    static decoded got, ref;
    /* A float source: BC6H unsigned float, against the RGBA16F cook. */
    ASSERT_TRUE(cook_pair(f, "bcd:/", "hdr.exr", NULL, 0, DE_FMT_BC6H_UFLOAT, &got, &ref));
    f64 p = psnr_log(&ref, &got);
    printf("  BC6H HDR log-space PSNR: %.2f dB\n", p);
    EXPECT_TRUE(p >= 38.0);
    /* The sun stays bright: the largest decoded value is within 10% of the reference's. */
    f32 mr = 0.0f, mg = 0.0f;
    for (u32 i = 0; i < ref.w * ref.h * 3u; ++i) {
        mr = ref.rgbf[i] > mr ? ref.rgbf[i] : mr;
        mg = got.rgbf[i] > mg ? got.rgbf[i] : mg;
    }
    printf("  peak: reference %.1f, BC6H %.1f\n", (f64)mr, (f64)mg);
    EXPECT_TRUE(mr > 100.0f && fabsf(mg - mr) <= 0.1f * mr);
}

/* ---- import.texture_bc.texture_bc6h_environment ---------------------------------------------- */

TEST_F(texture_bc, texture_bc6h_environment, bc_fx) {
    /* usage = "environment" (DENG-100's prefiltered cube): compression = "auto" encodes every face
     * and mip as BC6H; the ENVS trailer follows the DTEX payload. */
    make_cooker(f, NULL);
    de__manifest_kv kv[4] = {kv_str("usage", "environment"), kv_int("env_size", 16),
                             kv_int("env_samples", 16), kv_str("compression", "auto")};
    de_cook_result rb, rn;
    de_cooked_env_view eb, en;
    memset(&eb, 0, sizeof eb);
    memset(&en, 0, sizeof en);
    const char *why = NULL;
    ASSERT_EQ(cook(f, "bcd:/", "env.exr", kv, 4, &rb), DE_OK);
    ASSERT_TRUE(de_cooked_env_parse(rb.data, rb.size, &eb, &why));
    EXPECT_EQ(eb.texture.format, (u32)DE_FMT_BC6H_UFLOAT);
    EXPECT_EQ(eb.texture.dim, (u32)DE_TEX_CUBE);
    EXPECT_EQ(eb.texture.depth_or_layers, 6u);
    EXPECT_EQ(eb.face_size, 16u);
    EXPECT_EQ(eb.texture.usage, (u32)DE_COOKED_TEXTURE_USAGE_ENVIRONMENT);
    kv[3] = kv_str("compression", "none");
    ASSERT_EQ(cook(f, "bcd:/", "env.exr", kv, 4, &rn), DE_OK);
    ASSERT_TRUE(de_cooked_env_parse(rn.data, rn.size, &en, &why));
    EXPECT_EQ(en.texture.format, (u32)DE_FMT_RGBA16_FLOAT);
    EXPECT_EQ(eb.texture.mip_count, en.texture.mip_count);
    /* The SH9 trailer does not depend on the cube's format. */
    EXPECT_MEMEQ(eb.sh9, en.sh9, sizeof eb.sh9);
    /* Every face's mip 0 decodes close to the RGBA16F cube. */
    static decoded got, ref;
    for (u32 face = 0; face < 6u; ++face) {
        ASSERT_TRUE(decode_sub(&eb.texture, face, 0, &got));
        ASSERT_TRUE(decode_sub(&en.texture, face, 0, &ref));
        f64 p = psnr_log(&ref, &got);
        if (p < 38.0)
            printf("  face %u: log-space PSNR %.2f dB\n", face, p);
        EXPECT_TRUE(p >= 38.0);
    }
    de_cook_result_free(&rb);
    de_cook_result_free(&rn);
}

/* ---- import.texture_bc.texture_bc_auto_rules_table ------------------------------------------- */

/* What `auto` must give, written out per combination (the test's own table, not the
 * importer's). */
static de_gpu_format expected_auto(de_cooked_texture_usage usage, bool srgb,
                                   de_tex_source_class src, bool pixel_art) {
    bool hdr = src == DE_TEX_SOURCE_CLASS_HDR;
    if (pixel_art) {
        if (hdr || usage == DE_COOKED_TEXTURE_USAGE_ENVIRONMENT)
            return DE_FMT_RGBA16_FLOAT;
        return srgb ? DE_FMT_RGBA8_SRGB : DE_FMT_RGBA8_UNORM;
    }
    if (hdr || usage == DE_COOKED_TEXTURE_USAGE_ENVIRONMENT)
        return DE_FMT_BC6H_UFLOAT;
    switch (usage) {
    case DE_COOKED_TEXTURE_USAGE_NORMAL:
        return DE_FMT_BC5_UNORM;
    case DE_COOKED_TEXTURE_USAGE_MASK:
        if (src == DE_TEX_SOURCE_CLASS_LDR_SINGLE)
            return DE_FMT_BC4_UNORM;
        return srgb ? DE_FMT_BC7_SRGB : DE_FMT_BC7_UNORM;
    default:
        return srgb ? DE_FMT_BC7_SRGB : DE_FMT_BC7_UNORM;
    }
}

TEST(texture_bc, texture_bc_auto_rules_table) {
    /* Every (usage, srgb, source class, pixel_art) combination, with and without alpha. */
    u32 checked = 0;
    for (u32 u = 0; u < DE_COOKED_TEXTURE_USAGE_COUNT; ++u)
        for (u32 s = 0; s < 2u; ++s)
            for (u32 k = 0; k < DE_TEX_SOURCE_CLASS_COUNT; ++k)
                for (u32 p = 0; p < 2u; ++p)
                    for (u32 a = 0; a < 2u; ++a) {
                        de_tex_format_query q;
                        memset(&q, 0, sizeof q);
                        q.compression = DE_TEX_COMPRESSION_AUTO;
                        q.usage = (de_cooked_texture_usage)u;
                        q.srgb = s != 0;
                        q.source = (de_tex_source_class)k;
                        q.pixel_art = p != 0;
                        q.has_alpha = a != 0;
                        q.block_aligned = true;
                        de_tex_format_choice c = de_tex_bc_choose(&q);
                        de_gpu_format want = expected_auto(q.usage, q.srgb, q.source, q.pixel_art);
                        if (c.format != want)
                            printf("  usage %u srgb %u source %u pixel_art %u: got %u, want %u\n",
                                   u, s, k, p, (u32)c.format, (u32)want);
                        EXPECT_EQ((u32)c.format, (u32)want);
                        EXPECT_TRUE(c.compression != DE_TEX_COMPRESSION_AUTO);
                        ++checked;
                    }
    EXPECT_EQ(checked, 4u * 2u * 3u * 2u * 2u);

    /* The spec's examples (SPEC §11.2: bc7 color, bc5 normals, bc4 masks, bc6h hdr). */
    de_tex_format_query q;
    memset(&q, 0, sizeof q);
    q.block_aligned = true;
    q.srgb = true;
    EXPECT_EQ((u32)de_tex_bc_choose(&q).format, (u32)DE_FMT_BC7_SRGB);
    q.srgb = false;
    q.usage = DE_COOKED_TEXTURE_USAGE_NORMAL;
    EXPECT_EQ((u32)de_tex_bc_choose(&q).format, (u32)DE_FMT_BC5_UNORM);
    q.usage = DE_COOKED_TEXTURE_USAGE_MASK;
    q.source = DE_TEX_SOURCE_CLASS_LDR_SINGLE;
    EXPECT_EQ((u32)de_tex_bc_choose(&q).format, (u32)DE_FMT_BC4_UNORM);
    q.usage = DE_COOKED_TEXTURE_USAGE_COLOR;
    q.source = DE_TEX_SOURCE_CLASS_HDR;
    EXPECT_EQ((u32)de_tex_bc_choose(&q).format, (u32)DE_FMT_BC6H_UFLOAT);

    /* Explicit values and their notes. */
    static const struct {
        de_tex_compression comp;
        bool srgb;
        de_tex_source_class src;
        bool alpha;
        de_gpu_format format;
        u32 notes;
    } k[] = {
        {DE_TEX_COMPRESSION_NONE, true, DE_TEX_SOURCE_CLASS_LDR, true, DE_FMT_RGBA8_SRGB, 0},
        {DE_TEX_COMPRESSION_NONE, false, DE_TEX_SOURCE_CLASS_HDR, true, DE_FMT_RGBA16_FLOAT, 0},
        {DE_TEX_COMPRESSION_BC1, true, DE_TEX_SOURCE_CLASS_LDR, false, DE_FMT_BC1_SRGB, 0},
        {DE_TEX_COMPRESSION_BC1, true, DE_TEX_SOURCE_CLASS_LDR, true, DE_FMT_BC1_SRGB,
         DE_TEX_NOTE_DROPS_ALPHA},
        {DE_TEX_COMPRESSION_BC1, false, DE_TEX_SOURCE_CLASS_LDR, true, DE_FMT_BC7_UNORM,
         DE_TEX_NOTE_LINEAR_BC1_BC3},
        {DE_TEX_COMPRESSION_BC3, true, DE_TEX_SOURCE_CLASS_LDR, true, DE_FMT_BC3_SRGB, 0},
        {DE_TEX_COMPRESSION_BC3, false, DE_TEX_SOURCE_CLASS_LDR, true, DE_FMT_BC7_UNORM,
         DE_TEX_NOTE_LINEAR_BC1_BC3},
        {DE_TEX_COMPRESSION_BC4, false, DE_TEX_SOURCE_CLASS_LDR_SINGLE, false, DE_FMT_BC4_UNORM, 0},
        {DE_TEX_COMPRESSION_BC4, false, DE_TEX_SOURCE_CLASS_LDR, true, DE_FMT_BC4_UNORM,
         DE_TEX_NOTE_DROPS_ALPHA | DE_TEX_NOTE_DROPS_CHANNELS},
        {DE_TEX_COMPRESSION_BC4, true, DE_TEX_SOURCE_CLASS_LDR_SINGLE, false, DE_FMT_BC4_UNORM,
         DE_TEX_NOTE_SRGB_IGNORED},
        {DE_TEX_COMPRESSION_BC5, false, DE_TEX_SOURCE_CLASS_LDR, false, DE_FMT_BC5_UNORM,
         DE_TEX_NOTE_DROPS_CHANNELS},
        {DE_TEX_COMPRESSION_BC6H, false, DE_TEX_SOURCE_CLASS_HDR, true, DE_FMT_BC6H_UFLOAT,
         DE_TEX_NOTE_DROPS_ALPHA},
        {DE_TEX_COMPRESSION_BC6H, false, DE_TEX_SOURCE_CLASS_LDR, false, DE_FMT_BC6H_UFLOAT, 0},
        {DE_TEX_COMPRESSION_BC7, true, DE_TEX_SOURCE_CLASS_LDR, true, DE_FMT_BC7_SRGB, 0},
        {DE_TEX_COMPRESSION_BC7, false, DE_TEX_SOURCE_CLASS_HDR, false, DE_FMT_BC7_UNORM,
         DE_TEX_NOTE_CLAMPS_HDR},
    };
    for (u32 i = 0; i < BC_COUNT(k); ++i) {
        memset(&q, 0, sizeof q);
        q.compression = k[i].comp;
        q.srgb = k[i].srgb;
        q.source = k[i].src;
        q.has_alpha = k[i].alpha;
        q.block_aligned = true;
        de_tex_format_choice c = de_tex_bc_choose(&q);
        if (c.format != k[i].format || c.notes != k[i].notes)
            printf("  row %u: format %u notes 0x%x\n", i, (u32)c.format, c.notes);
        EXPECT_EQ((u32)c.format, (u32)k[i].format);
        EXPECT_EQ(c.notes, k[i].notes);
    }
    /* A mip 0 that is not a multiple of 4: uncompressed, whatever was asked. */
    memset(&q, 0, sizeof q);
    q.compression = DE_TEX_COMPRESSION_BC7;
    q.srgb = true;
    de_tex_format_choice c = de_tex_bc_choose(&q);
    EXPECT_EQ((u32)c.format, (u32)DE_FMT_RGBA8_SRGB);
    EXPECT_EQ(c.notes, (u32)DE_TEX_NOTE_UNALIGNED);

    /* Setting values. */
    de_tex_compression comp;
    static const char *const names[] = {"auto", "none", "bc1", "bc3", "bc4", "bc5", "bc6h", "bc7"};
    for (u32 i = 0; i < BC_COUNT(names); ++i) {
        EXPECT_TRUE(de_tex_compression_parse(names[i], &comp));
        EXPECT_STREQ(de_tex_compression_name(comp), names[i]);
    }
    EXPECT_FALSE(de_tex_compression_parse("bc2", &comp));
    EXPECT_FALSE(de_tex_compression_parse("BC7", &comp));
    de_tex_quality ql;
    EXPECT_TRUE(de_tex_quality_parse("fast", &ql) && ql == DE_TEX_QUALITY_FAST);
    EXPECT_TRUE(de_tex_quality_parse("high", &ql) && ql == DE_TEX_QUALITY_HIGH);
    EXPECT_FALSE(de_tex_quality_parse("ultra", &ql));
}

/* ---- import.texture_bc.texture_bc_alpha_warning ---------------------------------------------- */

TEST_F(texture_bc, texture_bc_alpha_warning, bc_fx) {
    /* An explicit choice that drops the source's alpha logs a warning; one that keeps it does
     * not. */
    make_cooker(f, NULL);
    de_cook_result r;
    de__manifest_kv bc1 = kv_str("compression", "bc1");
    ASSERT_EQ(cook(f, "bcd:/", "cutout.png", &bc1, 1, &r), DE_OK);
    de_cook_result_free(&r);
    de_log_flush();
    EXPECT_GE(g_warn.alpha_dropped, 1u);
    u32 before = g_warn.alpha_dropped;
    de__manifest_kv bc7 = kv_str("compression", "bc7");
    ASSERT_EQ(cook(f, "bcd:/", "cutout.png", &bc7, 1, &r), DE_OK);
    de_cook_result_free(&r);
    de_log_flush();
    EXPECT_EQ(g_warn.alpha_dropped, before);
}

/* ---- import.texture_bc.texture_bc_small_mip_padding ------------------------------------------ */

TEST_F(texture_bc, texture_bc_small_mip_padding, bc_fx) {
    make_cooker(f, NULL);
    /* 12x8 RGBA, full chain: 12x8, 6x4, 3x2, 1x1. Mips 1 to 3 end inside a block. */
    de__manifest_kv srgb = kv_bool("srgb", false);
    de_cook_result rb, rn;
    de_cooked_texture_view vb, vn;
    memset(&vb, 0, sizeof vb);
    memset(&vn, 0, sizeof vn);
    ASSERT_EQ(cook(f, "bcd:/", "odd.png", &srgb, 1, &rb), DE_OK);
    ASSERT_TRUE(view_of(&rb, &vb));
    EXPECT_EQ(vb.format, (u32)DE_FMT_BC7_UNORM);
    EXPECT_EQ(vb.mip_count, 4u);
    de__manifest_kv none[2] = {srgb, kv_str("compression", "none")};
    ASSERT_EQ(cook(f, "bcd:/", "odd.png", none, 2, &rn), DE_OK);
    ASSERT_TRUE(view_of(&rn, &vn));
    static decoded got, ref;
    for (u32 m = 0; m < vb.mip_count; ++m) {
        de_cooked_texture_subresource s = sub_of(&vb, 0, m);
        /* Block rows and pitch: ceil(w / 4) blocks of 16 bytes, at least one block, pitch a
         * multiple of 256, every offset a multiple of 512. */
        EXPECT_EQ(s.row_count, (s.height + 3u) / 4u);
        EXPECT_EQ(s.row_pitch % 256u, 0u);
        EXPECT_EQ(s.offset % 512u, 0u);
        EXPECT_GE(s.row_pitch, ((s.width + 3u) / 4u) * 16u);
        /* The visible texels decode close to the uncompressed mip. */
        ASSERT_TRUE(decode_mip(&vb, m, &got));
        ASSERT_TRUE(decode_mip(&vn, m, &ref));
        f64 p = psnr8(&got, &ref, 0, 4);
        printf("  mip %u (%ux%u): PSNR %.2f dB\n", m, s.width, s.height, p);
        EXPECT_TRUE(p >= 36.0);
        /* Bytes past the blocks of each row stay zero. */
        u64 row = (u64)((s.width + 3u) / 4u) * 16u;
        bool zero = true;
        for (u32 y = 0; y < s.row_count; ++y)
            for (u64 b = row; b < s.row_pitch; ++b)
                zero = zero && vb.blob[s.offset + (u64)y * s.row_pitch + b] == 0;
        EXPECT_TRUE(zero);
    }
    /* The 1x1 mip's block is one colour repeated (edge replication), so all 16 decoded texels of
     * the block are the same. */
    de_cooked_texture_subresource s3 = sub_of(&vb, 0, 3);
    u8 px[64];
    bcdec_bc7(vb.blob + s3.offset, px, 16);
    bool same = true;
    for (u32 i = 1; i < 16u; ++i)
        same = same && memcmp(px, px + i * 4u, 4) == 0;
    EXPECT_TRUE(same);
    /* The 3x2 mip: column 3 repeats column 2, rows 2 and 3 repeat row 1 (before compression; a
     * flat-enough BC7 block reproduces it within a few steps). */
    de_cooked_texture_subresource s2 = sub_of(&vb, 0, 2);
    bcdec_bc7(vb.blob + s2.offset, px, 16);
    int worst = 0;
    for (u32 y = 0; y < 4u; ++y)
        for (u32 x = 0; x < 4u; ++x) {
            u32 sx = x < 3u ? x : 2u, sy = y < 2u ? y : 1u;
            for (u32 c = 0; c < 4u; ++c) {
                int d = (int)px[(y * 4u + x) * 4u + c] - (int)px[(sy * 4u + sx) * 4u + c];
                d = d < 0 ? -d : d;
                worst = d > worst ? d : worst;
            }
        }
    EXPECT_LE(worst, 8);
    de_cook_result_free(&rb);
    de_cook_result_free(&rn);
}

/* ---- import.texture_bc.texture_bc_deterministic_threads -------------------------------------- */

/* A 128x64 f32 RGBA test image (512 blocks: two encode ranges) with detail at every scale. */
static void make_image(f32 *rgba, u32 w, u32 h, bool hdr) {
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            f32 *p = rgba + ((u64)y * w + x) * 4u;
            u32 hsh = (x * 73856093u) ^ (y * 19349663u);
            hsh = (hsh ^ (hsh >> 13)) * 1274126177u;
            f32 n = (f32)(hsh & 0xFFFFu) / 65535.0f;
            p[0] = 0.5f + 0.4f * sinf((f32)x * 0.21f) * cosf((f32)y * 0.13f) + 0.08f * n;
            p[1] = (f32)y / (f32)h * 0.8f + 0.1f * n;
            p[2] = (f32)((x / 8u + y / 8u) % 2u) * 0.6f + 0.2f * n;
            p[3] = (x + y) % 7u == 0 ? 0.3f : 1.0f;
            if (hdr)
                for (u32 c = 0; c < 3u; ++c)
                    p[c] = p[c] * p[c] * 40.0f * (1.0f + (f32)x / 16.0f);
        }
}

TEST_F(texture_bc, texture_bc_deterministic_threads, bc_fx) {
    enum { W = 128, H = 64 };
    static f32 ldr[W * H * 4], hdr[W * H * 4];
    make_image(ldr, W, H, false);
    make_image(hdr, W, H, true);
    static const struct {
        de_gpu_format format;
        f32 rdo;
    } k[] = {
        {DE_FMT_BC7_SRGB, 0.0f},    {DE_FMT_BC7_UNORM, 2.0f}, {DE_FMT_BC1_SRGB, 1.0f},
        {DE_FMT_BC3_SRGB, 0.0f},    {DE_FMT_BC4_UNORM, 1.0f}, {DE_FMT_BC5_UNORM, 0.0f},
        {DE_FMT_BC6H_UFLOAT, 0.0f},
    };
    enum { BC_COUNT_K = 7 };
    _Static_assert(sizeof k / sizeof k[0] == BC_COUNT_K, "one reference per format");
    const u32 pitch = 512; /* 32 blocks of 16 bytes */
    const u32 rows = H / 4;
    static u8 want[BC_COUNT_K][512 * 16], got[512 * 16];
    static const u32 workers[] = {1, 4, 16};
    de_fp_env_init_thread();
    de_tex_encode_opts o[BC_COUNT_K];
    /* Reference: one thread, no task system. */
    for (u32 i = 0; i < BC_COUNT(k); ++i) {
        memset(&o[i], 0, sizeof o[i]);
        o[i].quality = DE_TEX_QUALITY_FAST; /* the same code paths; keeps the test short */
        o[i].rdo_quality = k[i].rdo;
        const f32 *src = k[i].format == DE_FMT_BC6H_UFLOAT ? hdr : ldr;
        memset(want[i], 0, sizeof want[i]);
        ASSERT_EQ(de_tex_bc_encode(&f->alloc, k[i].format, src, W, H, want[i], pitch, &o[i]),
                  DE_OK);
    }
    for (u32 w = 0; w < BC_COUNT(workers); ++w) {
        de_task_system *ts = NULL;
        ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = workers[w],
                                                               .background_worker_count = 1,
                                                               .allocator = &f->alloc},
                                        &ts),
                  DE_OK);
        for (u32 i = 0; i < BC_COUNT(k); ++i) {
            const f32 *src = k[i].format == DE_FMT_BC6H_UFLOAT ? hdr : ldr;
            o[i].tasks = ts;
            memset(got, 0, sizeof got);
            de_result r = de_tex_bc_encode(&f->alloc, k[i].format, src, W, H, got, pitch, &o[i]);
            o[i].tasks = NULL;
            EXPECT_EQ(r, DE_OK);
            if (memcmp(want[i], got, (size_t)pitch * rows) != 0)
                printf("  format %u differs with %u workers\n", (u32)k[i].format, workers[w]);
            EXPECT_MEMEQ(want[i], got, (size_t)pitch * rows);
        }
        de_task_system_destroy(ts);
    }

    /* Through the importer: cooking with the cooker's task system (and verify_determinism, which
     * cooks twice) gives the bytes of a cook without one. */
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 4,
                                                           .background_worker_count = 1,
                                                           .allocator = &f->alloc},
                                    &ts),
              DE_OK);
    make_cooker(f, ts);
    de_cook_result a, b;
    de__manifest_kv kv[2] = {kv_str("quality", "fast"), kv_float("rdo_quality", 0.5)};
    ASSERT_EQ(cook(f, "bcd:/", "photo.png", kv, 2, &a), DE_OK);
    de_cooker_destroy(f->c);
    f->c = NULL;
    make_cooker(f, NULL);
    ASSERT_EQ(cook(f, "bcd:/", "photo.png", kv, 2, &b), DE_OK);
    ASSERT_EQ(a.size, b.size);
    EXPECT_MEMEQ(a.data, b.data, (size_t)a.size);
    de_cook_result_free(&a);
    de_cook_result_free(&b);
    de_cooker_destroy(f->c);
    f->c = NULL;
    de_task_system_destroy(ts);
}

/* ---- import.texture_bc.texture_bc_ktx2_uastc_to_bc7 ------------------------------------------ */

TEST_F(texture_bc, texture_bc_ktx2_uastc_to_bc7, bc_fx) {
    make_cooker(f, NULL);
    static decoded got, ref;
    /* UASTC (from rgba8.png, alpha in the top-left 4x4): transcoded at cook time, filtered, and
     * encoded as BC7 sRGB (the file's transfer function is sRGB). */
    ASSERT_TRUE(cook_pair(f, "bct:/", "uastc.ktx2", NULL, 0, DE_FMT_BC7_SRGB, &got, &ref));
    f64 p = psnr8(&got, &ref, 0, 4);
    printf("  UASTC -> BC7 PSNR (RGBA): %.2f dB\n", p);
    /* rgba8.png is a hard case for any 4x4 block format: red runs along x and green along y, so
     * a block's colours fill a plane, not a line, and alpha jumps from 0 to 255. */
    EXPECT_TRUE(p >= 33.0);
    /* ETC1S: BC7 by default, BC1 when asked. */
    ASSERT_TRUE(cook_pair(f, "bct:/", "etc1s.ktx2", NULL, 0, DE_FMT_BC7_SRGB, &got, &ref));
    EXPECT_TRUE(psnr8(&got, &ref, 0, 3) >= 36.0);
    de__manifest_kv bc1 = kv_str("compression", "bc1");
    ASSERT_TRUE(cook_pair(f, "bct:/", "etc1s.ktx2", &bc1, 1, DE_FMT_BC1_SRGB, &got, &ref));
    f64 p1 = psnr8(&got, &ref, 0, 3);
    printf("  ETC1S -> BC1 PSNR (RGB): %.2f dB\n", p1);
    EXPECT_TRUE(p1 >= 30.0);
    /* KTX2 files that already hold BC blocks are passed through, whatever `compression` says. */
    de_cook_result r;
    de_cooked_texture_view v;
    memset(&v, 0, sizeof v);
    ASSERT_EQ(cook(f, "bct:/", "bc7.ktx2", &bc1, 1, &r), DE_OK);
    ASSERT_TRUE(view_of(&r, &v));
    EXPECT_EQ(v.format, (u32)DE_FMT_BC7_SRGB);
    de_cook_result_free(&r);
}

/* ---- import.texture_bc.texture_bc_settings -----------------------------------------------------
 */

TEST_F(texture_bc, texture_bc_settings, bc_fx) {
    /* compression, quality and rdo_quality are in the settings hash. */
    de__manifest_kv base[3] = {kv_str("compression", "auto"), kv_str("quality", "normal"),
                               kv_float("rdo_quality", 0.0)};
    u64 h0 = de_cook_settings_hash(base, 3);
    de__manifest_kv c1[3] = {kv_str("compression", "bc7"), base[1], base[2]};
    de__manifest_kv c2[3] = {base[0], kv_str("quality", "high"), base[2]};
    de__manifest_kv c3[3] = {base[0], base[1], kv_float("rdo_quality", 1.0)};
    u64 h1 = de_cook_settings_hash(c1, 3), h2 = de_cook_settings_hash(c2, 3),
        h3 = de_cook_settings_hash(c3, 3);
    EXPECT_TRUE(h0 != h1 && h0 != h2 && h0 != h3 && h1 != h2 && h1 != h3 && h2 != h3);

    /* The importer reads them: a different quality or rdo_quality changes the blocks. */
    make_cooker(f, NULL);
    de_cook_result r0, r1, r2;
    ASSERT_EQ(cook(f, "bcd:/", "photo.png", base, 3, &r0), DE_OK);
    ASSERT_EQ(cook(f, "bcd:/", "photo.png", c2, 3, &r1), DE_OK);
    ASSERT_EQ(cook(f, "bcd:/", "photo.png", c3, 3, &r2), DE_OK);
    ASSERT_EQ(r0.size, r1.size);
    ASSERT_EQ(r0.size, r2.size);
    EXPECT_TRUE(memcmp(r0.data, r1.data, (size_t)r0.size) != 0);
    EXPECT_TRUE(memcmp(r0.data, r2.data, (size_t)r0.size) != 0);
    de_cook_result_free(&r0);
    de_cook_result_free(&r1);
    de_cook_result_free(&r2);

    /* Bad values are refused with a message. */
    static const struct {
        const char *key;
        const char *value;
        f64 number;
    } bad[] = {{"compression", "bc2", 0},
               {"compression", "BC7", 0},
               {"quality", "ultra", 0},
               {"rdo_quality", NULL, -1.0},
               {"rdo_quality", NULL, 1000.0}};
    for (u32 i = 0; i < BC_COUNT(bad); ++i) {
        de__manifest_kv kv = bad[i].value != NULL ? kv_str(bad[i].key, bad[i].value)
                                                  : kv_float(bad[i].key, bad[i].number);
        de_cook_result r;
        EXPECT_EQ(cook(f, "bcd:/", "photo.png", &kv, 1, &r), DE_ERR_INVALID_ARGUMENT);
        const de_error *e = de_error_last();
        EXPECT_TRUE(e != NULL && strstr(e->message, bad[i].key) != NULL);
    }
}
