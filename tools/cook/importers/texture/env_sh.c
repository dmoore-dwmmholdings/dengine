/*
 * env_sh.c: SH9 irradiance of an HDR environment (DENG-100, SPEC §11.6, §12.7): the radiance of
 * a cube projected onto the real SH basis of bands 0 to 2 and convolved with the cosine lobe, plus
 * the mean and largest luminance. The basis and the convention are in
 * render/formats/de_cooked_env.h ("SH9 irradiance").
 *
 * Determinism: one partial sum per (face, row), written by whichever worker runs that row and
 * reduced in row order on the calling thread, so the result does not depend on the worker count.
 * Built with the deterministic float flags (no FMA contraction; tools/cook/CMakeLists.txt).
 */
#include "env_internal.h"

#include <dengine/de_base.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_task.h>

#include <string.h>

#define ENV_SH_PI 3.14159265358979323846

/* The partial sums of one (face, row). */
typedef struct env_sh_partial {
    f64 sh[DE_COOKED_ENV_SH_COUNT][3]; /**< sum of L Y_k w */
    f64 lum;                           /**< sum of luminance w */
    f64 weight;                        /**< sum of w */
    f32 max_lum;
    u32 pad;
} env_sh_partial;

typedef struct env_sh_job {
    const f32 *faces;
    u32 n;
    env_sh_partial *partial; /**< 6 n entries */
} env_sh_job;

static void sh_rows(u32 begin, u32 end, void *user) {
    const env_sh_job *j = (const env_sh_job *)user;
    const u32 n = j->n;
    const f64 h = 2.0 / (f64)n;
    for (u32 i = begin; i < end; ++i) {
        const u32 face = i / n, y = i % n;
        env_sh_partial *p = &j->partial[i];
        memset(p, 0, sizeof *p);
        const f32 *row = j->faces + ((u64)face * n + y) * n * 4u;
        const f64 tc = ((f64)y + 0.5) * h - 1.0;
        for (u32 x = 0; x < n; ++x) {
            const f64 sc = ((f64)x + 0.5) * h - 1.0;
            /* Solid angle of the texel: h^2 / (1 + sc^2 + tc^2)^1.5. */
            const f64 q = 1.0 + sc * sc + tc * tc;
            const f64 len = de_dsqrt64(q);
            const f64 w = h * h / (q * len);
            f32 d[3];
            de_cooked_env_face_direction(face, (f32)sc, (f32)tc, d);
            const f32 inv = (f32)(1.0 / len);
            f32 b[DE_COOKED_ENV_SH_COUNT];
            de_cooked_env_sh9_basis(d[0] * inv, d[1] * inv, d[2] * inv, b);
            const f32 *t = row + (u64)x * 4u;
            for (u32 k = 0; k < DE_COOKED_ENV_SH_COUNT; ++k) {
                const f64 bw = (f64)b[k] * w;
                p->sh[k][0] += (f64)t[0] * bw;
                p->sh[k][1] += (f64)t[1] * bw;
                p->sh[k][2] += (f64)t[2] * bw;
            }
            const f32 lum = de_env_luminance(t);
            p->lum += (f64)lum * w;
            p->weight += w;
            if (lum > p->max_lum)
                p->max_lum = lum;
        }
    }
}

de_result de_env_sh9_project(const de_allocator *a, de_task_system *ts, const f32 *faces, u32 size,
                             de_env_sh9 *out) {
    memset(out, 0, sizeof *out);
    if (faces == NULL || size == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "environment: no cube to project");
    const u32 rows = 6u * size;
    const u64 bytes = (u64)rows * sizeof(env_sh_partial);
    env_sh_partial *partial =
        (env_sh_partial *)DE_ALLOC(a, (size_t)bytes, DE_ALIGNOF(env_sh_partial), DE_MEM_ASSET);
    if (partial == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "environment: out of memory for the SH sums");
    env_sh_job j = {faces, size, partial};
    de_env_for(ts, rows, 1u, sh_rows, &j);

    /* Reduce in row order. */
    f64 sh[DE_COOKED_ENV_SH_COUNT][3];
    memset(sh, 0, sizeof sh);
    f64 lum = 0.0, weight = 0.0;
    f32 max_lum = 0.0f;
    for (u32 i = 0; i < rows; ++i) {
        const env_sh_partial *p = &partial[i];
        for (u32 k = 0; k < DE_COOKED_ENV_SH_COUNT; ++k)
            for (u32 c = 0; c < 3u; ++c)
                sh[k][c] += p->sh[k][c];
        lum += p->lum;
        weight += p->weight;
        if (p->max_lum > max_lum)
            max_lum = p->max_lum;
    }
    DE_FREE(a, partial, (size_t)bytes);

    /* The weights are the midpoint rule of the solid angle: scale them to sum to 4 pi, then
     * convolve with the clamped cosine (A_0 = pi, A_1 = 2 pi / 3, A_2 = pi / 4). */
    const f64 norm = weight > 0.0 ? 4.0 * ENV_SH_PI / weight : 0.0;
    static const f64 k_band[DE_COOKED_ENV_SH_COUNT] = {
        ENV_SH_PI,       2.0 * ENV_SH_PI / 3.0, 2.0 * ENV_SH_PI / 3.0, 2.0 * ENV_SH_PI / 3.0,
        ENV_SH_PI / 4.0, ENV_SH_PI / 4.0,       ENV_SH_PI / 4.0,       ENV_SH_PI / 4.0,
        ENV_SH_PI / 4.0};
    for (u32 k = 0; k < DE_COOKED_ENV_SH_COUNT; ++k)
        for (u32 c = 0; c < 3u; ++c)
            out->irradiance[k][c] = (f32)(sh[k][c] * norm * k_band[k]);
    out->mean_luminance = weight > 0.0 ? (f32)(lum / weight) : 0.0f;
    out->max_luminance = max_lum;
    return DE_OK;
}
