/*
 * test_mesh_lod.c: the LOD chain, the quantization and the DMSH v2 blob of the mesh stage
 * (DENG-103, SPEC §11.6, §11.8): LOD monotonicity and the error metric (reproduced here with
 * meshoptimizer directly), position / normal / UV / weight quantization and its f32 fallbacks,
 * section alignment, determinism and the settings. CTest names import.mesh.<test>.
 */
#include "mesh_fixtures.h"

#include <float.h>
#include <meshoptimizer.h>
#include <stdio.h>

#define MOUNT "mlod"

typedef mesh_fx mlod_fx;

static void mlod_fx_setup(mlod_fx *f) {
    mesh_fx_setup(f, MOUNT);
}

static void mlod_fx_teardown(mlod_fx *f) {
    mesh_fx_teardown(f);
}

static void parse_sub(const de_cook_result *r, const char *name, de_cooked_mesh_view *v) {
    const de_cook_subasset *s = de_cook_result_subasset(r, name);
    ASSERT_TRUE(s != NULL);
    const char *why = "";
    bool ok = de_cooked_mesh_parse(s->data, s->size, v, &why);
    if (!ok)
        printf("  parse failed: %s\n", why);
    ASSERT_TRUE(ok);
}

/* ---- import.mesh.lod_monotonic --------------------------------------------------------------- */

TEST_F(mesh, lod_monotonic, mlod_fx) {
    /* The bunny-class fixture: about 51k triangles. */
    de_model_mesh *m = mesh_fx_bunny(f, 160, 160, false, 1.0);
    EXPECT_EQ(m->index_count / 3u, 2u * 160u * 159u);
    de_model_lods lods;
    ASSERT_EQ(de_model_lods_build(&f->ir, m, true, &lods), DE_OK);
    printf("  LODs:");
    for (u32 k = 0; k < lods.count; ++k)
        printf(" %u (%.6f)", lods.lod[k].index_count / 3u, (double)lods.lod[k].error);
    printf("\n");
    EXPECT_GE(lods.count, 6u);
    EXPECT_LE(lods.count, DE_COOKED_MESH_MAX_LODS);
    EXPECT_FLOAT_EQ(lods.lod[0].error, 0.0f, 0.0f);
    for (u32 k = 1; k < lods.count; ++k) {
        u32 t = lods.lod[k].index_count / 3u, p = lods.lod[k - 1].index_count / 3u;
        EXPECT_LT(t, p);
        EXPECT_TRUE((f64)t <= DE_MODEL_LOD_KEEP_RATIO * (f64)p);
        EXPECT_GE(p, DE_MODEL_LOD_MIN_TRIANGLES);
        EXPECT_TRUE(lods.lod[k].error >= lods.lod[k - 1].error);
        EXPECT_TRUE(lods.lod[k].error > 0.0f);
        /* The bumps are about 0.1 of the size: no LOD drifts further than the shape itself. */
        EXPECT_TRUE(lods.lod[k].error < 0.5f);
    }
    /* The chain ends for a reason: 8 LODs, under the minimum, or too little reduction. */
    u32 last = lods.lod[lods.count - 1u].index_count / 3u;
    EXPECT_TRUE(lods.count == DE_COOKED_MESH_MAX_LODS || last < 4u * DE_MODEL_LOD_MIN_TRIANGLES);

    /* The cooked LOD table says the same, and each LOD needs only a prefix of the vertices. */
    de_model_mesh_settings s = mesh_default_settings();
    de_model_buf b;
    de_cooked_mesh_view v;
    mesh_fx_cook_mesh(f, m, &s, &b, &v);
    ASSERT_EQ(v.lod_count, lods.count);
    for (u32 k = 0; k < v.lod_count; ++k) {
        de_cooked_mesh_lod l = de_cooked_mesh_get_lod(&v, k);
        EXPECT_EQ(l.triangle_count, lods.lod[k].index_count / 3u);
        EXPECT_FLOAT_EQ(l.error, lods.lod[k].error, 0.0f);
        u32 used = 0;
        for (u32 i = 0; i < l.index_count; ++i) {
            u32 x = de_cooked_mesh_index(&v, l.index_first + i);
            used = x + 1u > used ? x + 1u : used;
        }
        EXPECT_EQ(l.vertex_count, used);
    }
    EXPECT_EQ(de_cooked_mesh_get_lod(&v, 0).vertex_count, v.vertex_count);
    EXPECT_LT(de_cooked_mesh_get_lod(&v, v.lod_count - 1u).vertex_count, v.vertex_count / 8u);
    EXPECT_FLOAT_EQ(v.lod_error, 0.01f, 0.0f);

    /* generate_lods = false: LOD 0 only. */
    s.generate_lods = false;
    mesh_fx_cook_mesh(f, m, &s, &b, &v);
    EXPECT_EQ(v.lod_count, 1u);

    /* Duck through the importer: a chain with the default settings. */
    mesh_fx_cooker(f);
    de_cook_result r;
    ASSERT_EQ(mesh_fx_cook(f, MOUNT, "props/duck", "Duck.glb", NULL, 0, &r), DE_OK);
    parse_sub(&r, "node2", &v);
    EXPECT_GE(v.lod_count, 4u);
    for (u32 k = 1; k < v.lod_count; ++k) {
        EXPECT_LT(de_cooked_mesh_get_lod(&v, k).triangle_count,
                  de_cooked_mesh_get_lod(&v, k - 1).triangle_count);
        EXPECT_TRUE(de_cooked_mesh_get_lod(&v, k).error >= de_cooked_mesh_get_lod(&v, k - 1).error);
    }
    de_cook_result_free(&r);
}

/* ---- import.mesh.lod_error_metric ------------------------------------------------------------ */

/* Closest distance from p to triangle abc (Ericson, Real-Time Collision Detection 5.1.5). */
static f64 point_tri_dist(const f64 p[3], const f64 a[3], const f64 b[3], const f64 c[3]) {
    f64 ab[3], ac[3], ap[3], q[3];
    for (u32 k = 0; k < 3; ++k) {
        ab[k] = b[k] - a[k];
        ac[k] = c[k] - a[k];
        ap[k] = p[k] - a[k];
    }
#define DOT(x, y) ((x)[0] * (y)[0] + (x)[1] * (y)[1] + (x)[2] * (y)[2])
    f64 d1 = DOT(ab, ap), d2 = DOT(ac, ap);
    if (d1 <= 0 && d2 <= 0) {
        memcpy(q, a, sizeof q);
    } else {
        f64 bp[3], cp[3];
        for (u32 k = 0; k < 3; ++k) {
            bp[k] = p[k] - b[k];
            cp[k] = p[k] - c[k];
        }
        f64 d3 = DOT(ab, bp), d4 = DOT(ac, bp), d5 = DOT(ab, cp), d6 = DOT(ac, cp);
        f64 vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
        if (d3 >= 0 && d4 <= d3) {
            memcpy(q, b, sizeof q);
        } else if (d6 >= 0 && d5 <= d6) {
            memcpy(q, c, sizeof q);
        } else if (vc <= 0 && d1 >= 0 && d3 <= 0) {
            f64 t = d1 / (d1 - d3);
            for (u32 k = 0; k < 3; ++k)
                q[k] = a[k] + t * ab[k];
        } else if (vb <= 0 && d2 >= 0 && d6 <= 0) {
            f64 t = d2 / (d2 - d6);
            for (u32 k = 0; k < 3; ++k)
                q[k] = a[k] + t * ac[k];
        } else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
            f64 t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            for (u32 k = 0; k < 3; ++k)
                q[k] = b[k] + t * (c[k] - b[k]);
        } else {
            f64 den = 1.0 / (va + vb + vc), v = vb * den, w = vc * den;
            for (u32 k = 0; k < 3; ++k)
                q[k] = a[k] + ab[k] * v + ac[k] * w;
        }
    }
#undef DOT
    f64 d = 0.0;
    for (u32 k = 0; k < 3; ++k)
        d += (p[k] - q[k]) * (p[k] - q[k]);
    return sqrt(d);
}

/*
 * The LOD error metric, reproduced from its definition (model_internal.h) with meshoptimizer
 * called directly: per submesh, its vertex range, normals and UV0 as weighted attributes, the
 * seam locks, a target of floor(t0 / 2^k) triangles; error = max over submeshes of result_error
 * x meshopt_simplifyScale(range), at least the previous LOD's. Fills the triangle count and the
 * error of each LOD; returns the LOD count.
 */
static u32 repro_lods(mlod_fx *f, const de_model_mesh *m, u32 tris[8], f32 errors[8]) {
    tris[0] = m->index_count / 3u;
    errors[0] = 0.0f;
    if (tris[0] < 64u)
        return 1;
    const u8 *locks = de_model_lod_seam_locks(&f->ir, m);
    const f32 weights[5] = {0.5f, 0.5f, 0.5f, 1.0f, 1.0f};
    u32 count = 1;
    u32 *dst = (u32 *)malloc((size_t)m->index_count * sizeof(u32));
    u32 *local = (u32 *)malloc((size_t)m->index_count * sizeof(u32));
    f32 *attrs = (f32 *)malloc((size_t)m->vertex_count * 5u * sizeof(f32));
    ASSERT_TRUE(dst != NULL && local != NULL && attrs != NULL);
    for (u32 k = 1; k < 8u && tris[k - 1] >= 64u; ++k) {
        u32 total = 0;
        f32 err = errors[k - 1];
        for (u32 s = 0; s < m->submesh_count; ++s) {
            const de_model_submesh *sm = &m->submeshes[s];
            if (sm->index_count == 0)
                continue;
            u32 lo = UINT32_MAX, hi = 0;
            for (u32 i = 0; i < sm->index_count; ++i) {
                u32 x = m->indices[sm->index_first + i];
                lo = x < lo ? x : lo;
                hi = x > hi ? x : hi;
            }
            u32 vc = hi - lo + 1u;
            for (u32 i = 0; i < sm->index_count; ++i)
                local[i] = m->indices[sm->index_first + i] - lo;
            for (u32 x = 0; x < vc; ++x) {
                memcpy(attrs + 5u * x, m->normal + 3u * (u64)(lo + x), 3 * sizeof(f32));
                memcpy(attrs + 5u * x + 3u, m->uv[0] + 2u * (u64)(lo + x), 2 * sizeof(f32));
            }
            float rel = 0.0f;
            size_t n = meshopt_simplifyWithAttributes(
                dst, local, sm->index_count, m->position + 3u * (u64)lo, vc, 12u, attrs, 20u,
                weights, 5u, locks != NULL ? locks + lo : NULL,
                (size_t)((sm->index_count / 3u) >> k) * 3u, FLT_MAX, 0u, &rel);
            total += (u32)n / 3u;
            f32 e = rel * meshopt_simplifyScale(m->position + 3u * (u64)lo, vc, 12u);
            err = e > err ? e : err;
        }
        if (total == 0 || (f64)total > 0.9 * (f64)tris[k - 1])
            break;
        tris[k] = total;
        errors[k] = err;
        count = k + 1u;
    }
    free(dst);
    free(local);
    free(attrs);
    return count;
}

TEST_F(mesh, lod_error_metric, mlod_fx) {
    /* Two submeshes with a seam: the seam locks are part of the metric. */
    u32 segs = 120;
    de_model_mesh *m = mesh_fx_bunny(f, 120, segs, true, 1.0);
    /* The locks are exactly the duplicated equator ring, on both sides. */
    const u8 *locks = de_model_lod_seam_locks(&f->ir, m);
    ASSERT_TRUE(locks != NULL);
    u32 locked = 0;
    for (u32 i = 0; i < m->vertex_count; ++i)
        locked += locks[i] != 0 ? 1u : 0u;
    EXPECT_EQ(locked, 2u * segs);
    for (u32 i = 0; i < m->vertex_count; ++i)
        if (locks[i] != 0)
            EXPECT_FLOAT_EQ(m->position[3u * i + 2u], 0.0f, 1e-6f);

    de_model_lods lods;
    ASSERT_EQ(de_model_lods_build(&f->ir, m, true, &lods), DE_OK);
    u32 tris[8];
    f32 errors[8];
    u32 n = repro_lods(f, m, tris, errors);
    ASSERT_EQ(n, lods.count);
    for (u32 k = 0; k < n; ++k) {
        EXPECT_EQ(tris[k], lods.lod[k].index_count / 3u);
        EXPECT_FLOAT_EQ(errors[k], lods.lod[k].error, 0.0f);
    }

    /* The seam stays closed: every LOD keeps every locked vertex. */
    for (u32 k = 0; k < lods.count; ++k) {
        u8 *seen = (u8 *)calloc(m->vertex_count, 1);
        ASSERT_TRUE(seen != NULL);
        for (u32 i = 0; i < lods.lod[k].index_count; ++i)
            seen[lods.lod[k].indices[i]] = 1;
        u32 kept = 0;
        for (u32 i = 0; i < m->vertex_count; ++i)
            kept += locks[i] != 0 && seen[i] != 0 ? 1u : 0u;
        EXPECT_EQ(kept, 2u * segs);
        free(seen);
    }

    /* The metric is a geometric error: source vertices lie within a few times the stated error
     * of the simplified surface (sampled: every 97th vertex). */
    for (u32 k = 1; k < lods.count; ++k) {
        const de_model_lod *l = &lods.lod[k];
        f64 worst = 0.0;
        for (u32 i = 0; i < m->vertex_count; i += 97u) {
            f64 p[3] = {m->position[3u * i], m->position[3u * i + 1], m->position[3u * i + 2]};
            f64 best = DBL_MAX;
            for (u32 t = 0; t < l->index_count; t += 3) {
                f64 a[3], b[3], c[3];
                for (u32 j = 0; j < 3; ++j) {
                    a[j] = m->position[3u * (u64)l->indices[t] + j];
                    b[j] = m->position[3u * (u64)l->indices[t + 1] + j];
                    c[j] = m->position[3u * (u64)l->indices[t + 2] + j];
                }
                f64 d = point_tri_dist(p, a, b, c);
                best = d < best ? d : best;
            }
            worst = best > worst ? best : worst;
        }
        printf("  LOD %u: %u triangles, error %.6f, sampled distance %.6f\n", k,
               l->index_count / 3u, (double)l->error, worst);
        EXPECT_TRUE(worst <= 4.0 * (f64)l->error + 1e-6);
    }
}

/* ---- Quantization: a view over encoded vertices ---------------------------------------------- */

/* Encodes attribute `a` of every vertex of `m` with `q` into `buf` and points `v` at it. */
static void encode_attr(const de_model_quant *q, const de_model_mesh *m, u32 a, u8 *buf,
                        de_cooked_mesh_view *v) {
    u32 sz = de_cooked_vertex_format_size(q->format[a]);
    for (u32 i = 0; i < m->vertex_count; ++i)
        de_model_quant_encode(q, m, a, i, buf + (u64)i * sz);
    v->streams[a] = buf;
    v->stream_format[a] = q->format[a];
    memcpy(v->position_offset, q->pos_offset, sizeof v->position_offset);
    memcpy(v->position_scale, q->pos_scale, sizeof v->position_scale);
    memcpy(v->uv_offset, q->uv_offset, sizeof v->uv_offset);
    memcpy(v->uv_scale, q->uv_scale, sizeof v->uv_scale);
}

static f64 max_position_error(const de_model_mesh *m, const de_cooked_mesh_view *v) {
    f64 worst = 0.0;
    for (u32 i = 0; i < m->vertex_count; ++i) {
        f32 p[3] = {0};
        de_cooked_mesh_position(v, i, p);
        f64 d = 0.0;
        for (u32 k = 0; k < 3; ++k)
            d += ((f64)p[k] - m->position[3u * i + k]) * ((f64)p[k] - m->position[3u * i + k]);
        worst = sqrt(d) > worst ? sqrt(d) : worst;
    }
    return worst;
}

/* ---- import.mesh.quantize_position_error ----------------------------------------------------- */

TEST_F(mesh, quantize_position_error, mlod_fx) {
    /* Size 10: an extent of about 25 m, a 16-bit step of about 0.4 mm. */
    de_model_mesh *m = mesh_fx_bunny(f, 80, 80, false, 10.0);
    de_model_quant q;
    de_model_quant_choose(m, NULL, m->vertex_count, 0.0005f, &q);
    ASSERT_EQ(q.format[DE_MODEL_ATTR_POSITION], (u32)DE_COOKED_VF_UNORM16X4);
    for (u32 k = 0; k < 3; ++k)
        EXPECT_TRUE(q.pos_scale[k] <= 0.0005f);
    u8 *buf = (u8 *)calloc(m->vertex_count, 16);
    ASSERT_TRUE(buf != NULL);
    de_cooked_mesh_view v;
    memset(&v, 0, sizeof v);
    encode_attr(&q, m, DE_MODEL_ATTR_POSITION, buf, &v);
    f64 worst = max_position_error(m, &v);
    printf("  position error %.7f m (limit 0.0005)\n", worst);
    EXPECT_TRUE(worst <= 0.0005);
    /* The cooker's own decode is the runtime's. */
    for (u32 i = 0; i < m->vertex_count; i += 7u) {
        f32 a[3], b[3];
        de_model_quant_position(&q, m, i, a);
        de_cooked_mesh_position(&v, i, b);
        for (u32 k = 0; k < 3; ++k)
            EXPECT_FLOAT_EQ(a[k], b[k], 1e-5f);
    }

    /* UVs: within 1/65535 of their range. */
    ASSERT_EQ(q.format[DE_MODEL_ATTR_UV0], (u32)DE_COOKED_VF_UNORM16X2);
    encode_attr(&q, m, DE_MODEL_ATTR_UV0, buf, &v);
    f32 lo[2] = {FLT_MAX, FLT_MAX}, hi[2] = {-FLT_MAX, -FLT_MAX};
    for (u32 i = 0; i < m->vertex_count; ++i)
        for (u32 k = 0; k < 2; ++k) {
            f32 t = m->uv[0][2u * i + k];
            lo[k] = t < lo[k] ? t : lo[k];
            hi[k] = t > hi[k] ? t : hi[k];
        }
    for (u32 i = 0; i < m->vertex_count; ++i) {
        f32 t[2] = {0};
        ASSERT_TRUE(de_cooked_mesh_uv(&v, 0, i, t));
        for (u32 k = 0; k < 2; ++k)
            EXPECT_TRUE(fabs((f64)t[k] - m->uv[0][2u * i + k]) <= ((f64)hi[k] - lo[k]) / 65535.0);
    }

    /* Through the whole stage: the cooked header and the decoded AABB match the source. */
    de_model_mesh_settings s = mesh_default_settings();
    de_model_buf b;
    de_cooked_mesh_view cv;
    mesh_fx_cook_mesh(f, m, &s, &b, &cv);
    EXPECT_EQ(cv.stream_format[DE_COOKED_STREAM_POSITION], (u32)DE_COOKED_VF_UNORM16X4);
    EXPECT_FLOAT_EQ(cv.max_position_error, 0.0005f, 0.0f);
    for (u32 k = 0; k < 3; ++k) {
        EXPECT_FLOAT_EQ(cv.bounds_min[k], m->bounds_min[k], 0.0005f);
        EXPECT_FLOAT_EQ(cv.bounds_max[k], m->bounds_max[k], 0.0005f);
    }
    free(buf);
}

/* ---- import.mesh.quantize_fallback_f32 ------------------------------------------------------- */

TEST_F(mesh, quantize_fallback_f32, mlod_fx) {
    /* Size 40: an extent of about 100 m, a 16-bit step of about 1.5 mm > 0.5 mm. */
    de_model_mesh *m = mesh_fx_bunny(f, 60, 60, false, 40.0);
    de_model_quant q;
    de_model_quant_choose(m, NULL, m->vertex_count, 0.0005f, &q);
    EXPECT_EQ(q.format[DE_MODEL_ATTR_POSITION], (u32)DE_COOKED_VF_F32X3);
    u8 *buf = (u8 *)calloc(m->vertex_count, 16);
    ASSERT_TRUE(buf != NULL);
    de_cooked_mesh_view v;
    memset(&v, 0, sizeof v);
    encode_attr(&q, m, DE_MODEL_ATTR_POSITION, buf, &v);
    EXPECT_TRUE(max_position_error(m, &v) == 0.0);
    /* A looser limit quantizes it, within that limit. */
    de_model_quant_choose(m, NULL, m->vertex_count, 0.01f, &q);
    EXPECT_EQ(q.format[DE_MODEL_ATTR_POSITION], (u32)DE_COOKED_VF_UNORM16X4);
    encode_attr(&q, m, DE_MODEL_ATTR_POSITION, buf, &v);
    EXPECT_TRUE(max_position_error(m, &v) <= 0.01);
    /* A limit of 0 never quantizes. */
    de_model_quant_choose(m, NULL, 4, 0.0f, &q);
    EXPECT_EQ(q.format[DE_MODEL_ATTR_POSITION], (u32)DE_COOKED_VF_F32X3);

    /* UVs over a range above DE_COOKED_UV_UNORM_MAX_RANGE: F32X2, exact. */
    for (u32 i = 0; i < 2u * m->vertex_count; ++i)
        m->uv[0][i] *= 100.0f;
    de_model_quant_choose(m, NULL, m->vertex_count, 0.0005f, &q);
    EXPECT_EQ(q.format[DE_MODEL_ATTR_UV0], (u32)DE_COOKED_VF_F32X2);
    encode_attr(&q, m, DE_MODEL_ATTR_UV0, buf, &v);
    for (u32 i = 0; i < m->vertex_count; ++i) {
        f32 t[2] = {0};
        ASSERT_TRUE(de_cooked_mesh_uv(&v, 0, i, t));
        EXPECT_FLOAT_EQ(t[0], m->uv[0][2u * i], 0.0f);
        EXPECT_FLOAT_EQ(t[1], m->uv[0][2u * i + 1u], 0.0f);
    }
    free(buf);

    /* Through the importer: max_position_error = 0 keeps f32 positions. */
    mesh_fx_cooker(f);
    de__manifest_kv kv = mesh_kv_float("max_position_error", 0.0);
    de_cook_result r;
    ASSERT_EQ(mesh_fx_cook(f, MOUNT, "props/duck", "Duck.glb", &kv, 1, &r), DE_OK);
    de_cooked_mesh_view dv;
    parse_sub(&r, "node2", &dv);
    EXPECT_EQ(dv.stream_format[DE_COOKED_STREAM_POSITION], (u32)DE_COOKED_VF_F32X3);
    EXPECT_FLOAT_EQ(dv.max_position_error, 0.0f, 0.0f);
    de_cook_result_free(&r);
}

/* ---- import.mesh.quantize_oct_normals -------------------------------------------------------- */

static f64 angle_between(const f32 a[3], const f64 b[3]) {
    f64 la = sqrt((f64)a[0] * a[0] + (f64)a[1] * a[1] + (f64)a[2] * a[2]);
    f64 lb = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    f64 c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
    c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
    return acos(c);
}

TEST_F(mesh, quantize_oct_normals, mlod_fx) {
    /* Deterministic directions: the axes and octant diagonals, then a Fibonacci sphere. */
    enum { N = 20000 };
    static f64 dirs[N + 26][3];
    u32 n = 0;
    for (i32 x = -1; x <= 1; ++x)
        for (i32 y = -1; y <= 1; ++y)
            for (i32 z = -1; z <= 1; ++z)
                if (x != 0 || y != 0 || z != 0) {
                    dirs[n][0] = x;
                    dirs[n][1] = y;
                    dirs[n][2] = z;
                    ++n;
                }
    for (u32 i = 0; i < N; ++i, ++n) {
        f64 y = 1.0 - 2.0 * ((f64)i + 0.5) / N, r = sqrt(1.0 - y * y), ph = i * 2.39996322972865332;
        dirs[n][0] = r * cos(ph);
        dirs[n][1] = y;
        dirs[n][2] = r * sin(ph);
    }
    f64 worst_n = 0.0, worst_t = 0.0;
    for (u32 i = 0; i < n; ++i) {
        f64 l = sqrt(dirs[i][0] * dirs[i][0] + dirs[i][1] * dirs[i][1] + dirs[i][2] * dirs[i][2]);
        f32 u[3] = {(f32)(dirs[i][0] / l), (f32)(dirs[i][1] / l), (f32)(dirs[i][2] / l)};
        i16 c[2];
        f32 d[3] = {0};
        de_model_oct_encode(u, -1, c);
        de_cooked_oct_decode(c[0], c[1], d);
        f64 a = angle_between(d, dirs[i]);
        worst_n = a > worst_n ? a : worst_n;
        /* Tangents carry the bitangent sign in bit 0 of y; both signs keep the direction. */
        for (i32 sign = 0; sign < 2; ++sign) {
            de_model_oct_encode(u, sign, c);
            EXPECT_EQ((u32)((u16)c[1] & 1u), (u32)sign);
            de_cooked_oct_decode(c[0], (i16)((u16)c[1] & 0xFFFEu), d);
            a = angle_between(d, dirs[i]);
            worst_t = a > worst_t ? a : worst_t;
        }
    }
    printf("  worst angle: normals %.2e rad, tangents %.2e rad\n", worst_n, worst_t);
    EXPECT_TRUE(worst_n <= 0.01);
    EXPECT_TRUE(worst_t <= 0.01);
    /* 16 bits are far better than the requirement. */
    EXPECT_TRUE(worst_n <= 5e-4);

    /* Through a cooked mesh's stream: normals and tangents with both signs. */
    de_model_mesh *m = mesh_fx_bunny(f, 40, 40, false, 1.0);
    m->tangent = (f32 *)de_model_ir_array(&f->ir, (u64)m->vertex_count * 4u, sizeof(f32));
    ASSERT_TRUE(m->tangent != NULL);
    for (u32 i = 0; i < m->vertex_count; ++i) {
        const f32 *nn = m->normal + 3u * i;
        /* Any unit vector perpendicular to the normal. */
        f64 t[3] = {-nn[1], nn[0], 0.0};
        f64 l = sqrt(t[0] * t[0] + t[1] * t[1]);
        if (l < 1e-3) {
            t[0] = 1.0;
            t[1] = 0.0;
            l = 1.0;
        }
        for (u32 k = 0; k < 3; ++k)
            m->tangent[4u * i + k] = (f32)(t[k] / l);
        m->tangent[4u * i + 3u] = i % 2u == 0 ? 1.0f : -1.0f;
    }
    de_model_quant q;
    de_model_quant_choose(m, NULL, m->vertex_count, 0.0005f, &q);
    EXPECT_EQ(q.format[DE_MODEL_ATTR_NORMAL], (u32)DE_COOKED_VF_OCT16);
    EXPECT_EQ(q.format[DE_MODEL_ATTR_TANGENT], (u32)DE_COOKED_VF_OCT16_SIGN);
    u8 *nb = (u8 *)calloc(m->vertex_count, 4), *tb = (u8 *)calloc(m->vertex_count, 4);
    ASSERT_TRUE(nb != NULL && tb != NULL);
    de_cooked_mesh_view v;
    memset(&v, 0, sizeof v);
    encode_attr(&q, m, DE_MODEL_ATTR_NORMAL, nb, &v);
    encode_attr(&q, m, DE_MODEL_ATTR_TANGENT, tb, &v);
    for (u32 i = 0; i < m->vertex_count; ++i) {
        f32 d[4] = {0};
        ASSERT_TRUE(de_cooked_mesh_normal(&v, i, d));
        f64 want[3] = {m->normal[3u * i], m->normal[3u * i + 1], m->normal[3u * i + 2]};
        EXPECT_TRUE(angle_between(d, want) <= 0.01);
        ASSERT_TRUE(de_cooked_mesh_tangent(&v, i, d));
        f64 tw[3] = {m->tangent[4u * i], m->tangent[4u * i + 1], m->tangent[4u * i + 2]};
        EXPECT_TRUE(angle_between(d, tw) <= 0.01);
        EXPECT_FLOAT_EQ(d[3], m->tangent[4u * i + 3u], 0.0f);
    }
    free(nb);
    free(tb);
}

/* ---- import.mesh.quantize_weights_sum -------------------------------------------------------- */

TEST_F(mesh, quantize_weights_sum, mlod_fx) {
    const f32 cases[][4] = {
        {1.0f, 0.0f, 0.0f, 0.0f},          {0.5f, 0.5f, 0.0f, 0.0f},
        {1.0f / 3, 1.0f / 3, 1.0f / 3, 0}, {0.25f, 0.25f, 0.25f, 0.25f},
        {0.7f, 0.2f, 0.07f, 0.03f},        {2.0f, 1.0f, 1.0f, 0.0f},  /* not normalised */
        {0.0f, 0.0f, 0.0f, 0.0f},          {-1.0f, 0.5f, 0.5f, 0.0f}, /* negative: 0 */
        {0.999f, 0.001f, 0.0f, 0.0f},      {0.002f, 0.002f, 0.002f, 0.994f},
    };
    for (u32 c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        u8 w[4] = {0};
        de_model_weights_unorm8(cases[c], w);
        EXPECT_EQ((u32)w[0] + w[1] + w[2] + w[3], 255u);
        f64 sum = 0.0;
        for (u32 k = 0; k < 4; ++k)
            sum += cases[c][k] > 0.0f ? cases[c][k] : 0.0;
        for (u32 k = 0; k < 4; ++k) {
            f64 want = sum > 0.0 ? (cases[c][k] > 0.0f ? cases[c][k] : 0.0) / sum * 255.0
                                 : (k == 0 ? 255.0 : 0.0);
            EXPECT_TRUE(fabs((f64)w[k] - want) < 1.0);
        }
    }
    /* Many generated weight sets. */
    u32 seed = 12345u;
    for (u32 i = 0; i < 10000u; ++i) {
        f32 v[4];
        for (u32 k = 0; k < 4; ++k) {
            seed = seed * 1664525u + 1013904223u;
            v[k] = (f32)(seed >> 8) / (f32)(1u << 24);
        }
        u8 w[4] = {0};
        de_model_weights_unorm8(v, w);
        EXPECT_EQ((u32)w[0] + w[1] + w[2] + w[3], 255u);
    }

    /* Through the encoder of a skinned mesh: joints u8, then u16 above 255. */
    de_model_mesh *m = mesh_fx_bunny(f, 20, 20, false, 1.0);
    m->joints0 = (u16 *)de_model_ir_array(&f->ir, (u64)m->vertex_count * 4u, sizeof(u16));
    m->weights0 = (f32 *)de_model_ir_array(&f->ir, (u64)m->vertex_count * 4u, sizeof(f32));
    ASSERT_TRUE(m->joints0 != NULL && m->weights0 != NULL);
    for (u32 i = 0; i < m->vertex_count; ++i)
        for (u32 k = 0; k < 4; ++k) {
            m->joints0[4u * i + k] = (u16)((i + k) % 200u);
            m->weights0[4u * i + k] = (f32)(k + 1u) / 10.0f;
        }
    de_model_quant q;
    de_model_quant_choose(m, NULL, m->vertex_count, 0.0005f, &q);
    EXPECT_EQ(q.format[DE_MODEL_ATTR_JOINTS0], (u32)DE_COOKED_VF_U8X4);
    EXPECT_EQ(q.format[DE_MODEL_ATTR_WEIGHTS0], (u32)DE_COOKED_VF_UNORM8X4);
    m->joints0[7] = 300;
    de_model_quant_choose(m, NULL, m->vertex_count, 0.0005f, &q);
    EXPECT_EQ(q.format[DE_MODEL_ATTR_JOINTS0], (u32)DE_COOKED_VF_U16X4);
    de_model_mesh_settings s = mesh_default_settings();
    de_model_buf b;
    de_cooked_mesh_view v;
    mesh_fx_cook_mesh(f, m, &s, &b, &v);
    EXPECT_EQ(v.stream_format[DE_COOKED_STREAM_JOINTS0], (u32)DE_COOKED_VF_U16X4);
    bool saw300 = false;
    for (u32 i = 0; i < v.vertex_count; ++i) {
        u8 w[4] = {0};
        u16 j[4] = {0};
        ASSERT_TRUE(de_cooked_mesh_weights(&v, i, w));
        ASSERT_TRUE(de_cooked_mesh_joints(&v, i, j));
        EXPECT_EQ((u32)w[0] + w[1] + w[2] + w[3], 255u);
        EXPECT_EQ((u32)w[3], 102u); /* 0.4 of 255 */
        saw300 = saw300 || j[3] == 300u;
    }
    EXPECT_TRUE(saw300);
}

/* ---- import.mesh.cook_v2_alignment ----------------------------------------------------------- */

static void check_alignment(const de_cooked_mesh_view *v) {
    const u8 *p = v->base;
    EXPECT_EQ(memcmp(p, "DMSH", 4), 0);
    EXPECT_EQ(de_cooked_get_u32(p + 4), 2u);
    /* Every section offset in the header is 256-aligned (0 = absent meshlet sections). */
    u32 sections = 0;
    for (u32 i = 0; i < 8; ++i) {
        u32 off = de_cooked_get_u32(p + 56 + 4 * i);
        EXPECT_EQ(off % DE_COOKED_MESH_ALIGN, 0u);
        if (off != 0) {
            EXPECT_GE(off, DE_COOKED_MESH_HEADER_SIZE);
            ++sections;
        }
    }
    EXPECT_EQ(sections, (v->meshlet_count != 0) ? 8u : 5u);
    for (u32 s = 0; s < DE_COOKED_STREAM_COUNT; ++s)
        if (v->streams[s] != NULL)
            EXPECT_EQ((u64)(v->streams[s] - p) % DE_COOKED_MESH_ALIGN, 0u);
    /* Pointers a loader uploads are aligned within the blob. */
    EXPECT_EQ((u64)(v->indices - p) % DE_COOKED_MESH_ALIGN, 0u);
    if (v->meshlet_count != 0) {
        EXPECT_EQ((u64)(v->meshlets - p) % DE_COOKED_MESH_ALIGN, 0u);
        EXPECT_EQ((u64)(v->meshlet_vertices - p) % DE_COOKED_MESH_ALIGN, 0u);
        EXPECT_EQ((u64)(v->meshlet_triangles - p) % DE_COOKED_MESH_ALIGN, 0u);
        EXPECT_EQ(v->meshlet_triangle_bytes % 4u, 0u);
    }
    /* The reserved header words are zero. */
    for (u32 i = 192; i < DE_COOKED_MESH_HEADER_SIZE; ++i)
        EXPECT_EQ((u32)p[i], 0u);
    EXPECT_EQ(de_cooked_get_u32(p + 52), 0u);
}

TEST_F(mesh, cook_v2_alignment, mlod_fx) {
    mesh_fx_cooker(f);
    de_cook_result r;
    ASSERT_EQ(mesh_fx_cook(f, MOUNT, "props/duck", "Duck.glb", NULL, 0, &r), DE_OK);
    de_cooked_mesh_view v;
    parse_sub(&r, "node2", &v);
    check_alignment(&v);
    /* Duck has normals and UVs: three streams. */
    EXPECT_EQ(v.stream_count, 3u);
    EXPECT_EQ(v.stream_format[DE_COOKED_STREAM_NORMAL], (u32)DE_COOKED_VF_OCT16);
    EXPECT_EQ(v.stream_format[DE_COOKED_STREAM_UV0], (u32)DE_COOKED_VF_UNORM16X2);
    de_cook_result_free(&r);
    de__manifest_kv kv = mesh_kv_bool("meshlets", false);
    ASSERT_EQ(mesh_fx_cook(f, MOUNT, "props/duck", "Duck.glb", &kv, 1, &r), DE_OK);
    parse_sub(&r, "node2", &v);
    check_alignment(&v);
    de_cook_result_free(&r);

    /* A corrupted blob is refused: a misaligned section, a meshlet over the limits. */
    ASSERT_EQ(mesh_fx_cook(f, MOUNT, "props/duck", "Duck.glb", NULL, 0, &r), DE_OK);
    const de_cook_subasset *s = de_cook_result_subasset(&r, "node2");
    ASSERT_TRUE(s != NULL);
    u8 *copy = (u8 *)malloc((size_t)s->size);
    ASSERT_TRUE(copy != NULL);
    memcpy(copy, s->data, (size_t)s->size);
    const char *why = NULL;
    de_cooked_put_u32(copy + 84, de_cooked_get_u32(copy + 84) + 4u);
    EXPECT_FALSE(de_cooked_mesh_parse(copy, s->size, &v, &why));
    memcpy(copy, s->data, (size_t)s->size);
    u32 ml_off = de_cooked_get_u32(copy + 72);
    de_cooked_put_u32(copy + ml_off + 8, 65u | (124u << 8));
    EXPECT_FALSE(de_cooked_mesh_parse(copy, s->size, &v, &why));
    EXPECT_FALSE(de_cooked_mesh_parse(copy, 100, &v, &why));
    free(copy);
    de_cook_result_free(&r);
}

/* ---- import.mesh.cook_deterministic ---------------------------------------------------------- */

TEST_F(mesh, cook_deterministic, mlod_fx) {
    /* The stage alone, twice on the same IR: the same bytes. */
    de_model_mesh *m = mesh_fx_bunny(f, 100, 100, true, 1.0);
    de_model_mesh_settings s = mesh_default_settings();
    de_model_buf a, b;
    de_cooked_mesh_view va, vb;
    mesh_fx_cook_mesh(f, m, &s, &a, &va);
    mesh_fx_cook_mesh(f, m, &s, &b, &vb);
    ASSERT_EQ(a.size, b.size);
    EXPECT_MEMEQ(a.data, b.data, (size_t)a.size);

    /* Through the cooker (verify_determinism on: every cook runs the importer twice), and a
     * second cook. */
    mesh_fx_cooker(f);
    de_cook_result r1, r2;
    ASSERT_EQ(mesh_fx_cook(f, MOUNT, "props/duck", "Duck.glb", NULL, 0, &r1), DE_OK);
    ASSERT_EQ(mesh_fx_cook(f, MOUNT, "props/duck", "Duck.glb", NULL, 0, &r2), DE_OK);
    EXPECT_EQ(r1.output_hash, r2.output_hash);
    const de_cook_subasset *s1 = de_cook_result_subasset(&r1, "node2");
    const de_cook_subasset *s2 = de_cook_result_subasset(&r2, "node2");
    ASSERT_TRUE(s1 != NULL && s2 != NULL);
    ASSERT_EQ(s1->size, s2->size);
    EXPECT_MEMEQ(s1->data, s2->data, (size_t)s1->size);
    EXPECT_EQ(r1.importer_version, DE_MODEL_IMPORTER_VERSION);
    EXPECT_EQ(DE_MODEL_IMPORTER_VERSION, 2u);
    de_cook_result_free(&r2);

    /* Each setting of the mesh stage is in the settings hash and changes the output. */
    const de__manifest_kv settings[4] = {
        mesh_kv_bool("generate_lods", false), mesh_kv_float("lod_error", 0.02),
        mesh_kv_bool("meshlets", false), mesh_kv_float("max_position_error", 0.0)};
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_EQ(mesh_fx_cook(f, MOUNT, "props/duck", "Duck.glb", &settings[i], 1, &r2), DE_OK);
        EXPECT_NE(r1.settings_hash, r2.settings_hash);
        EXPECT_NE(r1.output_hash, r2.output_hash);
        de_cooked_mesh_view v;
        parse_sub(&r2, "node2", &v);
        if (i == 0)
            EXPECT_EQ(v.lod_count, 1u);
        if (i == 1)
            EXPECT_FLOAT_EQ(v.lod_error, 0.02f, 0.0f);
        if (i == 2)
            EXPECT_EQ(v.meshlet_count, 0u);
        de_cook_result_free(&r2);
    }
    de_cook_result_free(&r1);

    /* Out-of-range settings fail the cook with a message. */
    de__manifest_kv bad = mesh_kv_float("lod_error", -1.0);
    EXPECT_EQ(mesh_fx_cook(f, MOUNT, "props/duck", "Duck.glb", &bad, 1, &r2),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(de_error_last()->message, "lod_error") != NULL);
    bad = mesh_kv_float("max_position_error", -0.5);
    EXPECT_EQ(mesh_fx_cook(f, MOUNT, "props/duck", "Duck.glb", &bad, 1, &r2),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(de_error_last()->message, "max_position_error") != NULL);
}
