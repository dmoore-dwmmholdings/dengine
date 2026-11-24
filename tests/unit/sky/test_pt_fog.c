/*
 * test_pt_fog.c: the path tracer's fog density (DENG-848). The CPU mirror of pt_volume.slang
 * (de__pt_fog_density / de__pt_fog_majorant) against the raster froxel formula
 * (froxel_inject_media.slang fog_volume_density, on the camera-relative pack of fog_volumes.c),
 * the majorant bounding the density, and a homogeneous volume keeping its constant density.
 */
#include "de_test.h"

#include "render/pathtracer/pt_internal.h"
#include "render/world/render_world.h"
#include "world/sky/fog_internal.h"

#include <math.h>
#include <string.h>

static f32 ref_hash(f32 x, f32 y, f32 z) {
    x = x * 0.3183099f + 0.1f;
    y = y * 0.3183099f + 0.1f;
    z = z * 0.3183099f + 0.1f;
    x -= floorf(x);
    y -= floorf(y);
    z -= floorf(z);
    x *= 17.0f;
    y *= 17.0f;
    z *= 17.0f;
    f32 v = x * y * z * (x + y + z);
    return v - floorf(v);
}
static f32 ref_lerp(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}
static f32 ref_noise(f32 px, f32 py, f32 pz) {
    f32 ix = floorf(px), iy = floorf(py), iz = floorf(pz), fx = px - ix, fy = py - iy, fz = pz - iz;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    fz = fz * fz * (3 - 2 * fz);
    f32 a = ref_lerp(ref_lerp(ref_hash(ix, iy, iz), ref_hash(ix + 1, iy, iz), fx),
                     ref_lerp(ref_hash(ix, iy + 1, iz), ref_hash(ix + 1, iy + 1, iz), fx), fy);
    f32 b = ref_lerp(ref_lerp(ref_hash(ix, iy, iz + 1), ref_hash(ix + 1, iy, iz + 1), fx),
                     ref_lerp(ref_hash(ix, iy + 1, iz + 1), ref_hash(ix + 1, iy + 1, iz + 1), fx),
                     fy);
    return ref_lerp(a, b, fz);
}
/* fog_volume_density of froxel_inject_media.slang, line for line, at camera-relative p. */
static f32 raster_density(const de__fog_volume_gpu *v, de_vec3 p) {
    f32 q[3];
    for (u32 r = 0; r < 3; ++r)
        q[r] = v->inv[r][0] * p.x + v->inv[r][1] * p.y + v->inv[r][2] * p.z + v->inv[r][3];
    f32 d = v->extent_density[3];
    if (v->extent_density[0] < 0) {
        f32 e = v->params[0] * (p.z - v->params[3]);
        d *= expf(-fminf(fmaxf(e, -8.0f), 80.0f));
    } else {
        f32 inside = fminf(v->extent_density[0] - fabsf(q[0]),
                           fminf(v->extent_density[1] - fabsf(q[1]),
                                 v->extent_density[2] - fabsf(q[2])));
        if (inside < 0)
            return 0;
        if (v->params[2] > 0) {
            f32 t = fminf(fmaxf(inside / v->params[2], 0.0f), 1.0f);
            d *= t * t * (3 - 2 * t);
        }
    }
    if (v->params[1] > 0)
        d *= ref_lerp(1.0f, 2.0f * ref_noise(q[0] * 0.5f, q[1] * 0.5f, q[2] * 0.5f), v->params[1]);
    return d;
}

static de__fog_snapshot snap(de_vec3 pos, de_vec3 extent, f32 density, f32 falloff, f32 noise,
                             f32 fade, f32 angle) {
    de__fog_snapshot s;
    memset(&s, 0, sizeof s);
    s.entity = 1u;
    s.transform = de_affine_identity();
    s.transform.m[0] = cosf(angle);
    s.transform.m[1] = sinf(angle);
    s.transform.m[3] = -sinf(angle);
    s.transform.m[4] = cosf(angle);
    s.transform.m[9] = pos.x;
    s.transform.m[10] = pos.y;
    s.transform.m[11] = pos.z;
    s.volume.extent = extent;
    s.volume.density = density;
    s.volume.albedo = (de_vec3){0.5f, 0.75f, 0.9f};
    s.volume.height_falloff = falloff;
    s.volume.noise = noise;
    s.volume.edge_fade = fade;
    return s;
}

static u32 rng_state = 12345u;
static f32 rnd(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return (f32)(rng_state >> 8) * (1.0f / 16777216.0f);
}

TEST(pt_fog, density_matches_raster) {
    const de__fog_snapshot src[4] = {
        snap((de_vec3){0, 0, 2}, (de_vec3){0, 0, 0}, 0.2f, 0.35f, 0.0f, 0.0f, 0.0f),
        snap((de_vec3){0, 0, -1}, (de_vec3){0, 0, 0}, 0.1f, 0.15f, 0.8f, 0.0f, 0.0f),
        snap((de_vec3){3, -2, 1}, (de_vec3){8, 6, 4}, 0.3f, 0.0f, 0.6f, 1.5f, 0.5f),
        snap((de_vec3){-4, 5, 0.5f}, (de_vec3){5, 5, 5}, 0.25f, 0.4f, 0.0f, 0.0f, 0.0f),
    };
    const de_vec3 cam = {3.5f, -2.25f, 1.75f};
    de__fog_volume_gpu raster[4];
    ASSERT_EQ(de__fog_pack_volumes((const de__fog_snapshot *)src, 4u, cam, raster, 4u), 4u);
    for (u32 v = 0; v < 4u; ++v) {
        de__pt_fog pt;
        de__pt_fog_pack(&src[v], &pt);
        u32 inside = 0;
        for (u32 i = 0; i < 3000u; ++i) {
            de_vec3 p = {rnd() * 24 - 12, rnd() * 24 - 12, rnd() * 14 - 6};
            f32 want = raster_density(&raster[v], (de_vec3){p.x - cam.x, p.y - cam.y, p.z - cam.z});
            f32 got = de__pt_fog_density(&pt, p);
            EXPECT_FLOAT_EQ(got, want, 2e-4f * fmaxf(1.0f, want));
            inside += want > 0.0f;
        }
        EXPECT_GT(inside, 20u); /* the samples do land inside the region */
    }
}

TEST(pt_fog, majorant_bounds_density) {
    const de__fog_snapshot src[4] = {
        snap((de_vec3){0, 0, 2}, (de_vec3){0, 0, 0}, 0.2f, 0.5f, 0.0f, 0.0f, 0.0f),
        snap((de_vec3){0, 0, -1}, (de_vec3){0, 0, 0}, 0.1f, 0.2f, 1.0f, 0.0f, 0.0f),
        snap((de_vec3){1, 1, 1}, (de_vec3){10, 10, 10}, 0.3f, 0.0f, 1.0f, 2.0f, 0.3f),
        snap((de_vec3){0, 0, 0}, (de_vec3){6, 6, 6}, 0.4f, 0.0f, 0.0f, 1.0f, 0.0f),
    };
    for (u32 v = 0; v < 4u; ++v) {
        de__pt_fog pt;
        de__pt_fog_pack(&src[v], &pt);
        for (u32 i = 0; i < 100u; ++i) {
            de_vec3 o = {rnd() * 10 - 5, rnd() * 10 - 5, rnd() * 10 - 5};
            de_vec3 d = {rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 2 - 1};
            f32 len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z) + 1e-6f;
            d = (de_vec3){d.x / len, d.y / len, d.z / len};
            f32 t0 = rnd() * 3, t1 = t0 + rnd() * 15;
            f32 m = de__pt_fog_majorant(&pt, o, d, t0, t1);
            for (u32 k = 0; k <= 64u; ++k) {
                f32 t = t0 + (t1 - t0) * (f32)k / 64.0f;
                f32 dens = de__pt_fog_density(
                    &pt, (de_vec3){o.x + d.x * t, o.y + d.y * t, o.z + d.z * t});
                EXPECT_TRUE(dens <= m * (1.0f + 1e-5f) + 1e-7f);
            }
        }
    }
}

TEST(pt_fog, homogeneous_is_unchanged) {
    /* A box ignores height_falloff (global fog only); noise 0 and edge_fade 0 add nothing. */
    de__fog_snapshot box = snap((de_vec3){1, 2, 3}, (de_vec3){4, 4, 4}, 0.5f, 0.7f, 0, 0, 0);
    de__pt_fog b;
    de__pt_fog_pack(&box, &b);
    EXPECT_FALSE(de__pt_fog_heterogeneous(&b));
    EXPECT_FLOAT_EQ(de__pt_fog_density(&b, (de_vec3){1, 2, 3}), 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(de__pt_fog_density(&b, (de_vec3){2.9f, 3.9f, 4.9f}), 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(de__pt_fog_density(&b, (de_vec3){3.5f, 2, 3}), 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ(de__pt_fog_majorant(&b, (de_vec3){0, 0, 0}, (de_vec3){1, 0, 0}, 0, 9), 0.5f,
                    1e-6f);
    /* Global fog with no falloff is uniform; with falloff it is not. */
    de__fog_snapshot flat = snap((de_vec3){0, 0, 0}, (de_vec3){0, 0, 0}, 0.1f, 0, 0, 0, 0);
    de__pt_fog g;
    de__pt_fog_pack(&flat, &g);
    EXPECT_FALSE(de__pt_fog_heterogeneous(&g));
    EXPECT_FLOAT_EQ(de__pt_fog_density(&g, (de_vec3){9, -4, 77}), 0.1f, 1e-6f);
    flat.volume.height_falloff = 0.5f;
    de__pt_fog_pack(&flat, &g);
    EXPECT_TRUE(de__pt_fog_heterogeneous(&g));
    EXPECT_TRUE(de__pt_fog_density(&g, (de_vec3){0, 0, 4}) < 0.1f);
    /* The path tracer's cvar fog has zero params: uniform. */
    de__pt_fog cvar;
    memset(&cvar, 0, sizeof cvar);
    cvar.extent_density[0] = -1;
    cvar.extent_density[3] = 0.03f;
    EXPECT_FALSE(de__pt_fog_heterogeneous(&cvar));
    EXPECT_FLOAT_EQ(de__pt_fog_density(&cvar, (de_vec3){5, 6, 7}), 0.03f, 1e-7f);
}
