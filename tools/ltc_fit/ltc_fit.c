/*
 * ltc_fit.c: fits the linearly transformed cosines (LTC, Heitz, Dupuy, Hill and Neubelt, "Real-Time
 * Polygonal-Light Shading with Linearly Transformed Cosines", SIGGRAPH 2016) of the engine's
 * isotropic GGX specular lobe and writes src/render/lighting/ltc_data.c (DENG-303). Our own fit,
 * not vendored tables: no third-party data, and the lobe is exactly the one shaders/brdf/ggx.slang
 * evaluates (GGX D, height-correlated Smith G2, alpha = perceptual roughness^2).
 *
 * Build and run (offline; the output is committed, the build never runs this):
 *     cc -O2 -o ltc_fit tools/ltc_fit/ltc_fit.c -lm -lpthread
 *     ./ltc_fit > src/render/lighting/ltc_data.c
 *     clang-format -i src/render/lighting/ltc_data.c
 *
 * Table: LTC_N x LTC_N, indexed by (u, v) = (perceptual roughness, sqrt(1 - cos(theta_v))), each
 * from 0 to 1. Per entry:
 *   - the BRDF (F = 1) x cos(theta_l) of view direction V = (sin t, 0, cos t);
 *   - its integral `norm`, and the split of it by Schlick's weight w = (1 - v.h)^5:
 *     x = integral of f cos (1 - w), y = integral of f cos w, so the specular reflectance of a
 *     Schlick Fresnel (F0, F90) is F0 x + F90 y;
 *   - its average direction Z (in the xz-plane), the frame X = Y x Z, Y = (0, 1, 0);
 *   - M = [X Y Z] diag-shear(m11, m22, m13) fitted with Nelder-Mead to minimise the cubed
 * difference between the normalised lobe and the LTC distribution (Heitz's error estimator: samples
 * of both distributions, weighted by the balance heuristic);
 *   - stored: M^-1 normalised so that its (1, 1) entry is 1 (an LTC is scale invariant): the four
 *     non-trivial entries (row-major) m00, m02, m20, m22, and (x, y).
 * Each roughness row starts at normal incidence from an isotropic guess and continues along
 * theta with the previous fit; rows run on threads.
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LTC_N
#define LTC_N 64
#endif
#define SAMPLES 48 /* per axis: SAMPLES^2 stratified samples per distribution */
#define PI_D    3.14159265358979323846

typedef struct v3 {
    double x, y, z;
} v3;

static v3 vadd(v3 a, v3 b) {
    return (v3){a.x + b.x, a.y + b.y, a.z + b.z};
}
static v3 vscale(v3 a, double s) {
    return (v3){a.x * s, a.y * s, a.z * s};
}
static double vdot(v3 a, v3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static v3 vnorm(v3 a) {
    double l = sqrt(vdot(a, a));
    return l > 0 ? vscale(a, 1.0 / l) : a;
}
static v3 vcross(v3 a, v3 b) {
    return (v3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

typedef struct m3 {
    double m[3][3];
} m3;

static v3 mmul(const m3 *a, v3 v) {
    return (v3){a->m[0][0] * v.x + a->m[0][1] * v.y + a->m[0][2] * v.z,
                a->m[1][0] * v.x + a->m[1][1] * v.y + a->m[1][2] * v.z,
                a->m[2][0] * v.x + a->m[2][1] * v.y + a->m[2][2] * v.z};
}
static m3 mmulm(const m3 *a, const m3 *b) {
    m3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j];
    return r;
}
static double mdet(const m3 *a) {
    const double (*m)[3] = a->m;
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}
static m3 minv(const m3 *a) {
    const double (*m)[3] = a->m;
    const double d = mdet(a);
    m3 r;
    r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / d;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / d;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / d;
    r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / d;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / d;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / d;
    r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / d;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / d;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / d;
    return r;
}

/* ---- The GGX lobe (F = 1): f(v, l) cos(theta_l) ---- */

static double ggx_d(double alpha, double noh) {
    const double a2 = alpha * alpha;
    const double d = noh * noh * (a2 - 1.0) + 1.0;
    return a2 / (PI_D * d * d);
}
static double smith_lambda(double alpha, double c) {
    if (c <= 0.0)
        return 1e30;
    const double t2 = (1.0 - c * c) / (c * c);
    return 0.5 * (-1.0 + sqrt(1.0 + alpha * alpha * t2));
}
/* f cos(theta_l); `pdf` the density of sampling l through D(h) (h-space to l-space). */
static double brdf_eval(double alpha, v3 v, v3 l, double *pdf) {
    *pdf = 0.0;
    if (l.z <= 0.0 || v.z <= 0.0)
        return 0.0;
    const v3 h = vnorm(vadd(v, l));
    const double d = ggx_d(alpha, h.z);
    const double g2 = 1.0 / (1.0 + smith_lambda(alpha, v.z) + smith_lambda(alpha, l.z));
    const double voh = vdot(v, h);
    *pdf = d * h.z / (4.0 * fabs(voh));
    return d * g2 / (4.0 * v.z);
}
/* l from uniform (u1, u2) through D(h). */
static v3 brdf_sample(double alpha, v3 v, double u1, double u2) {
    const double phi = 2.0 * PI_D * u1;
    const double t2 = alpha * alpha * u2 / (1.0 - u2);
    const double ct = 1.0 / sqrt(1.0 + t2), st = sqrt(fmax(0.0, 1.0 - ct * ct));
    const v3 h = {st * cos(phi), st * sin(phi), ct};
    return vadd(vscale(h, 2.0 * vdot(v, h)), vscale(v, -1.0));
}

/* ---- The LTC ---- */

typedef struct ltc {
    m3 m, minv;
    double det_minv;
} ltc;

static void ltc_set(ltc *t, v3 x, v3 y, v3 z, double m11, double m22, double m13) {
    m3 basis = {{{x.x, y.x, z.x}, {x.y, y.y, z.y}, {x.z, y.z, z.z}}};
    m3 local = {{{m11, 0, m13}, {0, m22, 0}, {0, 0, 1}}};
    t->m = mmulm(&basis, &local);
    t->minv = minv(&t->m);
    t->det_minv = fabs(mdet(&t->minv));
}
static double ltc_eval(const ltc *t, v3 l) {
    v3 lo = mmul(&t->minv, l);
    const double len = sqrt(vdot(lo, lo));
    lo = vscale(lo, 1.0 / len);
    const double d0 = fmax(0.0, lo.z) / PI_D;
    return d0 * t->det_minv / (len * len * len);
}
static v3 ltc_sample(const ltc *t, double u1, double u2) {
    const double r = sqrt(u1), phi = 2.0 * PI_D * u2;
    const v3 lo = {r * cos(phi), r * sin(phi), sqrt(fmax(0.0, 1.0 - u1))};
    return vnorm(mmul(&t->m, lo));
}

typedef struct fit_ctx {
    double alpha;
    v3 v, x, y, z;
    double norm;
} fit_ctx;

static double fit_error(const fit_ctx *c, const double p[3]) {
    if (p[0] <= 1e-7 || p[1] <= 1e-7)
        return 1e30;
    ltc t;
    ltc_set(&t, c->x, c->y, c->z, p[0], p[1], p[2]);
    double err = 0.0;
    for (int j = 0; j < SAMPLES; ++j)
        for (int i = 0; i < SAMPLES; ++i) {
            const double u1 = (i + 0.5) / SAMPLES, u2 = (j + 0.5) / SAMPLES;
            /* LTC samples */
            {
                const v3 l = ltc_sample(&t, u1, u2);
                double pdf_b;
                const double fb = brdf_eval(c->alpha, c->v, l, &pdf_b) / c->norm;
                const double fl = ltc_eval(&t, l);
                const double e = fabs(fb - fl);
                err += e * e * e / (fl + pdf_b);
            }
            /* BRDF samples */
            {
                const v3 l = brdf_sample(c->alpha, c->v, u1, u2);
                double pdf_b;
                const double fb = brdf_eval(c->alpha, c->v, l, &pdf_b) / c->norm;
                const double fl = ltc_eval(&t, l);
                const double e = fabs(fb - fl);
                if (pdf_b + fl > 0.0)
                    err += e * e * e / (fl + pdf_b);
            }
        }
    return err / (2.0 * SAMPLES * SAMPLES);
}

/* The L1 distance between the normalised lobe and the fit (0 = equal, 2 = disjoint). */
static double l1_error(const fit_ctx *c, const ltc *t) {
    double err = 0.0;
    for (int j = 0; j < SAMPLES; ++j)
        for (int i = 0; i < SAMPLES; ++i) {
            const double u1 = (i + 0.5) / SAMPLES, u2 = (j + 0.5) / SAMPLES;
            const v3 la = ltc_sample(t, u1, u2), lb = brdf_sample(c->alpha, c->v, u1, u2);
            const v3 ls[2] = {la, lb};
            for (int k = 0; k < 2; ++k) {
                double pdf_b;
                const double fb = brdf_eval(c->alpha, c->v, ls[k], &pdf_b) / c->norm;
                const double fl = ltc_eval(t, ls[k]);
                if (fl + pdf_b > 0.0)
                    err += fabs(fb - fl) / (0.5 * (fl + pdf_b));
            }
        }
    return err / (2.0 * SAMPLES * SAMPLES);
}

/* Nelder-Mead in 3 dimensions. */
static void nelder_mead(const fit_ctx *c, double p[3], double scale) {
    double s[4][3], f[4];
    for (int k = 0; k < 4; ++k) {
        for (int d = 0; d < 3; ++d)
            s[k][d] = p[d];
        if (k > 0)
            s[k][k - 1] += scale * (k - 1 < 2 ? fmax(fabs(p[k - 1]), 0.01) : 0.1);
        f[k] = fit_error(c, s[k]);
    }
    for (int it = 0; it < 400; ++it) {
        /* order */
        for (int a = 0; a < 4; ++a)
            for (int b = a + 1; b < 4; ++b)
                if (f[b] < f[a]) {
                    double t = f[a];
                    f[a] = f[b];
                    f[b] = t;
                    for (int d = 0; d < 3; ++d) {
                        t = s[a][d];
                        s[a][d] = s[b][d];
                        s[b][d] = t;
                    }
                }
        if (fabs(f[3] - f[0]) <= 1e-9 * (fabs(f[0]) + 1e-12) && it > 20)
            break;
        double o[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k)
            for (int d = 0; d < 3; ++d)
                o[d] += s[k][d] / 3.0;
        double r[3], e[3], cc[3];
        for (int d = 0; d < 3; ++d)
            r[d] = o[d] + (o[d] - s[3][d]);
        const double fr = fit_error(c, r);
        if (fr < f[0]) {
            for (int d = 0; d < 3; ++d)
                e[d] = o[d] + 2.0 * (o[d] - s[3][d]);
            const double fe = fit_error(c, e);
            if (fe < fr) {
                memcpy(s[3], e, sizeof e);
                f[3] = fe;
            } else {
                memcpy(s[3], r, sizeof r);
                f[3] = fr;
            }
        } else if (fr < f[2]) {
            memcpy(s[3], r, sizeof r);
            f[3] = fr;
        } else {
            for (int d = 0; d < 3; ++d)
                cc[d] = o[d] + 0.5 * (s[3][d] - o[d]);
            const double fc = fit_error(c, cc);
            if (fc < f[3]) {
                memcpy(s[3], cc, sizeof cc);
                f[3] = fc;
            } else {
                for (int k = 1; k < 4; ++k) {
                    for (int d = 0; d < 3; ++d)
                        s[k][d] = s[0][d] + 0.5 * (s[k][d] - s[0][d]);
                    f[k] = fit_error(c, s[k]);
                }
            }
        }
    }
    int best = 0;
    for (int k = 1; k < 4; ++k)
        if (f[k] < f[best])
            best = k;
    memcpy(p, s[best], sizeof s[best]);
}

static float g_tab1[LTC_N][LTC_N][4];
static float g_tab2[LTC_N][LTC_N][2];

/* The lobe's integral, Schlick split and average direction (stratified BRDF samples). */
static void lobe_terms(double alpha, v3 v, double *norm, double *fx, double *fy, v3 *avg) {
    const int n = 256;
    double sum = 0, sx = 0, sy = 0;
    v3 a = {0, 0, 0};
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const v3 l = brdf_sample(alpha, v, (i + 0.5) / n, (j + 0.5) / n);
            double pdf;
            const double f = brdf_eval(alpha, v, l, &pdf);
            if (pdf <= 0.0 || f <= 0.0)
                continue;
            const double w = f / pdf;
            const v3 h = vnorm(vadd(v, l));
            const double s5 = pow(1.0 - fmax(0.0, vdot(v, h)), 5.0);
            sum += w;
            sx += w * (1.0 - s5);
            sy += w * s5;
            a = vadd(a, vscale(l, w));
        }
    const double inv = 1.0 / ((double)n * n);
    *norm = sum * inv;
    *fx = sx * inv;
    *fy = sy * inv;
    a.y = 0.0;
    *avg = vnorm(a);
}

static void *fit_row(void *arg) {
    const int ri = (int)(size_t)arg;
    const double rough = fmax((double)ri / (LTC_N - 1), 0.0089);
    const double alpha = fmax(rough * rough, 1e-4);
    double p[3] = {1, 1, 0};
    for (int ti = 0; ti < LTC_N; ++ti) {
        const double s = (double)ti / (LTC_N - 1);
        const double ct = fmax(1.0 - s * s, 1e-3), st = sqrt(1.0 - ct * ct);
        fit_ctx c = {.alpha = alpha, .v = {st, 0, ct}};
        double fx, fy;
        v3 avg;
        lobe_terms(alpha, c.v, &c.norm, &fx, &fy, &avg);
        c.z = avg;
        c.y = (v3){0, 1, 0};
        c.x = vcross(c.y, c.z);
        if (ti == 0) {
            /* isotropic start: a cosine lobe squeezed to the GGX width */
            p[0] = p[1] = fmax(alpha, 1e-3);
            p[2] = 0.0;
            nelder_mead(&c, p, 0.5);
            nelder_mead(&c, p, 0.1);
        } else {
            /* continue from the previous fit; also try a fresh isotropic start, keep the better */
            double q[3] = {fmax(alpha, 1e-3), fmax(alpha, 1e-3), 0.0};
            nelder_mead(&c, p, 0.1);
            nelder_mead(&c, p, 0.02);
            nelder_mead(&c, q, 0.5);
            nelder_mead(&c, q, 0.1);
            nelder_mead(&c, q, 0.02);
            if (fit_error(&c, q) < fit_error(&c, p))
                memcpy(p, q, sizeof q);
        }
        ltc t;
        ltc_set(&t, c.x, c.y, c.z, p[0], p[1], p[2]);
        if (getenv("LTC_FIT_REPORT") != NULL)
            fprintf(stderr, "r %.3f s %.3f: L1 %.4f\n", rough, s, l1_error(&c, &t));
        m3 mi = t.minv;
        const double k = mi.m[1][1];
        g_tab1[ri][ti][0] = (float)(mi.m[0][0] / k);
        g_tab1[ri][ti][1] = (float)(mi.m[0][2] / k);
        g_tab1[ri][ti][2] = (float)(mi.m[2][0] / k);
        g_tab1[ri][ti][3] = (float)(mi.m[2][2] / k);
        g_tab2[ri][ti][0] = (float)fx;
        g_tab2[ri][ti][1] = (float)fy;
    }
    fprintf(stderr, "row %d done\n", ri);
    return NULL;
}

/* A float literal that round-trips (9 significant digits, always with a '.' or an exponent). */
static const char *flit(float v, char *buf) {
    snprintf(buf, 40, "%.9g", (double)v);
    if (strchr(buf, '.') == NULL && strchr(buf, 'e') == NULL && strchr(buf, 'n') == NULL &&
        strchr(buf, 'i') == NULL)
        strcat(buf, ".0");
    strcat(buf, "f");
    return buf;
}

int main(void) {
    pthread_t th[LTC_N];
    for (int r = 0; r < LTC_N; ++r)
        pthread_create(&th[r], NULL, fit_row, (void *)(size_t)r);
    for (int r = 0; r < LTC_N; ++r)
        pthread_join(th[r], NULL);
    printf(
        "/*\n * ltc_data.c: the GGX LTC tables of the area lights (DENG-303). GENERATED by\n"
        " * tools/ltc_fit/ltc_fit.c (our own fit of the engine's GGX lobe; see that file for the\n"
        " * method). Do not edit; re-run the fit and replace this file.\n */\n");
    printf("#include \"render/lighting/lighting_inputs.h\"\n\n");
    printf("/* [roughness][sqrt(1 - cos theta)]: M^-1 (normalised, (1,1) = 1) as m00, m02, m20, "
           "m22. */\nconst float de__ltc_tab1[DE__LTC_SIZE][DE__LTC_SIZE][4] = {\n");
    for (int r = 0; r < LTC_N; ++r) {
        printf("    {");
        for (int t = 0; t < LTC_N; ++t) {
            char a[40], b[40], c[40], d[40];
            printf("{%s, %s, %s, %s}%s", flit(g_tab1[r][t][0], a), flit(g_tab1[r][t][1], b),
                   flit(g_tab1[r][t][2], c), flit(g_tab1[r][t][3], d),
                   t + 1 < LTC_N ? ",\n     " : "");
        }
        printf("}%s\n", r + 1 < LTC_N ? "," : "");
    }
    printf("};\n\n/* The specular reflectance split: F0 x + F90 y. */\n"
           "const float de__ltc_tab2[DE__LTC_SIZE][DE__LTC_SIZE][2] = {\n");
    for (int r = 0; r < LTC_N; ++r) {
        printf("    {");
        for (int t = 0; t < LTC_N; ++t) {
            char a[40], b[40];
            printf("{%s, %s}%s", flit(g_tab2[r][t][0], a), flit(g_tab2[r][t][1], b),
                   t + 1 < LTC_N ? ",\n     " : "");
        }
        printf("}%s\n", r + 1 < LTC_N ? "," : "");
    }
    printf("};\n");
    return 0;
}
