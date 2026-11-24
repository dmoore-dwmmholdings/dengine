/*
 * test_world_cook.c: `de-cook world` (DENG-271, tools/cook/world_cells.c) and the [worlds.<name>]
 * manifest checks (src/asset/manifest.c, resolve.c).
 *
 * The fixture tests/assets/world_small/ holds a world "small" of 2 scenes and 7 entities, and its
 * committed cook output assets/worlds/small/ (4 cells and cells.toml). The tests cook it into two
 * scratch directories (build tree) and compare the bytes with each other and with the golden.
 * world_cook_proxy (DENG-830) cooks the golden world "near" (tests/golden/world/assets), whose
 * proxies are enabled, and checks the far proxy glTF and its cells.toml entries.
 * world_cook_terrain_proxy (DENG-871) cooks a scratch world with a DeTerrain and proxies enabled
 * and checks the terrain heightfield in the far proxies by structure (triangle count, bounds,
 * the same bytes from two cooks), not against a golden.
 * world_cook_pin (DENG-831) cooks a scratch world whose roots carry DeWorldCellPin.
 * To refresh the golden after an intended format change:
 *   de-cook world tests/assets/world_small/assets.toml --world small
 */
#define _CRT_SECURE_NO_WARNINGS /* fopen in slurp(), for the MSVC CRT */
#include "de_test.h"
#include <dengine/de_base.h>
#include <dengine/de_platform.h>
#include "cook/check.h"
#include "cook/world_cells.h"
#include "cook/world_proxy.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXTURE DE_STREAMING_FIXTURE_DIR
#define TMP DE_STREAMING_TMP_DIR

/* ---- Helpers --------------------------------------------------------------------------------- */

typedef struct wc_cap {
    char out[16384];
    size_t len;
} wc_cap;

static void wc_cap_write(void *user, de_cook_stream stream, const char *text, size_t len) {
    wc_cap *c = (wc_cap *)user;
    (void)stream;
    if (c->len + len >= sizeof c->out)
        len = sizeof c->out - 1 - c->len;
    memcpy(c->out + c->len, text, len);
    c->len += len;
    c->out[c->len] = '\0';
}

static void tmp_mount(void) {
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("wctmp", TMP, DE_MOUNT_WRITABLE, 0), DE_OK);
        mounted = true;
    }
}

/* Reads a native file whole; NULL when missing. *len gets the size. */
static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    char *buf = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n + 4096 > cap) {
            cap = cap * 2 + 8192;
            char *nb = (char *)realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
        }
        size_t got = fread(buf + n, 1, 4096, f);
        n += got;
        if (got < 4096)
            break;
    }
    fclose(f);
    *len = n;
    return buf;
}

/* Byte-compares two native files. */
static bool same_bytes(const char *a, const char *b) {
    size_t la = 0, lb = 0;
    char *x = slurp(a, &la), *y = slurp(b, &lb);
    bool same = x != NULL && y != NULL && la == lb && memcmp(x, y, la) == 0;
    if (!same)
        printf("  differs: %s vs %s\n", a, b);
    free(x);
    free(y);
    return same;
}

static int run_world(const char *out_dir, wc_cap *cap) {
    const char *argv[] = {FIXTURE "/assets.toml", "--world", "small", "--out", out_dir};
    de_cook_io io = {wc_cap_write, cap};
    return de_cook_world_main(5, argv, &io);
}

static const char *const k_outputs[] = {
    "cells.toml", "cell_-1_-3.scene.toml", "cell_0_0.scene.toml", "cell_1_0.scene.toml",
    "cell_390_390.scene.toml"};

/* ---- test_world_cook_deterministic ----------------------------------------------------------- */

TEST(streaming, world_cook_deterministic) {
    tmp_mount();
    ASSERT_EQ(de_dir_create("wctmp:/cook_a"), DE_OK);
    ASSERT_EQ(de_dir_create("wctmp:/cook_b"), DE_OK);
    wc_cap a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    ASSERT_EQ(run_world(TMP "/cook_a", &a), DE_COOK_EXIT_OK);
    ASSERT_EQ(run_world(TMP "/cook_b", &b), DE_COOK_EXIT_OK);
    if (strstr(a.out, "7 entities in 4 cells, 0 warnings") == NULL)
        printf("%s", a.out);
    EXPECT_TRUE(strstr(a.out, "de-cook world: \"small\": 7 entities in 4 cells, 0 warnings") !=
                NULL);
    EXPECT_STREQ(a.out, b.out);

    char pa[1024], pb[1024], pg[1024];
    for (size_t i = 0; i < sizeof k_outputs / sizeof k_outputs[0]; ++i) {
        snprintf(pa, sizeof pa, "%s/cook_a/%s", TMP, k_outputs[i]);
        snprintf(pb, sizeof pb, "%s/cook_b/%s", TMP, k_outputs[i]);
        snprintf(pg, sizeof pg, "%s/assets/worlds/small/%s", FIXTURE, k_outputs[i]);
        EXPECT_TRUE(same_bytes(pa, pb));
        EXPECT_TRUE(same_bytes(pa, pg)); /* the committed golden */
    }
    /* The far cell holds the 100 km entity relative to its origin (390*256 = 99840). */
    size_t n = 0;
    snprintf(pa, sizeof pa, "%s/cook_a/cell_390_390.scene.toml", TMP);
    char *far = slurp(pa, &n);
    ASSERT_TRUE(far != NULL);
    far = (char *)realloc(far, n + 1);
    ASSERT_TRUE(far != NULL);
    far[n] = '\0';
    EXPECT_TRUE(strstr(far, "position = [160.0, 160.125, 3.0]") != NULL);
    free(far);
}

/* ---- test_world_cook_proxy (DENG-830) ------------------------------------------------------ */

/* The golden world "near" (tests/golden/world/assets) has proxies enabled: its cell (0, 0) bakes a
 * far proxy glTF with one primitive per base colour, listed in cells.toml as a model and a
 * bundle. */
#define GOLDEN_WORLD FIXTURE "/../../golden/world/assets"

static int run_near(const char *out_dir, wc_cap *cap) {
    const char *argv[] = {GOLDEN_WORLD "/assets.toml", "--world", "near", "--out", out_dir};
    de_cook_io io = {wc_cap_write, cap};
    return de_cook_world_main(5, argv, &io);
}

static char *slurp_text(const char *dir, const char *file) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s/%s", TMP, dir, file);
    size_t n = 0;
    char *t = slurp(path, &n);
    char *z = t ? (char *)realloc(t, n + 1) : NULL;
    if (z == NULL) {
        free(t);
        return NULL;
    }
    z[n] = '\0';
    return z;
}

static size_t count_of(const char *hay, const char *needle) {
    size_t n = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle))
        ++n;
    return n;
}

TEST(streaming, world_cook_proxy) {
    tmp_mount();
    ASSERT_EQ(de_dir_create("wctmp:/proxy_a"), DE_OK);
    ASSERT_EQ(de_dir_create("wctmp:/proxy_b"), DE_OK);
    wc_cap a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    ASSERT_EQ(run_near(TMP "/proxy_a", &a), DE_COOK_EXIT_OK);
    ASSERT_EQ(run_near(TMP "/proxy_b", &b), DE_COOK_EXIT_OK);
    EXPECT_STREQ(a.out, b.out);

    /* Deterministic, and equal to the committed golden. */
    char pa[1024], pb[1024], pg[1024];
    static const char *const files[] = {"cells.toml", "cell_0_0.proxy.gltf"};
    for (size_t i = 0; i < 2; ++i) {
        snprintf(pa, sizeof pa, "%s/proxy_a/%s", TMP, files[i]);
        snprintf(pb, sizeof pb, "%s/proxy_b/%s", TMP, files[i]);
        snprintf(pg, sizeof pg, "%s/worlds/near/%s", GOLDEN_WORLD, files[i]);
        EXPECT_TRUE(same_bytes(pa, pb));
        EXPECT_TRUE(same_bytes(pa, pg));
    }

    char *cells = slurp_text("proxy_a", "cells.toml");
    ASSERT_TRUE(cells != NULL);
    EXPECT_TRUE(strstr(cells, "\"world/near/proxy_0_0\" = { type = \"model\", source = "
                              "\"worlds/near/cell_0_0.proxy.gltf\"") != NULL);
    EXPECT_TRUE(strstr(cells, "[bundles.\"world/near/proxy_0_0\"]") != NULL);
    free(cells);

    /* Four colour groups (ground, red, green, white): four primitives and materials. */
    char *gl = slurp_text("proxy_a", "cell_0_0.proxy.gltf");
    ASSERT_TRUE(gl != NULL);
    EXPECT_EQ(count_of(gl, "\"name\":\"proxy"), (size_t)6); /* mesh, node, proxy0..3 */
    EXPECT_TRUE(strstr(gl, "\"name\":\"proxy3\"") != NULL);
    EXPECT_TRUE(strstr(gl, "\"name\":\"proxy4\"") == NULL);
    EXPECT_TRUE(strstr(gl, "\"TEXCOORD_0\"") == NULL);
    free(gl);
}

/* ---- test_world_cook_terrain_proxy (DENG-871) ---------------------------------------------- */

/* A 128 m terrain (33 x 33 samples, 4 m apart) at (200, 100, 5), heights i * 60 for sample i and
 * a hole at sample 0, in a world "tp" with proxies at triangle_ratio 0.25 (terrain step 2). Its
 * x = 0 tiles (200..264 m) overlap cells (0, 0) and (1, 0); the others only (1, 0). No entity draws
 * a mesh, so each proxy is the terrain alone. Writes into <TMP>/<dir>/ and cooks into
 * <TMP>/<dir>/out. */
static int cook_terrain_world(const char *dir, wc_cap *cap) {
    tmp_mount();
    static const char manifest[] =
        "[project]\nasset_root = \".\"\n"
        "[assets]\n\"levels/t\" = { type = \"scene\", source = \"t.scene.toml\" }\n"
        "\"mats/red\" = { type = \"material\", source = \"red.material.toml\" }\n"
        "\"mats/blue\" = { type = \"material\", source = \"blue.material.toml\" }\n"
        "[worlds.tp]\nscenes = [\"levels/t\"]\n[worlds.tp.proxy]\ntriangle_ratio = 0.25\n";
    char vp[256], out[1024], toml[1024];
    snprintf(vp, sizeof vp, "wctmp:/%s", dir);
    if (de_dir_create(vp) != DE_OK)
        return -1;
    snprintf(vp, sizeof vp, "wctmp:/%s/out", dir);
    if (de_dir_create(vp) != DE_OK)
        return -1;
    snprintf(vp, sizeof vp, "wctmp:/%s/assets.toml", dir);
    if (de_file_write_all_atomic(vp, manifest, sizeof manifest - 1) != DE_OK)
        return -1;
    static const char red[] = "base_color = [1.0, 0.0, 0.0, 1.0]\n";
    static const char blue[] = "base_color = [0.0, 0.0, 1.0, 1.0]\n";
    snprintf(vp, sizeof vp, "wctmp:/%s/red.material.toml", dir);
    if (de_file_write_all_atomic(vp, red, sizeof red - 1) != DE_OK)
        return -1;
    snprintf(vp, sizeof vp, "wctmp:/%s/blue.material.toml", dir);
    if (de_file_write_all_atomic(vp, blue, sizeof blue - 1) != DE_OK)
        return -1;
    size_t scap = 64u * 1024u, len = 0;
    char *scene = (char *)malloc(scap);
    if (scene == NULL)
        return -1;
    len += (size_t)snprintf(scene + len, scap - len,
                            "[entity.0x0000000000000100.DeTransform]\n"
                            "position = [200.0, 100.0, 5.0]\n"
                            "[entity.0x0000000000000100.DeTerrain]\nsize = 128.0\n"
                            "height = 10.0\nheights = [");
    for (unsigned i = 0; i < 33u * 33u; ++i)
        len += (size_t)snprintf(scene + len, scap - len, "%s%u", i ? ", " : "", i * 60u);
    len += (size_t)snprintf(scene + len, scap - len, "]\nholes = [1");
    for (unsigned i = 1; i < 33u * 33u; ++i)
        len += (size_t)snprintf(scene + len, scap - len, ", 0");
    len += (size_t)snprintf(scene + len, scap - len, "]\n");
    /* Layer 0 red, layer 1 blue; the splat shifts from red to blue along +x: sample column c has
     * weights (224 - 7c, 7c), so its colour is (1 - c / 32, 0, c / 32). */
    len += (size_t)snprintf(scene + len, scap - len,
                            "layers = [{ material = \"mats/red\" }, { material = \"mats/blue\" }]\n"
                            "splat = [");
    for (unsigned i = 0; i < 33u * 33u; ++i)
        len += (size_t)snprintf(scene + len, scap - len, "%s%u, %u, 0, 0", i ? ", " : "",
                                224u - 7u * (i % 33u), 7u * (i % 33u));
    len += (size_t)snprintf(scene + len, scap - len, "]\n");
    snprintf(vp, sizeof vp, "wctmp:/%s/t.scene.toml", dir);
    de_result wr = len < scap ? de_file_write_all_atomic(vp, scene, len) : DE_ERR_INVALID_ARGUMENT;
    free(scene);
    if (wr != DE_OK)
        return -1;
    snprintf(toml, sizeof toml, "%s/%s/assets.toml", TMP, dir);
    snprintf(out, sizeof out, "%s/%s/out", TMP, dir);
    memset(cap, 0, sizeof *cap);
    const char *argv[] = {toml, "--world", "tp", "--out", out};
    de_cook_io io = {wc_cap_write, cap};
    return de_cook_world_main(5, argv, &io);
}

/* The first `"key":[x,y,z]` after `from` in `gl`; false when absent. */
static bool vec3_after(const char *gl, const char *from, const char *key, double v[3]) {
    const char *p = strstr(gl, from);
    p = p != NULL ? strstr(p, key) : NULL;
    if (p == NULL)
        return false;
    p += strlen(key);
    for (int i = 0; i < 3; ++i) {
        char *end = NULL;
        v[i] = strtod(p, &end);
        if (end == p)
            return false;
        p = end + 1;
    }
    return true;
}

static unsigned position_count(const char *gl) {
    static const char k[] = "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":";
    const char *p = strstr(gl, k);
    return p != NULL ? (unsigned)strtoul(p + sizeof k - 1, NULL, 10) : 0u;
}

/* The binary buffer of a glTF with one base64 data URI buffer, decoded; NULL when malformed. */
static unsigned char *buffer_of(const char *gl, size_t *len) {
    static const char k[] = "base64,";
    const char *p = strstr(gl, k);
    if (p == NULL)
        return NULL;
    p += sizeof k - 1;
    const char *e = strchr(p, '"');
    if (e == NULL)
        return NULL;
    unsigned char *out = (unsigned char *)malloc((size_t)(e - p) / 4u * 3u + 3u);
    if (out == NULL)
        return NULL;
    size_t n = 0;
    unsigned acc = 0;
    int bits = 0;
    for (; p < e && *p != '='; ++p) {
        const char *q = strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", *p);
        if (q == NULL || *p == '\0') {
            free(out);
            return NULL;
        }
        acc = (acc << 6) | (unsigned)(q - "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (unsigned char)(acc >> bits);
        }
    }
    *len = n;
    return out;
}

/* Checks the terrain primitive's COLOR_0 of a cell proxy (DENG-882): one colour per vertex, the
 * splat's (1 - c / 32, 0, c / 32) of the vertex's sample column c, so blue is (x - 200 m) / 128
 * for the vertex at x metres (`ox`: the cell's origin) and red the rest. */
static void expect_vertex_colors(const char *gl, double ox) {
    const unsigned vc = position_count(gl);
    ASSERT_TRUE(vc > 0u);
    EXPECT_TRUE(strstr(gl, "\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"COLOR_0\":3}") != NULL);
    EXPECT_TRUE(strstr(gl, "\"componentType\":5126,\"count\":") != NULL);
    size_t len = 0;
    unsigned char *bin = buffer_of(gl, &len);
    ASSERT_TRUE(bin != NULL);
    const size_t vbytes = (size_t)vc * 12u, ibytes = (size_t)vc * 4u;
    ASSERT_EQ(len, vbytes * 3u + ibytes);
    float lo = 2.0f, hi = -1.0f;
    size_t bad = 0;
    for (unsigned v = 0; v < vc; ++v) {
        float p[3], c[3];
        memcpy(p, bin + (size_t)v * 12u, sizeof p);
        memcpy(c, bin + vbytes * 2u + ibytes + (size_t)v * 12u, sizeof c);
        const double blue = (ox + (double)p[0] - 200.0) / 128.0;
        if (fabs((double)c[2] - blue) > 1e-5 || fabs((double)c[0] - (1.0 - blue)) > 1e-5 ||
            c[1] != 0.0f)
            bad++;
        lo = c[2] < lo ? c[2] : lo;
        hi = c[2] > hi ? c[2] : hi;
    }
    EXPECT_EQ(bad, (size_t)0);
    EXPECT_TRUE(hi - lo > 0.1f); /* it varies across the cell */
    free(bin);
}

TEST(streaming, world_cook_terrain_proxy) {
    /* The decimation step of the triangle ratio. */
    EXPECT_EQ(de__wproxy_terrain_step(1.0f), 1u);
    EXPECT_EQ(de__wproxy_terrain_step(0.25f), 2u);
    EXPECT_EQ(de__wproxy_terrain_step(0.1f), 2u);
    EXPECT_EQ(de__wproxy_terrain_step(0.05f), 4u);
    EXPECT_EQ(de__wproxy_terrain_step(0.01f), 8u);
    EXPECT_EQ(de__wproxy_terrain_step(0.0001f), 16u);

    wc_cap a, b;
    ASSERT_EQ(cook_terrain_world("tproxy_a", &a), DE_COOK_EXIT_OK);
    ASSERT_EQ(cook_terrain_world("tproxy_b", &b), DE_COOK_EXIT_OK);
    if (strstr(a.out, "0 warnings") == NULL)
        printf("%s", a.out);
    EXPECT_STREQ(a.out, b.out);

    /* Deterministic: two cooks give the same bytes. */
    static const char *const files[] = {"cells.toml", "cell_0_0.proxy.gltf",
                                        "cell_1_0.proxy.gltf"};
    char pa[1024], pb[1024];
    for (size_t i = 0; i < 3; ++i) {
        snprintf(pa, sizeof pa, "%s/tproxy_a/out/%s", TMP, files[i]);
        snprintf(pb, sizeof pb, "%s/tproxy_b/out/%s", TMP, files[i]);
        EXPECT_TRUE(same_bytes(pa, pb));
    }

    /* Both cells list a proxy; the terrain-only cell (1, 0) too. */
    char *cells = slurp_text("tproxy_a/out", "cells.toml");
    ASSERT_TRUE(cells != NULL);
    EXPECT_TRUE(strstr(cells, "[bundles.\"world/tp/proxy_0_0\"]") != NULL);
    EXPECT_TRUE(strstr(cells, "[bundles.\"world/tp/proxy_1_0\"]") != NULL);
    free(cells);

    /* Cell (0, 0): step 2 keeps 8 x 8 quads of each 16 x 16 tile. Of the x = 0 tiles, the 7 quad
     * columns whose centres are below x = 256 m, by 16 rows (tiles y = 0 and 1; y = 2 is one
     * sample wide), less the quad on the hole: (7 * 16 - 1) * 2 triangles. One primitive. */
    char *gl = slurp_text("tproxy_a/out", "cell_0_0.proxy.gltf");
    ASSERT_TRUE(gl != NULL);
    EXPECT_EQ(position_count(gl), (unsigned)((7u * 16u - 1u) * 2u * 3u));
    EXPECT_EQ(count_of(gl, "\"name\":\"proxy"), (size_t)3); /* node, mesh, proxy0 */
    /* The colour is per vertex; the material's factor is white. */
    EXPECT_TRUE(strstr(gl, "\"baseColorFactor\":[1.0,1.0,1.0,1.0]") != NULL);
    expect_vertex_colors(gl, 0.0);
    /* Bounds in glTF axes (x, z up, -y), metres from the cell origin. Heights: 5 m plus sample
     * i's i * 60 / 65535 * 10 m; the lowest kept vertex is sample 2 (the hole drops sample 0's
     * quad), the highest sample 32 * 33 + 14. */
    double lo[3], hi[3];
    ASSERT_TRUE(vec3_after(gl, "\"accessors\"", "\"min\":[", lo));
    ASSERT_TRUE(vec3_after(gl, "\"accessors\"", "\"max\":[", hi));
    EXPECT_FLOAT_EQ(lo[0], 200.0, 1e-4);
    EXPECT_FLOAT_EQ(hi[0], 256.0, 1e-4);
    EXPECT_FLOAT_EQ(lo[1], 5.0 + 2.0 * 60.0 / 65535.0 * 10.0, 1e-4);
    EXPECT_FLOAT_EQ(hi[1], 5.0 + (32.0 * 33.0 + 14.0) * 60.0 / 65535.0 * 10.0, 1e-4);
    EXPECT_FLOAT_EQ(lo[2], -228.0, 1e-4);
    EXPECT_FLOAT_EQ(hi[2], -100.0, 1e-4);
    free(gl);

    /* Cell (1, 0): the last quad column of the x = 0 tiles and all 8 of the x = 1 tiles. */
    gl = slurp_text("tproxy_a/out", "cell_1_0.proxy.gltf");
    ASSERT_TRUE(gl != NULL);
    EXPECT_EQ(position_count(gl), (unsigned)(9u * 16u * 2u * 3u));
    expect_vertex_colors(gl, 256.0);
    ASSERT_TRUE(vec3_after(gl, "\"accessors\"", "\"min\":[", lo));
    ASSERT_TRUE(vec3_after(gl, "\"accessors\"", "\"max\":[", hi));
    EXPECT_FLOAT_EQ(lo[0], 0.0, 1e-4);   /* 256 m */
    EXPECT_FLOAT_EQ(hi[0], 72.0, 1e-4);  /* 328 m */
    EXPECT_FLOAT_EQ(lo[2], -228.0, 1e-4);
    EXPECT_FLOAT_EQ(hi[2], -100.0, 1e-4);
    free(gl);
}

/* ---- test_world_cook_pin (DENG-831) --------------------------------------------------------- */

/* Writes a world "pin" of one scene `scene` into <TMP>/<dir>/ and cooks it into <TMP>/<dir>/out. */
static int cook_pin_world(const char *dir, const char *scene, wc_cap *cap) {
    tmp_mount();
    static const char manifest[] =
        "[project]\nasset_root = \".\"\n"
        "[assets]\n\"levels/p\" = { type = \"scene\", source = \"p.scene.toml\" }\n"
        "[worlds.pin]\nscenes = [\"levels/p\"]\n";
    char vp[256], out[1024], toml[1024];
    snprintf(vp, sizeof vp, "wctmp:/%s", dir);
    if (de_dir_create(vp) != DE_OK)
        return -1;
    snprintf(vp, sizeof vp, "wctmp:/%s/out", dir);
    if (de_dir_create(vp) != DE_OK)
        return -1;
    snprintf(vp, sizeof vp, "wctmp:/%s/assets.toml", dir);
    if (de_file_write_all_atomic(vp, manifest, sizeof manifest - 1) != DE_OK)
        return -1;
    snprintf(vp, sizeof vp, "wctmp:/%s/p.scene.toml", dir);
    if (de_file_write_all_atomic(vp, scene, strlen(scene)) != DE_OK)
        return -1;
    snprintf(toml, sizeof toml, "%s/%s/assets.toml", TMP, dir);
    snprintf(out, sizeof out, "%s/%s/out", TMP, dir);
    memset(cap, 0, sizeof *cap);
    const char *argv[] = {toml, "--world", "pin", "--out", out};
    de_cook_io io = {wc_cap_write, cap};
    return de_cook_world_main(5, argv, &io);
}

/* Reads <TMP>/<dir>/out/<file> as a C string; NULL when missing. */
static char *slurp_out(const char *dir, const char *file) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s/out/%s", TMP, dir, file);
    size_t n = 0;
    char *s = slurp(path, &n);
    if (s == NULL)
        return NULL;
    char *z = (char *)realloc(s, n + 1);
    if (z == NULL) {
        free(s);
        return NULL;
    }
    z[n] = '\0';
    return z;
}

#define PIN_ENT(id, extra, x)                                                                      \
    "[[entity]]\nid = \"" id "\"\n" extra "[entity.DeTransform]\nposition = [" x                    \
    ", 10.0, 0.0]\nrotation = [0.0, 0.0, 0.0, 1.0]\nscale = [1.0, 1.0, 1.0]\n"

TEST(streaming, world_cook_pin) {
    /* 0x10 at (10, 10) is pinned to (3, -2) and takes its child 0x11 (at 300 m, cell (1, 0)
     * alone) along; 0x12 (pin only, no transform) goes to (-4, 7); 0x13 stays by position. */
    static const char scene[] =
        PIN_ENT("0x0000000000000010", "", "10.0")
        "[entity.DeWorldCellPin]\ncx = 3\ncy = -2\n"
        PIN_ENT("0x0000000000000011", "parent = \"0x0000000000000010\"\n", "300.0")
        "[[entity]]\nid = \"0x0000000000000012\"\n[entity.DeWorldCellPin]\ncx = -4\ncy = 7\n"
        PIN_ENT("0x0000000000000013", "", "20.0");
    wc_cap c;
    ASSERT_EQ(cook_pin_world("pin_ok", scene, &c), DE_COOK_EXIT_OK);
    if (strstr(c.out, "0 warnings") == NULL)
        printf("%s", c.out);
    EXPECT_TRUE(strstr(c.out, "4 entities in 3 cells, 0 warnings") != NULL);
    char *pinned = slurp_out("pin_ok", "cell_3_-2.scene.toml");
    char *pin_only = slurp_out("pin_ok", "cell_-4_7.scene.toml");
    char *plain = slurp_out("pin_ok", "cell_0_0.scene.toml");
    char *none = slurp_out("pin_ok", "cell_1_0.scene.toml");
    ASSERT_TRUE(pinned != NULL && pin_only != NULL && plain != NULL);
    EXPECT_TRUE(none == NULL);
    EXPECT_TRUE(strstr(pinned, "0x0000000000000010") != NULL);
    EXPECT_TRUE(strstr(pinned, "0x0000000000000011") != NULL);
    EXPECT_TRUE(strstr(pin_only, "0x0000000000000012") != NULL);
    EXPECT_TRUE(strstr(plain, "0x0000000000000013") != NULL);
    EXPECT_TRUE(strstr(plain, "0x0000000000000010") == NULL);
    free(pinned);
    free(pin_only);
    free(plain);
    free(none);

    /* A malformed pin is an error; the root falls back to its position. */
    static const char bad[] =
        PIN_ENT("0x0000000000000020", "", "10.0") "[entity.DeWorldCellPin]\ncx = \"a\"\ncy = 0\n";
    EXPECT_EQ(cook_pin_world("pin_bad", bad, &c), DE_COOK_EXIT_ERRORS);
    if (strstr(c.out, "DeWorldCellPin needs integer cx and cy") == NULL) {
        printf("%s", c.out);
        EXPECT_TRUE(false);
    }
}

/* Bad arguments are a usage error and write nothing. */
TEST(streaming, world_cook_usage) {
    wc_cap c;
    memset(&c, 0, sizeof c);
    de_cook_io io = {wc_cap_write, &c};
    const char *no_world[] = {FIXTURE "/assets.toml"};
    EXPECT_EQ(de_cook_world_main(1, no_world, &io), DE_COOK_EXIT_USAGE);
    const char *unknown[] = {FIXTURE "/assets.toml", "--world", "nope", "--out", TMP};
    EXPECT_NE(de_cook_world_main(5, unknown, &io), DE_COOK_EXIT_OK);
    EXPECT_FALSE(de_path_exists("wctmp:/assets/worlds/nope"));
    EXPECT_EQ(de_cook_world_main(0, NULL, &io), DE_COOK_EXIT_USAGE);
}

/* ---- test_world_manifest_check --------------------------------------------------------------- */

/* Writes `toml` as <TMP>/mcheck/<name>.toml and runs `de-cook --check` on it. */
static int check_manifest(const char *name, const char *toml, wc_cap *cap) {
    tmp_mount();
    char vp[256], np[1024];
    snprintf(vp, sizeof vp, "wctmp:/mcheck/%s.toml", name);
    snprintf(np, sizeof np, "%s/mcheck/%s.toml", TMP, name);
    static const char scene[] = "# empty scene\n";
    if (de_dir_create("wctmp:/mcheck") != DE_OK ||
        de_file_write_all_atomic(vp, toml, strlen(toml)) != DE_OK ||
        de_file_write_all_atomic("wctmp:/mcheck/a.scene.toml", scene, sizeof scene - 1) != DE_OK ||
        de_file_write_all_atomic("wctmp:/mcheck/b.png", "png", 3) != DE_OK)
        return -1;
    memset(cap, 0, sizeof *cap);
    const char *argv[] = {np};
    de_cook_io io = {wc_cap_write, cap};
    return de_cook_check_main(1, argv, &io);
}

#define WORLD_ASSETS                                                                               \
    "[assets]\n"                                                                                   \
    "\"levels/a\" = { type = \"scene\", source = \"a.scene.toml\" }\n"                             \
    "\"tex/b\" = { type = \"texture\", source = \"b.png\" }\n"

static void expect_issue(const char *name, const char *toml, const char *needle) {
    wc_cap c;
    int code = check_manifest(name, toml, &c);
    EXPECT_EQ(code, DE_COOK_EXIT_ERRORS);
    if (strstr(c.out, needle) == NULL) {
        printf("  %s: expected \"%s\" in:\n%s", name, needle, c.out);
        EXPECT_TRUE(false);
    }
}

TEST(streaming, world_manifest_check) {
    wc_cap c;
    /* A good world. */
    EXPECT_EQ(check_manifest("good",
                             WORLD_ASSETS "[worlds.w]\nscenes = [\"levels/a\"]\n"
                                          "load_radius = 300.0\nunload_radius = 400.0\n"
                                          "cell_size = 256\nmax_loads_per_frame = 1\n"
                                          "[worlds.w.proxy]\nfade_seconds = 0.5\n",
                             &c),
              DE_COOK_EXIT_OK);
    if (strstr(c.out, "0 errors") == NULL)
        printf("%s", c.out);

    expect_issue("cell_size", WORLD_ASSETS "[worlds.w]\nscenes = [\"levels/a\"]\ncell_size = 128\n",
                 "cell_size must be 256");
    expect_issue("unload_le_load",
                 WORLD_ASSETS "[worlds.w]\nscenes = [\"levels/a\"]\n"
                              "load_radius = 500.0\nunload_radius = 500.0\n",
                 "unload_radius must be greater than load_radius");
    expect_issue("bad_radius",
                 WORLD_ASSETS "[worlds.w]\nscenes = [\"levels/a\"]\nload_radius = -1.0\n",
                 "load_radius is a number");
    expect_issue("no_scenes", WORLD_ASSETS "[worlds.w]\nload_radius = 300.0\n",
                 "needs a non-empty scenes");
    expect_issue("undeclared", WORLD_ASSETS "[worlds.w]\nscenes = [\"levels/zzz\"]\n",
                 "is not declared in [assets]");
    expect_issue("not_scene", WORLD_ASSETS "[worlds.w]\nscenes = [\"tex/b\"]\n",
                 "a world lists scenes");
    expect_issue("bad_name", WORLD_ASSETS "[worlds.Big]\nscenes = [\"levels/a\"]\n",
                 "use lower-case letters");
    expect_issue("cells_dir", WORLD_ASSETS "[worlds.w]\nscenes = [\"levels/a\"]\ncells_dir = \"../x\"\n",
                 "cells_dir is a relative path");
}
