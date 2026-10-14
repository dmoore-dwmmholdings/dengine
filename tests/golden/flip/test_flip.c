/* test_flip.c — the LDR-FLIP implementation against NVIDIA's official one (DENG-329).
 *
 *   flip.reference_pairs     every pair in data/reference.txt: the mean error within 1e-3 of the
 *                            official tool's (the acceptance bound); p99 and max within 2e-2.
 *   flip.identical_is_zero   an image against itself: every pixel 0.
 *   flip.symmetry_bounds     flip(a, b) == flip(b, a) per pixel, and every value in [0, 1].
 *   flip.default_ppd         the default viewing setup gives about 67.02 pixels per degree.
 *   flip.stats_and_heatmap   mean / p99 / max on a known list; heat map ends black and yellow.
 *   flip.rejects_bad_input   size or channel mismatch, NULL pixels.
 *
 * data/ was written once by data/make_reference.py with the official `flip-evaluator` 1.7.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* fopen, sscanf */
#endif
#include "de_test.h"

#include "flip.h"

#include <stb_image.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_FLIP_TEST_DATA_DIR
#error "DE_FLIP_TEST_DATA_DIR must name tests/golden/flip/data"
#endif

typedef struct img {
    unsigned char *px;
    int w, h;
} img;

static bool load(const char *name, img *out) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", DE_FLIP_TEST_DATA_DIR, name);
    int comp = 0;
    out->px = stbi_load(path, &out->w, &out->h, &comp, 3);
    if (out->px == NULL)
        printf("cannot load %s: %s\n", path, stbi_failure_reason());
    return out->px != NULL;
}

static de_flip_image view(const img *i) {
    de_flip_image v = {i->px, (uint32_t)i->w, (uint32_t)i->h, 3, 0};
    return v;
}

TEST(flip, reference_pairs) {
    char path[1024];
    snprintf(path, sizeof path, "%s/reference.txt", DE_FLIP_TEST_DATA_DIR);
    FILE *f = fopen(path, "r");
    ASSERT_TRUE(f != NULL);
    char line[512];
    int pairs = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        char name[128], rn[128], tn[128];
        double ppd = 0, mean = 0, p99 = 0, max = 0;
        int n = sscanf(line, "%127s %127s %127s %lf %lf %lf %lf", name, rn, tn, &ppd, &mean, &p99,
                       &max);
        ASSERT_EQ(n, 7);
        img r = {0}, t = {0};
        ASSERT_TRUE(load(rn, &r));
        ASSERT_TRUE(load(tn, &t));
        ASSERT_EQ(r.w, t.w);
        ASSERT_EQ(r.h, t.h);
        const size_t count = (size_t)r.w * (size_t)r.h;
        float *err = (float *)malloc(count * sizeof(float));
        ASSERT_TRUE(err != NULL);
        const de_flip_image rv = view(&r), tv = view(&t);
        ASSERT_TRUE(de_flip_ldr(&rv, &tv, (float)ppd, err));
        de_flip_stats st;
        ASSERT_TRUE(de_flip_stats_compute(err, count, &st));
        printf("%-22s mean %.6f (official %.6f, diff %.2e)  p99 %.4f (%.4f)  max %.4f (%.4f)\n",
               name, st.mean, mean, fabs(st.mean - mean), st.p99, p99, st.max, max);
        EXPECT_FLOAT_EQ(st.mean, mean, 1e-3);
        EXPECT_FLOAT_EQ(st.p99, p99, 2e-2);
        EXPECT_FLOAT_EQ(st.max, max, 2e-2);
        free(err);
        stbi_image_free(r.px);
        stbi_image_free(t.px);
        ++pairs;
    }
    fclose(f);
    EXPECT_GE(pairs, 6);
}

TEST(flip, identical_is_zero) {
    img a = {0};
    ASSERT_TRUE(load("shapes.png", &a));
    const size_t count = (size_t)a.w * (size_t)a.h;
    float *err = (float *)malloc(count * sizeof(float));
    ASSERT_TRUE(err != NULL);
    const de_flip_image v = view(&a);
    ASSERT_TRUE(de_flip_ldr(&v, &v, 0.0f, err));
    size_t nonzero = 0;
    for (size_t i = 0; i < count; ++i)
        nonzero += err[i] != 0.0f;
    EXPECT_EQ(nonzero, (size_t)0);
    free(err);
    stbi_image_free(a.px);
}

TEST(flip, symmetry_bounds) {
    img a = {0}, b = {0};
    ASSERT_TRUE(load("shapes.png", &a));
    ASSERT_TRUE(load("shapes_noise_ppd30_test.png", &b));
    const size_t count = (size_t)a.w * (size_t)a.h;
    float *ab = (float *)malloc(count * sizeof(float));
    float *ba = (float *)malloc(count * sizeof(float));
    ASSERT_TRUE(ab != NULL && ba != NULL);
    const de_flip_image av = view(&a), bv = view(&b);
    ASSERT_TRUE(de_flip_ldr(&av, &bv, 0.0f, ab));
    ASSERT_TRUE(de_flip_ldr(&bv, &av, 0.0f, ba));
    size_t asym = 0, out = 0;
    for (size_t i = 0; i < count; ++i) {
        asym += fabsf(ab[i] - ba[i]) > 1e-6f;
        out += !(ab[i] >= 0.0f && ab[i] <= 1.0f);
    }
    EXPECT_EQ(asym, (size_t)0);
    EXPECT_EQ(out, (size_t)0);
    free(ab);
    free(ba);
    stbi_image_free(a.px);
    stbi_image_free(b.px);
}

TEST(flip, default_ppd) {
    EXPECT_FLOAT_EQ(de_flip_default_ppd(), 67.0206f, 1e-3);
    EXPECT_FLOAT_EQ(de_flip_ppd(1.0f, 1000.0f, 1.0f), 1000.0f * 3.14159265f / 180.0f, 1e-3);
}

TEST(flip, stats_and_heatmap) {
    float v[200];
    for (int i = 0; i < 200; ++i)
        v[i] = (float)(199 - i) / 199.0f; /* 1 .. 0, unsorted for the sort */
    de_flip_stats st;
    ASSERT_TRUE(de_flip_stats_compute(v, 200, &st));
    EXPECT_FLOAT_EQ(st.mean, 0.5, 1e-6);
    EXPECT_FLOAT_EQ(st.p99, 197.0f / 199.0f, 1e-6); /* rank ceil(198) = 198: index 197 */
    EXPECT_FLOAT_EQ(st.max, 1.0f, 0);
    EXPECT_FALSE(de_flip_stats_compute(v, 0, &st));

    const float e[2] = {0.0f, 1.0f};
    unsigned char rgba[8];
    de_flip_heatmap(e, 2, 1, rgba);
    EXPECT_LE(rgba[0] + rgba[1] + rgba[2], 3); /* 0: black */
    EXPECT_GE(rgba[4], 240);                   /* 1: light yellow */
    EXPECT_GE(rgba[5], 230);
    EXPECT_GE(rgba[6], 150);
    EXPECT_EQ(rgba[3], 255);
    EXPECT_EQ(rgba[7], 255);
}

TEST(flip, rejects_bad_input) {
    unsigned char a[4 * 4 * 4] = {0}, b[4 * 4 * 3] = {0};
    float err[16];
    de_flip_image av = {a, 4, 4, 4, 0}, bv = {b, 4, 4, 3, 0};
    EXPECT_TRUE(de_flip_ldr(&av, &bv, 0.0f, err)); /* RGBA against RGB is fine */
    bv.width = 3;
    EXPECT_FALSE(de_flip_ldr(&av, &bv, 0.0f, err));
    bv.width = 4;
    bv.channels = 2;
    EXPECT_FALSE(de_flip_ldr(&av, &bv, 0.0f, err));
    bv.channels = 3;
    bv.pixels = NULL;
    EXPECT_FALSE(de_flip_ldr(&av, &bv, 0.0f, err));
}
