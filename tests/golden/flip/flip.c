/* flip.c — LDR-FLIP (DENG-329). See flip.h for the algorithm and how it is validated.
 *
 * The constants are the published ones (paper and official implementation): the CSF Gaussians
 * per YyCxCz channel, the color exponent qc 0.7, the error redistribution pc 0.4 / pt 0.95, the
 * feature filter width w 0.082 degrees and the feature exponent qf 0.5. The color transforms use
 * the sRGB primaries and the D65 white of linear RGB (1, 1, 1).
 */
#include "flip.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FLIP_PI 3.14159265358979323846

/* ---------- Constants ---------- */

/* Contrast sensitivity functions, per channel (Yy, Cx, Cz): a1, b1, a2, b2 of
 * a1 sqrt(pi / b1) exp(-pi^2 x^2 / b1) + a2 sqrt(pi / b2) exp(-pi^2 x^2 / b2), x in degrees. */
static const float k_csf_a1[3] = {1.0f, 1.0f, 34.1f};
static const float k_csf_b1[3] = {0.0047f, 0.0053f, 0.04f};
static const float k_csf_a2[3] = {0.0f, 0.0f, 13.5f};
static const float k_csf_b2[3] = {1e-5f, 1e-5f, 0.025f};

static const float k_qc = 0.7f;   /* color difference exponent */
static const float k_pc = 0.4f;   /* error redistribution: the first pc of the range ... */
static const float k_pt = 0.95f;  /* ... maps to [0, pt] */
static const float k_gw = 0.082f; /* feature filter width, degrees */
static const float k_qf = 0.5f;   /* feature difference exponent */

/* Linear sRGB <-> XYZ (the exact rational form of the sRGB matrix). */
static const float k_rgb2xyz[9] = {
    10135552.0f / 24577794.0f, 8788810.0f / 24577794.0f, 4435075.0f / 24577794.0f,
    2613072.0f / 12288897.0f,  8788810.0f / 12288897.0f, 887015.0f / 12288897.0f,
    1425312.0f / 73733382.0f,  8788810.0f / 73733382.0f, 70074185.0f / 73733382.0f,
};
static const float k_xyz2rgb[9] = {
    3.241003275f, -1.537398934f, -0.498615861f, -0.969224334f, 1.875930071f,
    0.041554224f, 0.055639423f,  -0.204011202f, 1.057148933f,
};

/* ---------- Color transforms ---------- */

typedef struct v3 {
    float x, y, z;
} v3;

static v3 mul3(const float m[9], v3 c) {
    v3 r = {m[0] * c.x + m[1] * c.y + m[2] * c.z, m[3] * c.x + m[4] * c.y + m[5] * c.z,
            m[6] * c.x + m[7] * c.y + m[8] * c.z};
    return r;
}

/* The reference white: XYZ of linear RGB (1, 1, 1). */
static v3 white(void) {
    v3 one = {1.0f, 1.0f, 1.0f};
    return mul3(k_rgb2xyz, one);
}

static float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

/* XYZ -> YyCxCz: CIELAB without the cube root (linear in XYZ). */
static v3 xyz_to_ycxcz(v3 c, v3 w) {
    const float x = c.x / w.x, y = c.y / w.y, z = c.z / w.z;
    v3 r = {116.0f * y - 16.0f, 500.0f * (x - y), 200.0f * (y - z)};
    return r;
}

static v3 ycxcz_to_xyz(v3 c, v3 w) {
    const float y = (c.x + 16.0f) / 116.0f;
    const float x = c.y / 500.0f + y;
    const float z = y - c.z / 200.0f;
    v3 r = {x * w.x, y * w.y, z * w.z};
    return r;
}

static float lab_f(float t) {
    const float delta = 6.0f / 29.0f;
    return t > delta * delta * delta ? cbrtf(t) : t / (3.0f * delta * delta) + 4.0f / 29.0f;
}

static v3 xyz_to_lab(v3 c, v3 w) {
    const float fx = lab_f(c.x / w.x), fy = lab_f(c.y / w.y), fz = lab_f(c.z / w.z);
    v3 r = {116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
    return r;
}

/* The Hunt effect: chroma shrinks with lightness. */
static v3 hunt(v3 lab) {
    v3 r = {lab.x, 0.01f * lab.x * lab.y, 0.01f * lab.x * lab.z};
    return r;
}

static float hyab(v3 a, v3 b) {
    const float da = a.y - b.y, db = a.z - b.z;
    return fabsf(a.x - b.x) + sqrtf(da * da + db * db);
}

static float clamp01(float v) {
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* ---------- Filters ---------- */

/* Spatial (CSF) filter radius in pixels: three standard deviations of the widest Gaussian. */
static int csf_radius(float ppd) {
    float b = 0.0f;
    for (int i = 0; i < 3; ++i) {
        b = k_csf_b1[i] > b ? k_csf_b1[i] : b;
        b = k_csf_b2[i] > b ? k_csf_b2[i] : b;
    }
    return (int)ceilf(3.0f * sqrtf(b / (2.0f * (float)(FLIP_PI * FLIP_PI))) * ppd);
}

/* 1D CSF kernels of width 2r+1: [0] Yy, [1] Cx, [2] and [3] the two Gaussians of Cz. The 2D
 * kernel of Yy and Cx is k(x) k(y); that of Cz is k2(x) k2(y) + k3(x) k3(y), whose 1D factors
 * carry sqrt(a sqrt(pi / b)) so the products have the CSF's weights. Each 2D kernel sums to 1. */
static void csf_kernels(float ppd, int r, float *k[4]) {
    const float pi2 = (float)(FLIP_PI * FLIP_PI);
    const float dx = 1.0f / ppd;
    double sum[4] = {0, 0, 0, 0};
    for (int i = -r; i <= r; ++i) {
        const float x2 = ((float)i * dx) * ((float)i * dx);
        float v[4];
        v[0] = k_csf_a1[0] * expf(-pi2 * x2 / k_csf_b1[0]);
        v[1] = k_csf_a1[1] * expf(-pi2 * x2 / k_csf_b1[1]);
        v[2] = sqrtf(k_csf_a1[2] * sqrtf((float)FLIP_PI / k_csf_b1[2])) *
               expf(-pi2 * x2 / k_csf_b1[2]);
        v[3] = sqrtf(k_csf_a2[2] * sqrtf((float)FLIP_PI / k_csf_b2[2])) *
               expf(-pi2 * x2 / k_csf_b2[2]);
        for (int c = 0; c < 4; ++c) {
            k[c][i + r] = v[c];
            sum[c] += v[c];
        }
    }
    const float n0 = (float)(1.0 / sum[0]), n1 = (float)(1.0 / sum[1]);
    const float ncz = (float)(1.0 / sqrt(sum[2] * sum[2] + sum[3] * sum[3]));
    for (int i = 0; i < 2 * r + 1; ++i) {
        k[0][i] *= n0;
        k[1][i] *= n1;
        k[2][i] *= ncz;
        k[3][i] *= ncz;
    }
}

/* Feature filters of width 2r+1: [0] Gaussian (sums to 1), [1] its first derivative and [2] its
 * second derivative (positive weights sum to 1, negative ones to -1). */
static int feature_radius(float ppd) {
    return (int)ceilf(3.0f * 0.5f * k_gw * ppd);
}

static void feature_kernels(float ppd, int r, float *k[3]) {
    const float sd = 0.5f * k_gw * ppd;
    double g_sum = 0.0, d_pos = 0.0, d_neg = 0.0, dd_pos = 0.0, dd_neg = 0.0;
    for (int i = -r; i <= r; ++i) {
        const float x = (float)i;
        const float g = expf(-(x * x) / (2.0f * sd * sd));
        const float d = -x * g;
        const float dd = (x * x / (sd * sd) - 1.0f) * g;
        k[0][i + r] = g;
        k[1][i + r] = d;
        k[2][i + r] = dd;
        g_sum += g;
        if (d > 0.0f)
            d_pos += d;
        else
            d_neg -= d;
        if (dd > 0.0f)
            dd_pos += dd;
        else
            dd_neg -= dd;
    }
    for (int i = 0; i < 2 * r + 1; ++i) {
        k[0][i] = (float)(k[0][i] / g_sum);
        k[1][i] = (float)(k[1][i] / (k[1][i] > 0.0f ? d_pos : d_neg));
        k[2][i] = (float)(k[2][i] / (k[2][i] > 0.0f ? dd_pos : dd_neg));
    }
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/* ---------- One image: YyCxCz, filtered color (Hunt-adjusted Lab), features ---------- */

typedef struct scratch {
    int w, h;
    float *ycc[3]; /* YyCxCz planes of the input */
    float *tmp[4]; /* horizontal pass results */
    float *lab[3]; /* filtered color, Hunt-adjusted CIELAB */
    float *edge;   /* edge magnitude */
    float *point;  /* point magnitude */
} scratch;

static void to_ycxcz(const de_flip_image *img, scratch *s, v3 w) {
    const size_t pitch = img->pitch ? img->pitch : (size_t)img->width * img->channels;
    for (int y = 0; y < s->h; ++y) {
        const uint8_t *row = img->pixels + (size_t)y * pitch;
        for (int x = 0; x < s->w; ++x) {
            const uint8_t *p = row + (size_t)x * img->channels;
            v3 lin = {srgb_to_linear((float)p[0] / 255.0f), srgb_to_linear((float)p[1] / 255.0f),
                      srgb_to_linear((float)p[2] / 255.0f)};
            v3 c = xyz_to_ycxcz(mul3(k_rgb2xyz, lin), w);
            const size_t i = (size_t)y * (size_t)s->w + (size_t)x;
            s->ycc[0][i] = c.x;
            s->ycc[1][i] = c.y;
            s->ycc[2][i] = c.z;
        }
    }
}

/* Spatial filtering of the YyCxCz planes, then to Hunt-adjusted CIELAB. */
static void color_pipeline(scratch *s, float *const csf[4], int r, v3 w) {
    const int W = s->w, H = s->h;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float a[4] = {0, 0, 0, 0};
            for (int i = -r; i <= r; ++i) {
                const size_t j = (size_t)y * (size_t)W + (size_t)clampi(x + i, 0, W - 1);
                a[0] += csf[0][i + r] * s->ycc[0][j];
                a[1] += csf[1][i + r] * s->ycc[1][j];
                a[2] += csf[2][i + r] * s->ycc[2][j];
                a[3] += csf[3][i + r] * s->ycc[2][j];
            }
            const size_t o = (size_t)y * (size_t)W + (size_t)x;
            for (int c = 0; c < 4; ++c)
                s->tmp[c][o] = a[c];
        }
    }
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            v3 c = {0, 0, 0};
            for (int i = -r; i <= r; ++i) {
                const size_t j = (size_t)clampi(y + i, 0, H - 1) * (size_t)W + (size_t)x;
                c.x += csf[0][i + r] * s->tmp[0][j];
                c.y += csf[1][i + r] * s->tmp[1][j];
                c.z += csf[2][i + r] * s->tmp[2][j] + csf[3][i + r] * s->tmp[3][j];
            }
            v3 rgb = mul3(k_xyz2rgb, ycxcz_to_xyz(c, w));
            rgb.x = clamp01(rgb.x);
            rgb.y = clamp01(rgb.y);
            rgb.z = clamp01(rgb.z);
            v3 lab = hunt(xyz_to_lab(mul3(k_rgb2xyz, rgb), w));
            const size_t o = (size_t)y * (size_t)W + (size_t)x;
            s->lab[0][o] = lab.x;
            s->lab[1][o] = lab.y;
            s->lab[2][o] = lab.z;
        }
    }
}

/* Edge and point magnitudes of the normalized luminance (Yy + 16) / 116 of the input. */
static void feature_pipeline(scratch *s, float *const fk[3], int r) {
    const int W = s->w, H = s->h;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float a[3] = {0, 0, 0};
            for (int i = -r; i <= r; ++i) {
                const size_t j = (size_t)y * (size_t)W + (size_t)clampi(x + i, 0, W - 1);
                const float l = (s->ycc[0][j] + 16.0f) / 116.0f;
                a[0] += fk[0][i + r] * l;
                a[1] += fk[1][i + r] * l;
                a[2] += fk[2][i + r] * l;
            }
            const size_t o = (size_t)y * (size_t)W + (size_t)x;
            s->tmp[0][o] = a[0]; /* Gaussian along x */
            s->tmp[1][o] = a[1]; /* first derivative along x */
            s->tmp[2][o] = a[2]; /* second derivative along x */
        }
    }
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float ex = 0, ey = 0, px = 0, py = 0;
            for (int i = -r; i <= r; ++i) {
                const size_t j = (size_t)clampi(y + i, 0, H - 1) * (size_t)W + (size_t)x;
                ex += fk[0][i + r] * s->tmp[1][j];
                ey += fk[1][i + r] * s->tmp[0][j];
                px += fk[0][i + r] * s->tmp[2][j];
                py += fk[2][i + r] * s->tmp[0][j];
            }
            const size_t o = (size_t)y * (size_t)W + (size_t)x;
            s->edge[o] = sqrtf(ex * ex + ey * ey);
            s->point[o] = sqrtf(px * px + py * py);
        }
    }
}

static void scratch_free(scratch *s) {
    for (int c = 0; c < 3; ++c) {
        free(s->ycc[c]);
        free(s->lab[c]);
    }
    for (int c = 0; c < 4; ++c)
        free(s->tmp[c]);
    free(s->edge);
    free(s->point);
    memset(s, 0, sizeof *s);
}

static bool scratch_alloc(scratch *s, int w, int h) {
    memset(s, 0, sizeof *s);
    s->w = w;
    s->h = h;
    const size_t n = (size_t)w * (size_t)h * sizeof(float);
    bool ok = true;
    for (int c = 0; c < 3; ++c) {
        ok = ok && (s->ycc[c] = (float *)malloc(n)) != NULL;
        ok = ok && (s->lab[c] = (float *)malloc(n)) != NULL;
    }
    for (int c = 0; c < 4; ++c)
        ok = ok && (s->tmp[c] = (float *)malloc(n)) != NULL;
    ok = ok && (s->edge = (float *)malloc(n)) != NULL;
    ok = ok && (s->point = (float *)malloc(n)) != NULL;
    if (!ok)
        scratch_free(s);
    return ok;
}

/* ---------- API ---------- */

float de_flip_ppd(float distance_m, float width_px, float width_m) {
    return distance_m * (width_px / width_m) * (float)(FLIP_PI / 180.0);
}

float de_flip_default_ppd(void) {
    return de_flip_ppd(DE_FLIP_DEFAULT_DISTANCE_M, DE_FLIP_DEFAULT_WIDTH_PX,
                       DE_FLIP_DEFAULT_WIDTH_M);
}

static bool valid_image(const de_flip_image *img) {
    return img != NULL && img->pixels != NULL && img->width > 0 && img->height > 0 &&
           (img->channels == 3 || img->channels == 4) && img->width <= 65536u &&
           img->height <= 65536u;
}

bool de_flip_ldr(const de_flip_image *ref, const de_flip_image *test, float ppd, float *errors) {
    if (!valid_image(ref) || !valid_image(test) || errors == NULL || ref->width != test->width ||
        ref->height != test->height)
        return false;
    if (!(ppd > 0.0f))
        ppd = de_flip_default_ppd();
    const int W = (int)ref->width, H = (int)ref->height;
    const int rc = csf_radius(ppd), rf = feature_radius(ppd);
    float *kern = (float *)malloc(sizeof(float) * (size_t)(4 * (2 * rc + 1) + 3 * (2 * rf + 1)));
    scratch s[2];
    if (kern == NULL)
        return false;
    if (!scratch_alloc(&s[0], W, H)) {
        free(kern);
        return false;
    }
    if (!scratch_alloc(&s[1], W, H)) {
        scratch_free(&s[0]);
        free(kern);
        return false;
    }
    float *csf[4], *fk[3];
    for (int c = 0; c < 4; ++c)
        csf[c] = kern + c * (2 * rc + 1);
    for (int c = 0; c < 3; ++c)
        fk[c] = kern + 4 * (2 * rc + 1) + c * (2 * rf + 1);
    csf_kernels(ppd, rc, csf);
    feature_kernels(ppd, rf, fk);

    const v3 w = white();
    to_ycxcz(ref, &s[0], w);
    to_ycxcz(test, &s[1], w);
    for (int i = 0; i < 2; ++i) {
        color_pipeline(&s[i], csf, rc, w);
        feature_pipeline(&s[i], fk, rf);
    }

    /* The largest color error: Hunt-adjusted HyAB of pure green and pure blue, ^qc. */
    const v3 green = {0.0f, 1.0f, 0.0f}, blue = {0.0f, 0.0f, 1.0f};
    const float cmax = powf(hyab(hunt(xyz_to_lab(mul3(k_rgb2xyz, green), w)),
                                 hunt(xyz_to_lab(mul3(k_rgb2xyz, blue), w))),
                            k_qc);
    const float pccmax = k_pc * cmax;
    const float inv_sqrt2 = 1.0f / sqrtf(2.0f);
    const size_t n = (size_t)W * (size_t)H;
    for (size_t i = 0; i < n; ++i) {
        v3 a = {s[0].lab[0][i], s[0].lab[1][i], s[0].lab[2][i]};
        v3 b = {s[1].lab[0][i], s[1].lab[1][i], s[1].lab[2][i]};
        float dc = powf(hyab(a, b), k_qc);
        if (dc < pccmax)
            dc *= k_pt / pccmax;
        else
            dc = k_pt + ((dc - pccmax) / (cmax - pccmax)) * (1.0f - k_pt);
        const float de = fabsf(s[0].edge[i] - s[1].edge[i]);
        const float dp = fabsf(s[0].point[i] - s[1].point[i]);
        const float df = powf(inv_sqrt2 * (de > dp ? de : dp), k_qf);
        errors[i] = powf(dc, 1.0f - df);
    }
    scratch_free(&s[0]);
    scratch_free(&s[1]);
    free(kern);
    return true;
}

static int cmp_float(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

bool de_flip_stats_compute(const float *errors, size_t count, de_flip_stats *out) {
    memset(out, 0, sizeof *out);
    if (errors == NULL || count == 0)
        return false;
    float *sorted = (float *)malloc(count * sizeof(float));
    if (sorted == NULL)
        return false;
    memcpy(sorted, errors, count * sizeof(float));
    qsort(sorted, count, sizeof(float), cmp_float);
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i)
        sum += errors[i];
    out->mean = sum / (double)count;
    size_t rank = (size_t)ceil(0.99 * (double)count);
    out->p99 = sorted[rank > 0 ? rank - 1 : 0];
    out->max = sorted[count - 1];
    free(sorted);
    return true;
}

/* Magma as a degree-6 polynomial fit per channel (close to matplotlib's table). */
void de_flip_heatmap(const float *errors, uint32_t width, uint32_t height, uint8_t *rgba) {
    static const float c[7][3] = {
        {-0.002136485053939582f, -0.000749655052795221f, -0.005386127855323933f},
        {0.2516605407371642f, 0.6775232436837668f, 2.494026599312351f},
        {8.353717279216625f, -3.577719514958484f, 0.3144679030132573f},
        {-27.66873308576866f, 14.26473078096533f, -13.64921318813922f},
        {52.17613981234068f, -27.94360607168351f, 12.94416944238394f},
        {-50.76852536473588f, 29.04658282127291f, 4.23415299384598f},
        {18.65570506591883f, -11.48977351997711f, -5.601961508734096f},
    };
    const size_t n = (size_t)width * (size_t)height;
    for (size_t i = 0; i < n; ++i) {
        const float t = clamp01(errors[i]);
        for (int ch = 0; ch < 3; ++ch) {
            float v = c[6][ch];
            for (int k = 5; k >= 0; --k)
                v = c[k][ch] + t * v;
            rgba[i * 4u + (size_t)ch] = (uint8_t)(clamp01(v) * 255.0f + 0.5f);
        }
        rgba[i * 4u + 3u] = 255;
    }
}
