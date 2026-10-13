/*
 * test_env_prefilter.c: HDR environments of the texture importer (DENG-100, SPEC §11.6, §12.7):
 * the cube face table against the GPU's face selection rule, equirectangular / cross / KTX2 cube
 * sources, the GGX prefilter of a constant environment, mip sizes and the roughness → mip
 * constant, SH9 irradiance (constant and directional), rotation, byte-identical output across
 * worker counts (and a golden hash for other hosts), and the cooked DTEX + ENVS blob with its
 * parser. CTest names import.env.<test>.
 *
 * Irradiance convention under test: E(n) = integral of L(w) max(0, n.w) dw, so a constant
 * radiance L gives E = pi L (render/formats/de_cooked_env.h).
 *
 * Faces are 4 to 32 texels with at most 64 samples, so every test stays well under a second,
 * also under the sanitizers. EXR sources are written here (uncompressed float) to the build
 * tree (etmp:/env/); the KTX2 cube and the square EXR come from data/texture/ (envd:/).
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "asset/manifest_internal.h"
#include "cook/importers/texture/env_internal.h"
#include "cook/importers/texture/texture_import.h"
#include "cook/importers/texture/texture_internal.h"
#include "cook/lib/de_cooker.h"
#include "render/formats/de_cooked_env.h"
#include "render/formats/de_cooked_le.h"
#include "render/formats/de_cooked_texture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENVT_PI 3.14159265358979323846

/* ---- Fixture --------------------------------------------------------------------------------- */

typedef struct env_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_task_system *tasks;
    de_cooker *c;
} env_fx;

static void env_mount(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("envd", DE_IMPORT_TEST_DATA_DIR "/texture", 0, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("etmp", DE_IMPORT_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    if (!de_path_exists("etmp:/env"))
        ASSERT_EQ(de_dir_create("etmp:/env"), DE_OK);
    mounted = true;
}

static void env_fx_setup(env_fx *f) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    env_mount();
    de_fp_env_init_thread(); /* the FP mode of the cook threads and task workers */
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
}

static void env_fx_teardown(env_fx *f) {
    if (f->c != NULL)
        de_cooker_destroy(f->c);
    if (f->tasks != NULL)
        de_task_system_destroy(f->tasks);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    de_log_shutdown();
}

static void start_tasks(env_fx *f, u32 workers) {
    if (f->tasks != NULL)
        de_task_system_destroy(f->tasks);
    f->tasks = NULL;
    ASSERT_EQ(de_task_system_create(
                  &(de_task_system_desc){.worker_count = workers, .background_worker_count = 1},
                  &f->tasks),
              DE_OK);
}

static void stop_tasks(env_fx *f) {
    if (f->tasks != NULL)
        de_task_system_destroy(f->tasks);
    f->tasks = NULL;
}

/* ---- Images ---------------------------------------------------------------------------------- */

static de_tex_image make_image(env_fx *f, u32 w, u32 h, u32 layers, bool cube) {
    de_tex_image img;
    memset(&img, 0, sizeof img);
    img.width = w;
    img.height = h;
    img.layers = layers;
    img.cube = cube;
    img.hdr = true;
    img.channels = 3;
    ASSERT_EQ(de_tex_image_alloc(&f->alloc, &img), DE_OK);
    for (u64 i = 0; i < (u64)w * h * layers; ++i)
        img.rgba[i * 4u + 3u] = 1.0f;
    return img;
}

static void put_px(de_tex_image *img, u32 layer, u32 x, u32 y, f32 r, f32 g, f32 b) {
    f32 *p = img->rgba + (((u64)layer * img->height + y) * img->width + x) * 4u;
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = 1.0f;
}

static void fill(de_tex_image *img, f32 r, f32 g, f32 b) {
    for (u32 l = 0; l < img->layers; ++l)
        for (u32 y = 0; y < img->height; ++y)
            for (u32 x = 0; x < img->width; ++x)
                put_px(img, l, x, y, r, g, b);
}

/* The engine direction of equirect texel (x, y): the importer's convention (env_internal.h). */
static void equirect_dir(u32 w, u32 h, f64 x, f64 y, f64 d[3]) {
    f64 phi = 2.0 * ENVT_PI * ((x + 0.5) / (f64)w - 0.5);
    f64 theta = ENVT_PI * (0.5 - (y + 0.5) / (f64)h);
    d[0] = sin(phi) * cos(theta);
    d[1] = cos(phi) * cos(theta);
    d[2] = sin(theta);
}

/* The equirect texel nearest engine direction d. */
static void equirect_texel(u32 w, u32 h, const f64 d[3], u32 *x, u32 *y) {
    f64 phi = atan2(d[0], d[1]);
    f64 theta = atan2(d[2], sqrt(d[0] * d[0] + d[1] * d[1]));
    f64 u = 0.5 + phi / (2.0 * ENVT_PI), v = 0.5 - theta / ENVT_PI;
    f64 fx = floor(u * w), fy = floor(v * h);
    *x = (u32)(fx < 0 ? 0 : (fx >= w ? w - 1 : fx));
    *y = (u32)(fy < 0 ? 0 : (fy >= h ? h - 1 : fy));
}

/* A bright 2x2 patch (value v) centred on engine direction d of a black equirect image. */
static void spot(de_tex_image *img, const f64 d[3], f32 v) {
    u32 x, y;
    equirect_texel(img->width, img->height, d, &x, &y);
    for (u32 dy = 0; dy < 2u; ++dy)
        for (u32 dx = 0; dx < 2u; ++dx) {
            u32 px = (x + img->width - 1u + dx) % img->width;
            u32 py = y + dy >= 1u ? y + dy - 1u : 0u;
            py = py < img->height ? py : img->height - 1u;
            put_px(img, 0, px, py, v, v, v);
        }
}

static const f32 *cube_texel(const de_tex_chain *c, u32 mip, u32 face, u32 x, u32 y) {
    u32 n = c->width[mip];
    return c->rgba[mip] + (((u64)face * n + y) * n + x) * 4u;
}

static de_env_settings settings(u32 size, u32 samples) {
    de_env_settings s;
    memset(&s, 0, sizeof s);
    s.size = size;
    s.samples = samples;
    return s;
}

/* The direction (unit) where the irradiance of `sh` peaks, over a Fibonacci sphere of 4096
 * points plus the six axes; its largest red irradiance in *peak. */
static void irradiance_peak(const de_env_sh9 *sh, f64 dir[3], f64 *peak) {
    de_cooked_env_view v;
    memset(&v, 0, sizeof v);
    memcpy(v.sh9, sh->irradiance, sizeof v.sh9);
    *peak = -1e30;
    dir[0] = dir[1] = dir[2] = 0.0;
    const u32 n = 4096;
    for (u32 i = 0; i < n + 6u; ++i) {
        f64 d[3];
        if (i < n) {
            f64 z = 1.0 - (2.0 * i + 1.0) / n, r = sqrt(1.0 - z * z);
            f64 a = ENVT_PI * (3.0 - sqrt(5.0)) * i;
            d[0] = r * cos(a);
            d[1] = r * sin(a);
            d[2] = z;
        } else {
            u32 ax = (i - n) / 2u;
            d[0] = d[1] = d[2] = 0.0;
            d[ax] = (i - n) % 2u == 0 ? 1.0 : -1.0;
        }
        f32 e[3];
        de_cooked_env_irradiance(&v, (f32)d[0], (f32)d[1], (f32)d[2], e);
        if (e[0] > *peak) {
            *peak = e[0];
            dir[0] = d[0];
            dir[1] = d[1];
            dir[2] = d[2];
        }
    }
}

/* ---- Cooking --------------------------------------------------------------------------------- */

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

static de__manifest_kv kv_str(const char *k, const char *v) {
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = k;
    kv.value.type = DE__MANIFEST_STRING;
    kv.value.u.s = v;
    return kv;
}

static void make_cooker(env_fx *f, de_task_system *ts) {
    if (f->c != NULL)
        de_cooker_destroy(f->c);
    f->c = NULL;
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.allocator = &f->alloc,
                                                 .tasks = ts,
                                                 .verify_determinism = DE_COOK_VERIFY_ON},
                               &f->c),
              DE_OK);
    de_asset_type_desc t = de_texture_import_type_desc();
    ASSERT_EQ(de_cooker_register_type(f->c, &t), DE_OK);
}

/* Cooks root/source as "env/test" with usage = "environment", compression = "none" and `extra`
 * settings (at most 6). */
static de_result cook_env(env_fx *f, const char *root, const char *source,
                          const de__manifest_kv *extra, u32 n, de_cook_result *out) {
    de__manifest_kv kv[8];
    kv[0] = kv_str("usage", "environment");
    kv[1] = kv_str("compression", "none");
    for (u32 i = 0; i < n && i < 6u; ++i)
        kv[2u + i] = extra[i];
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = "env/test";
    a.type = "texture";
    a.source = source;
    a.file = "etmp:/env/assets.toml";
    a.settings = kv;
    a.setting_count = 2u + (n < 6u ? n : 6u);
    de_error_clear();
    return de_cooker_cook(f->c, &(de_cook_input){.asset = &a, .asset_root = root}, out);
}

static bool error_says(const char *needle) {
    const de_error *e = de_error_last();
    if (e == NULL || strstr(e->message, needle) == NULL) {
        printf("  error: %s (wanted \"%s\")\n", e != NULL ? e->message : "(none)", needle);
        return false;
    }
    return true;
}

/* Writes an uncompressed scanline OpenEXR file with float B, G, R channels (rows top first). */
static void write_exr(const char *vpath, const de_tex_image *img) {
    const u32 w = img->width, h = img->height;
    u8 hdr[512];
    u32 p = 0;
#define PUT_BYTES(src, n) (memcpy(hdr + p, (src), (n)), p += (u32)(n))
#define PUT_U32(v)        (de_cooked_put_u32(hdr + p, (u32)(v)), p += 4u)
#define PUT_F32(v)        (de_cooked_put_f32(hdr + p, (f32)(v)), p += 4u)
#define PUT_STR(s)        PUT_BYTES((s), strlen(s) + 1u)
    PUT_BYTES("\x76\x2f\x31\x01", 4);
    PUT_U32(2);
    PUT_STR("channels");
    PUT_STR("chlist");
    PUT_U32(3u * 18u + 1u);
    for (u32 c = 0; c < 3u; ++c) {
        PUT_STR(c == 0 ? "B" : (c == 1 ? "G" : "R"));
        PUT_U32(2); /* FLOAT */
        PUT_U32(0); /* pLinear + reserved */
        PUT_U32(1);
        PUT_U32(1);
    }
    hdr[p++] = 0;
    PUT_STR("compression");
    PUT_STR("compression");
    PUT_U32(1);
    hdr[p++] = 0;
    for (u32 k = 0; k < 2u; ++k) {
        PUT_STR(k == 0 ? "dataWindow" : "displayWindow");
        PUT_STR("box2i");
        PUT_U32(16);
        PUT_U32(0);
        PUT_U32(0);
        PUT_U32(w - 1u);
        PUT_U32(h - 1u);
    }
    PUT_STR("lineOrder");
    PUT_STR("lineOrder");
    PUT_U32(1);
    hdr[p++] = 0;
    PUT_STR("pixelAspectRatio");
    PUT_STR("float");
    PUT_U32(4);
    PUT_F32(1.0f);
    PUT_STR("screenWindowCenter");
    PUT_STR("v2f");
    PUT_U32(8);
    PUT_F32(0.0f);
    PUT_F32(0.0f);
    PUT_STR("screenWindowWidth");
    PUT_STR("float");
    PUT_U32(4);
    PUT_F32(1.0f);
    hdr[p++] = 0;
#undef PUT_BYTES
#undef PUT_U32
#undef PUT_F32
#undef PUT_STR
    const u64 line = 8u + 12u * (u64)w;
    const u64 size = p + 8u * (u64)h + line * h;
    u8 *blob = (u8 *)calloc(1, (size_t)size);
    ASSERT_TRUE(blob != NULL);
    memcpy(blob, hdr, p);
    for (u32 y = 0; y < h; ++y) {
        u64 off = p + 8u * (u64)h + line * y;
        de_cooked_put_u64(blob + p + 8u * y, off);
        u8 *b = blob + off;
        de_cooked_put_u32(b, y);
        de_cooked_put_u32(b + 4, 12u * w);
        for (u32 c = 0; c < 3u; ++c)
            for (u32 x = 0; x < w; ++x)
                de_cooked_put_f32(b + 8u + 4u * ((u64)c * w + x),
                                  img->rgba[((u64)y * w + x) * 4u + (2u - c)]);
    }
    de_result r = de_file_write_all_atomic(vpath, blob, size);
    free(blob);
    ASSERT_EQ(r, DE_OK);
}

static bool env_view(const de_cook_result *r, de_cooked_env_view *v) {
    const char *why = NULL;
    if (!de_cooked_env_parse(r->data, r->size, v, &why)) {
        printf("  environment rejected: %s\n", why != NULL ? why : "?");
        return false;
    }
    return true;
}

static f32 half_at(const de_cooked_texture_view *v, u32 face, u32 mip, u32 x, u32 y, u32 c) {
    de_cooked_texture_subresource s;
    memset(&s, 0, sizeof s);
    (void)de_cooked_texture_subresource_at(v, face * v->mip_count + mip, &s);
    const u8 *t = v->blob + s.offset + (u64)y * s.row_pitch + (u64)x * 8u + 2u * c;
    return de_tex_f16_to_f32((u16)(t[0] | (t[1] << 8)));
}

static u64 fnv1a(const void *data, u64 size) {
    const u8 *p = (const u8 *)data;
    u64 h = 0xcbf29ce484222325ull;
    for (u64 i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/* ---- import.env.equirect_to_cube_directions -------------------------------------------------- */

/* The face table of de_cooked_env.h against the GPU face selection rule (Vulkan spec "Cube Map
 * Face Selection", the same in D3D12 and Metal), written out independently here. */
static u32 gpu_face(const f64 d[3], f64 *sc, f64 *tc) {
    f64 ax = fabs(d[0]), ay = fabs(d[1]), az = fabs(d[2]);
    if (ax >= ay && ax >= az) {
        *sc = (d[0] > 0 ? -d[2] : d[2]) / ax; /* +X: -rz, -X: +rz */
        *tc = -d[1] / ax;
        return d[0] > 0 ? 0u : 1u;
    }
    if (ay >= az) {
        *sc = d[0] / ay;                      /* +-Y: +rx */
        *tc = (d[1] > 0 ? d[2] : -d[2]) / ay; /* +Y: +rz, -Y: -rz */
        return d[1] > 0 ? 2u : 3u;
    }
    *sc = (d[2] > 0 ? d[0] : -d[0]) / az; /* +Z: +rx, -Z: -rx */
    *tc = -d[1] / az;
    return d[2] > 0 ? 4u : 5u;
}

TEST_F(env, equirect_to_cube_directions, env_fx) {
    /* 1. The face table is the GPU's, both ways. */
    u32 seed = 12345u;
    for (u32 i = 0; i < 2000u; ++i) {
        f64 d[3];
        for (u32 c = 0; c < 3u; ++c) {
            seed = seed * 1664525u + 1013904223u;
            d[c] = ((f64)(seed >> 8) / 16777216.0) * 2.0 - 1.0;
        }
        if (fabs(d[0]) + fabs(d[1]) + fabs(d[2]) < 1e-3)
            continue;
        f64 sc, tc;
        u32 face = gpu_face(d, &sc, &tc);
        const f32 df[3] = {(f32)d[0], (f32)d[1], (f32)d[2]};
        f32 scf, tcf;
        ASSERT_EQ(de_cooked_env_direction_face(df, &scf, &tcf), face);
        EXPECT_FLOAT_EQ(scf, (f32)sc, 1e-5f);
        EXPECT_FLOAT_EQ(tcf, (f32)tc, 1e-5f);
        f32 back[3];
        de_cooked_env_face_direction(face, (f32)sc, (f32)tc, back);
        f64 m = fmax(fabs(d[0]), fmax(fabs(d[1]), fabs(d[2])));
        for (u32 c = 0; c < 3u; ++c)
            EXPECT_FLOAT_EQ(back[c], (f32)(d[c] / m), 1e-5f);
    }
    /* Face centres look along their axis: +Z is up (the sky), +Y forward. */
    static const f32 k_axis[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                     {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (u32 face = 0; face < 6u; ++face) {
        f32 d[3];
        de_cooked_env_face_direction(face, 0.0f, 0.0f, d);
        for (u32 c = 0; c < 3u; ++c)
            EXPECT_FLOAT_EQ(d[c], k_axis[face][c], 0.0f);
    }

    /* 2. Coloured octants of an equirect image land in the matching octants of the cube:
     *    R = 2 where x > 0 (else 0.5), G likewise for y, B for z. */
    const u32 w = 64, h = 32;
    de_tex_image img = make_image(f, w, h, 1, false);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            f64 d[3];
            equirect_dir(w, h, x, y, d);
            put_px(&img, 0, x, y, d[0] > 0 ? 2.0f : 0.5f, d[1] > 0 ? 2.0f : 0.5f,
                   d[2] > 0 ? 2.0f : 0.5f);
        }
    de_env_settings s = settings(16, 8);
    de_tex_chain chain;
    ASSERT_EQ(de_env_build(&f->alloc, NULL, &img, &s, &chain), DE_OK);
    ASSERT_EQ(chain.layers, 6u);
    u32 checked = 0;
    for (u32 face = 0; face < 6u; ++face)
        for (u32 y = 0; y < 16u; ++y)
            for (u32 x = 0; x < 16u; ++x) {
                f32 d[3];
                de_cooked_env_face_direction(face, (2.0f * x + 1.0f) / 16.0f - 1.0f,
                                             (2.0f * y + 1.0f) / 16.0f - 1.0f, d);
                f32 len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (fabsf(d[0]) < 0.25f * len || fabsf(d[1]) < 0.25f * len ||
                    fabsf(d[2]) < 0.25f * len)
                    continue; /* near an octant boundary: filtered across it */
                const f32 *t = cube_texel(&chain, 0, face, x, y);
                EXPECT_FLOAT_EQ(t[0], d[0] > 0 ? 2.0f : 0.5f, 1e-4f);
                EXPECT_FLOAT_EQ(t[1], d[1] > 0 ? 2.0f : 0.5f, 1e-4f);
                EXPECT_FLOAT_EQ(t[2], d[2] > 0 ? 2.0f : 0.5f, 1e-4f);
                EXPECT_FLOAT_EQ(t[3], 1.0f, 0.0f);
                ++checked;
            }
    EXPECT_GT(checked, 300u);
    de_tex_chain_free(&f->alloc, &chain);
    de_tex_image_free(&f->alloc, &img);
}

/* ---- import.env.cross_and_cube_sources ------------------------------------------------------- */

/* A cross whose face f is (f + 1) / 8 in red, with the -Z cell split: A = 10 in green on its top
 * half, B = 20 on its bottom half. */
static de_tex_image make_cross(env_fx *f, bool horizontal, u32 n) {
    u32 w = horizontal ? 4u * n : 3u * n, h = horizontal ? 3u * n : 4u * n;
    de_tex_image img = make_image(f, w, h, 1, false);
    static const u32 k_h[6][2] = {{2, 1}, {0, 1}, {1, 0}, {1, 2}, {1, 1}, {3, 1}};
    static const u32 k_v[6][2] = {{2, 1}, {0, 1}, {1, 0}, {1, 2}, {1, 1}, {1, 3}};
    for (u32 face = 0; face < 6u; ++face) {
        const u32 *cell = horizontal ? k_h[face] : k_v[face];
        for (u32 y = 0; y < n; ++y)
            for (u32 x = 0; x < n; ++x) {
                f32 g = face == 5u ? (y < n / 2u ? 10.0f : 20.0f) : 0.0f;
                put_px(&img, 0, cell[0] * n + x, cell[1] * n + y, (f32)(face + 1u) / 8.0f, g, 0.0f);
            }
    }
    return img;
}

TEST_F(env, cross_and_cube_sources, env_fx) {
    /* Y-up source face -> engine face: engine e is source (ex, ez, -ey). */
    static const u32 k_engine_from_source[6] = {0, 1, 4, 5, 3, 2};
    for (u32 hv = 0; hv < 2u; ++hv) {
        const bool horizontal = hv == 0;
        de_tex_image img = make_cross(f, horizontal, 8);
        de_env_layout layout;
        ASSERT_EQ(de_env_source_layout(&img, &layout), DE_OK);
        EXPECT_EQ(layout, horizontal ? DE_ENV_LAYOUT_CROSS_H : DE_ENV_LAYOUT_CROSS_V);
        de_env_settings s = settings(8, 4);
        de_tex_chain chain;
        ASSERT_EQ(de_env_build(&f->alloc, NULL, &img, &s, &chain), DE_OK);
        for (u32 src = 0; src < 6u; ++src) {
            const u32 face = k_engine_from_source[src];
            /* Texels away from the edges hold the source face's red. */
            for (u32 y = 2; y < 6u; ++y)
                for (u32 x = 2; x < 6u; ++x)
                    EXPECT_FLOAT_EQ(cube_texel(&chain, 0, face, x, y)[0], (f32)(src + 1u) / 8.0f,
                                    1e-5f);
        }
        /* Engine +Y (forward) is source -Z. Its bottom rows look up (z = tc > 0), which is the
         * top half of source face -Z: cell top (A) in a horizontal cross, cell bottom (B) in a
         * vertical one (the cell is turned 180 degrees). */
        const f32 up = horizontal ? 10.0f : 20.0f, down = horizontal ? 20.0f : 10.0f;
        EXPECT_FLOAT_EQ(cube_texel(&chain, 0, 2, 3, 7)[1], up, 1e-4f);
        EXPECT_FLOAT_EQ(cube_texel(&chain, 0, 2, 3, 0)[1], down, 1e-4f);
        de_tex_chain_free(&f->alloc, &chain);
        de_tex_image_free(&f->alloc, &img);
    }

    /* A KTX2 cube through the importer: face f of cube_rgba8.ktx2 is f * 40 / 255 (UNORM, so
     * linear for an environment). Engine +Z (sky) holds source face 2 (+Y). */
    make_cooker(f, NULL);
    de__manifest_kv kv[2] = {kv_int("env_size", 4), kv_int("env_samples", 16)};
    de_cook_result r;
    ASSERT_EQ(cook_env(f, "envd:/", "cube_rgba8.ktx2", kv, 2, &r), DE_OK);
    de_cooked_env_view v;
    ASSERT_TRUE(env_view(&r, &v));
    EXPECT_EQ(v.texture.format, (u32)DE_FMT_RGBA16_FLOAT);
    for (u32 src = 0; src < 6u; ++src)
        EXPECT_FLOAT_EQ(half_at(&v.texture, k_engine_from_source[src], 0, 1, 1, 0),
                        (f32)(src * 40u) / 255.0f, 1e-3f);
    de_cook_result_free(&r);
}

/* ---- import.env.prefilter_constant ----------------------------------------------------------- */

TEST_F(env, prefilter_constant, env_fx) {
    const f32 c[3] = {0.5f, 0.25f, 1.0f};
    de_tex_image img = make_image(f, 32, 16, 1, false);
    fill(&img, c[0], c[1], c[2]);
    de_env_settings s = settings(16, 64);
    de_tex_chain chain;
    ASSERT_EQ(de_env_build(&f->alloc, NULL, &img, &s, &chain), DE_OK);
    f32 worst = 0.0f;
    for (u32 m = 0; m < chain.mip_count; ++m)
        for (u32 face = 0; face < 6u; ++face)
            for (u32 y = 0; y < chain.width[m]; ++y)
                for (u32 x = 0; x < chain.width[m]; ++x)
                    for (u32 ch = 0; ch < 3u; ++ch) {
                        f32 e = fabsf(cube_texel(&chain, m, face, x, y)[ch] - c[ch]);
                        worst = e > worst ? e : worst;
                    }
    EXPECT_TRUE((worst) < (1e-3f));
    de_tex_chain_free(&f->alloc, &chain);

    /* The firefly clamp scales the prefilter input down to the clamp luminance; mip 0 stays. */
    const f32 lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    s.clamp_luminance = 0.1f;
    ASSERT_EQ(de_env_build(&f->alloc, NULL, &img, &s, &chain), DE_OK);
    EXPECT_FLOAT_EQ(cube_texel(&chain, 0, 4, 3, 3)[0], c[0], 1e-5f);
    for (u32 m = 1; m < chain.mip_count; ++m)
        for (u32 ch = 0; ch < 3u; ++ch)
            EXPECT_FLOAT_EQ(cube_texel(&chain, m, 2, 0, 0)[ch], c[ch] * 0.1f / lum, 1e-3f);
    de_tex_chain_free(&f->alloc, &chain);
    de_tex_image_free(&f->alloc, &img);
}

/* ---- import.env.prefilter_widens_with_roughness ---------------------------------------------- */

TEST_F(env, prefilter_widens_with_roughness, env_fx) {
    /* A bright cap around +Z (elevation above about 80 degrees) on black. The peak straight up
     * only falls with roughness, light spreads further from the cap in the rough mips, and
     * nothing reaches below the horizon. */
    de_tex_image img = make_image(f, 64, 32, 1, false);
    for (u32 x = 0; x < 64u; ++x)
        for (u32 y = 0; y < 2u; ++y)
            put_px(&img, 0, x, y, 100.0f, 100.0f, 100.0f);
    de_env_settings s = settings(16, 64);
    de_tex_chain chain;
    ASSERT_EQ(de_env_build(&f->alloc, NULL, &img, &s, &chain), DE_OK);
    ASSERT_EQ(chain.mip_count, 5u);
    f32 prev_peak = 1e30f;
    for (u32 m = 0; m < chain.mip_count; ++m) {
        u32 n = chain.width[m], mid = n / 2u;
        f32 peak = cube_texel(&chain, m, 4, mid, mid)[0];
        EXPECT_TRUE((peak) <= (prev_peak * 1.0001f));
        EXPECT_TRUE((peak) > (1.0f));
        prev_peak = peak;
    }
    /* Face -Y, texel (0, 0) of each mip, looks about 40 degrees up and 50 degrees away from
     * the cap: GGX tails reach it a little at r = 0.25 (mip 1), much more at r = 0.75 (mip 3). */
    EXPECT_TRUE((cube_texel(&chain, 1, 3, 0, 0)[0]) < (0.5f * cube_texel(&chain, 3, 3, 0, 0)[0]));
    /* Mip 3 (2x2, r = 0.75): +Y face texel (0, 1) looks along (-0.5, 1, 0.5). */
    EXPECT_TRUE((cube_texel(&chain, 3, 2, 0, 1)[0]) > (0.05f));
    /* Below the horizon stays dark in every mip: face -Z. */
    for (u32 m = 0; m < chain.mip_count; ++m)
        EXPECT_TRUE((cube_texel(&chain, m, 5, 0, 0)[0]) < (1e-3f));
    de_tex_chain_free(&f->alloc, &chain);
    de_tex_image_free(&f->alloc, &img);
}

/* ---- import.env.prefilter_mip_sizes ---------------------------------------------------------- */

TEST_F(env, prefilter_mip_sizes, env_fx) {
    de_tex_image img = make_image(f, 64, 32, 1, false);
    fill(&img, 1.0f, 2.0f, 3.0f);
    write_exr("etmp:/env/const.exr", &img);
    de_tex_image_free(&f->alloc, &img);

    make_cooker(f, NULL);
    de__manifest_kv kv[3] = {kv_int("env_size", 32), kv_int("env_samples", 16),
                             kv_float("rotation_deg", 30.0)};
    de_cook_result r;
    ASSERT_EQ(cook_env(f, "etmp:/env", "const.exr", kv, 3, &r), DE_OK);
    de_cooked_env_view v;
    ASSERT_TRUE(env_view(&r, &v));
    /* DTEX part: one cube, 32 down to 1, usage environment, CUBE flag. */
    EXPECT_EQ(v.texture.dim, (u32)DE_TEX_CUBE);
    EXPECT_EQ(v.texture.depth_or_layers, 6u);
    EXPECT_EQ(v.texture.width, 32u);
    EXPECT_EQ(v.texture.height, 32u);
    EXPECT_EQ(v.texture.mip_count, 6u);
    EXPECT_EQ(v.texture.usage, (u32)DE_COOKED_TEXTURE_USAGE_ENVIRONMENT);
    EXPECT_TRUE((v.texture.flags & DE_COOKED_TEXTURE_CUBE) != 0u);
    EXPECT_EQ(v.texture.format, (u32)DE_FMT_RGBA16_FLOAT);
    for (u32 face = 0; face < 6u; ++face)
        for (u32 m = 0; m < 6u; ++m) {
            de_cooked_texture_subresource sub;
            memset(&sub, 0, sizeof sub);
            ASSERT_TRUE(de_cooked_texture_subresource_at(&v.texture, face * 6u + m, &sub));
            EXPECT_EQ(sub.width, 32u >> m);
            EXPECT_EQ(sub.height, 32u >> m);
            EXPECT_EQ(sub.offset % DE_COOKED_TEXTURE_SUBRESOURCE_ALIGN, 0u);
            /* Constant input: every texel of every mip is the constant (half precision). */
            EXPECT_FLOAT_EQ(half_at(&v.texture, face, m, 0, sub.height - 1u, 2), 3.0f, 2e-3f);
        }
    /* Trailer: right after the DTEX part, roughness -> mip linear over 5 mips. */
    EXPECT_EQ(v.dtex_size + DE_COOKED_ENV_TRAILER_SIZE, r.size);
    EXPECT_EQ(v.dtex_size % 256u, 0u);
    EXPECT_EQ(v.face_size, 32u);
    EXPECT_EQ(v.mip_count, 6u);
    EXPECT_EQ(v.roughness_mapping, (u32)DE_COOKED_ENV_ROUGHNESS_LINEAR);
    EXPECT_FLOAT_EQ(v.mip_per_roughness, 5.0f, 0.0f);
    EXPECT_FLOAT_EQ(de_cooked_env_roughness_to_mip(&v, 0.4f), 2.0f, 0.0f);
    EXPECT_FLOAT_EQ(de_cooked_env_roughness_to_mip(&v, 1.0f), 5.0f, 0.0f);
    EXPECT_FLOAT_EQ(de_cooked_env_roughness_to_mip(&v, 2.0f), 5.0f, 0.0f);
    EXPECT_EQ(v.sample_count, 16u);
    EXPECT_FLOAT_EQ(v.rotation_deg, 30.0f, 0.0f);
    EXPECT_FLOAT_EQ(v.clamp_luminance, 0.0f, 0.0f);
    const f32 lum = 0.2126f + 0.7152f * 2.0f + 0.0722f * 3.0f;
    EXPECT_FLOAT_EQ(v.mean_luminance, lum, 1e-4f);
    EXPECT_FLOAT_EQ(v.max_luminance, lum, 1e-4f);
    f32 e[3];
    de_cooked_env_irradiance(&v, 0.0f, 0.0f, 1.0f, e);
    EXPECT_FLOAT_EQ(e[1], (f32)ENVT_PI * 2.0f, 2e-3f);

    /* The whole blob is not a plain DTEX blob; its first dtex_size bytes are. */
    de_cooked_texture_view tv;
    EXPECT_FALSE(de_cooked_texture_parse(r.data, r.size, &tv, NULL));
    EXPECT_TRUE(de_cooked_texture_parse(r.data, v.dtex_size, &tv, NULL));
    /* Damaged trailers are refused. */
    u8 *bad = (u8 *)malloc((size_t)r.size);
    ASSERT_TRUE(bad != NULL);
    const char *why = NULL;
    memcpy(bad, r.data, (size_t)r.size);
    bad[v.dtex_size] = 'X';
    EXPECT_FALSE(de_cooked_env_parse(bad, r.size, &v, &why));
    memcpy(bad, r.data, (size_t)r.size);
    de_cooked_put_u32(bad + v.dtex_size + DE_COOKED_ENV_OFF_MIP_COUNT, 5);
    EXPECT_FALSE(de_cooked_env_parse(bad, r.size, &v, &why));
    memcpy(bad, r.data, (size_t)r.size);
    de_cooked_put_u32(bad + v.dtex_size + DE_COOKED_ENV_OFF_RESERVED, 1);
    EXPECT_FALSE(de_cooked_env_parse(bad, r.size, &v, &why));
    EXPECT_FALSE(de_cooked_env_parse(r.data, r.size - 1u, &v, &why));
    EXPECT_FALSE(de_cooked_env_parse(r.data, v.dtex_size, &v, &why));
    free(bad);
    de_cook_result_free(&r);
}

/* ---- import.env.sh9_constant ----------------------------------------------------------------- */

TEST_F(env, sh9_constant, env_fx) {
    /* E(n) = integral of L max(0, n.w) dw = pi L for a constant L, at every normal. */
    const f32 c[3] = {0.5f, 0.25f, 4.0f};
    de_tex_image img = make_image(f, 32, 16, 1, false);
    fill(&img, c[0], c[1], c[2]);
    de_env_settings s = settings(16, 4);
    de_tex_chain chain;
    ASSERT_EQ(de_env_build(&f->alloc, NULL, &img, &s, &chain), DE_OK);
    de_env_sh9 sh;
    ASSERT_EQ(de_env_sh9_project(&f->alloc, NULL, chain.rgba[0], 16, &sh), DE_OK);
    de_cooked_env_view v;
    memset(&v, 0, sizeof v);
    memcpy(v.sh9, sh.irradiance, sizeof v.sh9);
    static const f32 k_dirs[8][3] = {{1, 0, 0},          {-1, 0, 0},         {0, 1, 0},
                                     {0, -1, 0},         {0, 0, 1},          {0, 0, -1},
                                     {0.6f, 0.0f, 0.8f}, {0.0f, -0.8f, 0.6f}};
    for (u32 i = 0; i < 8u; ++i) {
        f32 e[3];
        de_cooked_env_irradiance(&v, k_dirs[i][0], k_dirs[i][1], k_dirs[i][2], e);
        for (u32 ch = 0; ch < 3u; ++ch)
            EXPECT_FLOAT_EQ(e[ch] / ((f32)ENVT_PI * c[ch]), 1.0f, 1e-3f);
    }
    /* Only band 0: E_0 = pi x L x sqrt(4 pi). */
    for (u32 ch = 0; ch < 3u; ++ch) {
        EXPECT_FLOAT_EQ(sh.irradiance[0][ch], (f32)(ENVT_PI * sqrt(4.0 * ENVT_PI)) * c[ch],
                        1e-3f * c[ch] * 12.0f);
        for (u32 k = 1; k < 9u; ++k)
            EXPECT_TRUE((fabsf(sh.irradiance[k][ch])) < (1e-4f * c[ch]));
    }
    const f32 lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    EXPECT_FLOAT_EQ(sh.mean_luminance, lum, 1e-5f);
    EXPECT_FLOAT_EQ(sh.max_luminance, lum, 1e-5f);
    de_tex_chain_free(&f->alloc, &chain);
    de_tex_image_free(&f->alloc, &img);
}

/* ---- import.env.sh9_directional -------------------------------------------------------------- */

TEST_F(env, sh9_directional, env_fx) {
    /* 1. A cube that is black but for the four centre texels of face +Z. */
    const u32 n = 16;
    f32 *cube = (f32 *)DE_ALLOC(&f->alloc, (size_t)n * n * 6u * 16u, 4, DE_MEM_ASSET);
    ASSERT_TRUE(cube != NULL);
    memset(cube, 0, (size_t)n * n * 6u * 16u);
    for (u32 y = n / 2u - 1u; y <= n / 2u; ++y)
        for (u32 x = n / 2u - 1u; x <= n / 2u; ++x)
            cube[(((u64)4u * n + y) * n + x) * 4u] = 1000.0f;
    de_env_sh9 sh;
    ASSERT_EQ(de_env_sh9_project(&f->alloc, NULL, cube, n, &sh), DE_OK);
    DE_FREE(&f->alloc, cube, (size_t)n * n * 6u * 16u);
    f64 dir[3], peak;
    irradiance_peak(&sh, dir, &peak);
    EXPECT_TRUE((dir[2]) > (0.99));
    EXPECT_TRUE((peak) > (0.0));
    EXPECT_FLOAT_EQ(sh.max_luminance, 0.2126f * 1000.0f, 1e-2f);

    /* 2. Through the whole pipeline: a bright spot at +Z (the top rows of an equirect). */
    de_tex_image img = make_image(f, 64, 32, 1, false);
    const f64 up[3] = {0.0, 0.0, 1.0};
    spot(&img, up, 500.0f);
    de_env_settings s = settings(16, 4);
    de_tex_chain chain;
    ASSERT_EQ(de_env_build(&f->alloc, NULL, &img, &s, &chain), DE_OK);
    ASSERT_EQ(de_env_sh9_project(&f->alloc, NULL, chain.rgba[0], 16, &sh), DE_OK);
    irradiance_peak(&sh, dir, &peak);
    EXPECT_TRUE((dir[2]) > (0.99));
    de_tex_chain_free(&f->alloc, &chain);
    de_tex_image_free(&f->alloc, &img);
}

/* ---- import.env.rotation --------------------------------------------------------------------- */

static void rotated_peak(env_fx *f, const f64 at[3], f32 deg, f64 dir[3], de_tex_chain *keep) {
    if (keep != NULL)
        memset(keep, 0, sizeof *keep);
    de_tex_image img = make_image(f, 64, 32, 1, false);
    spot(&img, at, 500.0f);
    de_env_settings s = settings(16, 4);
    s.rotation_deg = deg;
    de_tex_chain chain;
    ASSERT_EQ(de_env_build(&f->alloc, NULL, &img, &s, &chain), DE_OK);
    de_env_sh9 sh;
    ASSERT_EQ(de_env_sh9_project(&f->alloc, NULL, chain.rgba[0], 16, &sh), DE_OK);
    f64 peak;
    irradiance_peak(&sh, dir, &peak);
    if (keep != NULL)
        *keep = chain;
    else
        de_tex_chain_free(&f->alloc, &chain);
    de_tex_image_free(&f->alloc, &img);
}

TEST_F(env, rotation, env_fx) {
    /* rotation_deg turns the environment counter-clockwise about +Z seen from above: what was
     * ahead (+Y) moves to the left (-X) at 90 degrees, to +X at -90, behind at 180. */
    const f64 fwd[3] = {0.0, 1.0, 0.0}, up[3] = {0.0, 0.0, 1.0};
    f64 dir[3];
    rotated_peak(f, fwd, 0.0f, dir, NULL);
    EXPECT_TRUE((dir[1]) > (0.98));
    de_tex_chain chain;
    rotated_peak(f, fwd, 90.0f, dir, &chain);
    EXPECT_TRUE((dir[0]) < (-0.98));
    /* The cube itself: face -X is lit at its centre, face +Y is dark. */
    EXPECT_TRUE((cube_texel(&chain, 0, 1, 7, 7)[0] + cube_texel(&chain, 0, 1, 8, 8)[0]) > (1.0f));
    EXPECT_TRUE((cube_texel(&chain, 0, 2, 7, 7)[0]) < (1e-4f));
    de_tex_chain_free(&f->alloc, &chain);
    rotated_peak(f, fwd, -90.0f, dir, NULL);
    EXPECT_TRUE((dir[0]) > (0.98));
    rotated_peak(f, fwd, 180.0f, dir, NULL);
    EXPECT_TRUE((dir[1]) < (-0.98));
    /* A spot at +Z stays at +Z: the rotation axis. */
    rotated_peak(f, up, 90.0f, dir, NULL);
    EXPECT_TRUE((dir[2]) > (0.99));
}

/* ---- import.env.deterministic_threads -------------------------------------------------------- */

/* An HDR test pattern: hashed texel values up to 50, a few fireflies of 5000. */
static de_tex_image pattern(env_fx *f) {
    de_tex_image img = make_image(f, 64, 32, 1, false);
    for (u32 y = 0; y < 32u; ++y)
        for (u32 x = 0; x < 64u; ++x) {
            u32 hsh = (x * 73856093u) ^ (y * 19349663u);
            hsh = (hsh ^ (hsh >> 13)) * 0x5bd1e995u;
            f32 r = (f32)(hsh & 0xFFu) / 5.0f, g = (f32)((hsh >> 8) & 0xFFu) / 7.0f;
            f32 b = (f32)((hsh >> 16) & 0xFFu) / 11.0f;
            if ((hsh >> 24) == 7u)
                r = g = b = 5000.0f;
            put_px(&img, 0, x, y, r, g, b);
        }
    return img;
}

typedef struct env_run {
    de_tex_chain chain;
    de_env_sh9 sh;
} env_run;

static void run_build(env_fx *f, const de_tex_image *img, de_task_system *ts, env_run *out) {
    de_env_settings s = settings(32, 64);
    s.rotation_deg = 37.0f;
    s.clamp_luminance = 20.0f;
    ASSERT_EQ(de_env_build(&f->alloc, ts, img, &s, &out->chain), DE_OK);
    ASSERT_EQ(de_env_sh9_project(&f->alloc, ts, out->chain.rgba[0], 32, &out->sh), DE_OK);
}

static void expect_same_run(const env_run *a, const env_run *b) {
    ASSERT_EQ(a->chain.mip_count, b->chain.mip_count);
    for (u32 m = 0; m < a->chain.mip_count; ++m)
        EXPECT_MEMEQ(a->chain.rgba[m], b->chain.rgba[m],
                     (size_t)a->chain.width[m] * a->chain.width[m] * 6u * 16u);
    EXPECT_MEMEQ(&a->sh, &b->sh, sizeof a->sh);
}

TEST_F(env, deterministic_threads, env_fx) {
    de_tex_image img = pattern(f);
    env_run solo, one, many;
    run_build(f, &img, NULL, &solo);
    start_tasks(f, 1);
    run_build(f, &img, f->tasks, &one);
    start_tasks(f, 4);
    run_build(f, &img, f->tasks, &many);
    expect_same_run(&solo, &one);
    expect_same_run(&solo, &many);
    de_tex_chain_free(&f->alloc, &solo.chain);
    de_tex_chain_free(&f->alloc, &one.chain);
    de_tex_chain_free(&f->alloc, &many.chain);

    /* Whole cooks (verify_determinism on: each runs twice) with and without the cooker's task
     * system give the same bytes. */
    write_exr("etmp:/env/pattern.exr", &img);
    de_tex_image_free(&f->alloc, &img);
    de__manifest_kv kv[4] = {kv_int("env_size", 16), kv_int("env_samples", 32),
                             kv_float("rotation_deg", -15.0), kv_float("clamp_luminance", 30.0)};
    make_cooker(f, f->tasks);
    de_cook_result threaded;
    ASSERT_EQ(cook_env(f, "etmp:/env", "pattern.exr", kv, 4, &threaded), DE_OK);
    stop_tasks(f);
    make_cooker(f, NULL);
    de_cook_result alone;
    ASSERT_EQ(cook_env(f, "etmp:/env", "pattern.exr", kv, 4, &alone), DE_OK);
    ASSERT_EQ(threaded.size, alone.size);
    EXPECT_MEMEQ(threaded.data, alone.data, (size_t)alone.size);
    de_cooked_env_view v;
    EXPECT_TRUE(env_view(&alone, &v));
    EXPECT_FLOAT_EQ(v.clamp_luminance, 30.0f, 0.0f);
    EXPECT_TRUE((v.max_luminance) > (30.0f)); /* mip 0 is not clamped */
    /* Other hosts: the same bytes (x86-64 and arm64, every compiler). A change of the importer
     * output updates this hash on purpose. */
    printf("  cooked environment: %llu bytes, fnv1a 0x%016llx\n", (unsigned long long)alone.size,
           (unsigned long long)fnv1a(alone.data, alone.size));
    EXPECT_EQ(fnv1a(alone.data, alone.size), 0x2b92628fba7fd47eull);
    de_cook_result_free(&threaded);
    de_cook_result_free(&alone);
}

/* ---- import.env.reject_bad_input ------------------------------------------------------------- */

TEST_F(env, reject_bad_input, env_fx) {
    make_cooker(f, NULL);
    de_cook_result r;
    /* A square image is neither equirectangular nor a cross. */
    EXPECT_EQ(cook_env(f, "envd:/", "half_rgb.exr", NULL, 0, &r), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(error_says("neither equirectangular"));
    /* Bad settings. */
    de__manifest_kv size = kv_int("env_size", 100);
    EXPECT_EQ(cook_env(f, "envd:/", "half_rgb.exr", &size, 1, &r), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(error_says("env_size"));
    de__manifest_kv samples = kv_int("env_samples", 0);
    EXPECT_EQ(cook_env(f, "envd:/", "half_rgb.exr", &samples, 1, &r), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(error_says("env_samples"));
    de__manifest_kv clamp = kv_float("clamp_luminance", -1.0);
    EXPECT_EQ(cook_env(f, "envd:/", "half_rgb.exr", &clamp, 1, &r), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(error_says("clamp_luminance"));
    /* Block-compressed KTX2 levels cannot be prefiltered. */
    EXPECT_EQ(cook_env(f, "envd:/", "bc7.ktx2", NULL, 0, &r), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(error_says("block-compressed"));
    /* Arrays and odd shapes, without the cooker. */
    de_tex_image img = make_image(f, 30, 20, 1, false);
    de_env_layout layout;
    EXPECT_EQ(de_env_source_layout(&img, &layout), DE_ERR_UNSUPPORTED);
    de_tex_image_free(&f->alloc, &img);
    img = make_image(f, 8, 4, 2, false);
    EXPECT_EQ(de_env_source_layout(&img, &layout), DE_ERR_UNSUPPORTED);
    de_tex_image_free(&f->alloc, &img);
}
