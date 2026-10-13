/*
 * test_meshlets.c: the meshlets of the mesh stage (DENG-103, SPEC §11.6, §12.5): limits,
 * coverage, bounds and the conservative normal cone, on a generated bunny-class mesh (one and two
 * submeshes) and on cooked glTF models (Duck, two materials). CTest names import.mesh.<test>.
 */
#include "mesh_fixtures.h"

#include <stdio.h>

#define MOUNT "mlet"

typedef mesh_fx mlet_fx;

static void mlet_fx_setup(mlet_fx *f) {
    mesh_fx_setup(f, MOUNT);
}

static void mlet_fx_teardown(mlet_fx *f) {
    mesh_fx_teardown(f);
}

/* The DMSH blob of sub-asset `name` of a cook. */
static void parse_sub(const de_cook_result *r, const char *name, de_cooked_mesh_view *v) {
    const de_cook_subasset *s = de_cook_result_subasset(r, name);
    if (s == NULL)
        printf("  no sub-asset \"%s\"\n", name);
    ASSERT_TRUE(s != NULL);
    EXPECT_STREQ(s->type, "mesh");
    const char *why = "";
    bool ok = de_cooked_mesh_parse(s->data, s->size, v, &why);
    if (!ok)
        printf("  parse failed: %s\n", why);
    ASSERT_TRUE(ok);
}

/* ---- import.mesh.meshlet_limits -------------------------------------------------------------- */

TEST_F(mesh, meshlet_limits, mlet_fx) {
    de_model_mesh *m = mesh_fx_bunny(f, 160, 160, false, 1.0);
    de_model_mesh_settings s = mesh_default_settings();
    de_model_buf b;
    de_cooked_mesh_view v;
    mesh_fx_cook_mesh(f, m, &s, &b, &v);
    EXPECT_TRUE((v.flags & DE_COOKED_MESH_FLAG_MESHLETS) != 0);
    ASSERT_TRUE(v.meshlet_count > 0u);
    u32 full = 0, total_tris = 0;
    for (u32 i = 0; i < v.meshlet_count; ++i) {
        de_cooked_meshlet ml = de_cooked_mesh_meshlet(&v, i);
        EXPECT_LE(ml.vertex_count, 64u);
        EXPECT_LE(ml.triangle_count, 124u);
        EXPECT_GE(ml.vertex_count, 3u);
        full += ml.triangle_count == 124u || ml.vertex_count == 64u ? 1u : 0u;
        total_tris += ml.triangle_count;
    }
    /* The builder fills most meshlets to one of the limits. */
    EXPECT_GT(full * 2u, v.meshlet_count);
    u32 lod_tris = 0;
    for (u32 l = 0; l < v.lod_count; ++l) {
        de_cooked_mesh_lod lod = de_cooked_mesh_get_lod(&v, l);
        lod_tris += lod.triangle_count;
        EXPECT_GT(lod.meshlet_count, 0u);
    }
    EXPECT_EQ(total_tris, lod_tris);
    EXPECT_GT(mesh_check_meshlets(&v), 0u);

    /* A cooked glTF model. */
    mesh_fx_cooker(f);
    de_cook_result r;
    ASSERT_EQ(mesh_fx_cook(f, MOUNT, "props/duck", "Duck.glb", NULL, 0, &r), DE_OK);
    de_cooked_mesh_view dv;
    parse_sub(&r, "node2", &dv);
    EXPECT_GT(dv.meshlet_count, 0u);
    EXPECT_GT(mesh_check_meshlets(&dv), 0u);
    de_cook_result_free(&r);

    /* `meshlets = false`: indexed LODs only. */
    de__manifest_kv kv = mesh_kv_bool("meshlets", false);
    ASSERT_EQ(mesh_fx_cook(f, MOUNT, "props/duck", "Duck.glb", &kv, 1, &r), DE_OK);
    parse_sub(&r, "node2", &dv);
    EXPECT_EQ(dv.flags & DE_COOKED_MESH_FLAG_MESHLETS, 0u);
    EXPECT_EQ(dv.meshlet_count, 0u);
    EXPECT_TRUE(dv.meshlets == NULL);
    EXPECT_GT(dv.lod_count, 1u);
    EXPECT_GT(dv.index_count, 0u);
    de_cook_result_free(&r);
}

/* ---- import.mesh.meshlet_coverage ------------------------------------------------------------ */

TEST_F(mesh, meshlet_coverage, mlet_fx) {
    /* Two submeshes with a seam: every LOD and submesh is covered exactly by its meshlets. */
    de_model_mesh *m = mesh_fx_bunny(f, 120, 120, true, 1.0);
    de_model_mesh_settings s = mesh_default_settings();
    de_model_buf b;
    de_cooked_mesh_view v;
    mesh_fx_cook_mesh(f, m, &s, &b, &v);
    EXPECT_EQ(v.submesh_count, 2u);
    EXPECT_GT(v.lod_count, 3u);
    /* LOD 0 holds every source triangle. */
    EXPECT_EQ(de_cooked_mesh_get_lod(&v, 0).triangle_count, m->index_count / 3u);
    EXPECT_GT(mesh_check_meshlets(&v), 0u);
    /* The LOD-submesh ranges tile each LOD's ranges in submesh order. */
    for (u32 l = 0; l < v.lod_count; ++l) {
        de_cooked_mesh_lod lod = de_cooked_mesh_get_lod(&v, l);
        u32 at = lod.index_first, mat = lod.meshlet_first;
        for (u32 j = 0; j < v.submesh_count; ++j) {
            de_cooked_mesh_range r = de_cooked_mesh_lod_submesh(&v, l, j);
            EXPECT_EQ(r.index_first, at);
            EXPECT_EQ(r.meshlet_first, mat);
            at += r.index_count;
            mat += r.meshlet_count;
        }
        EXPECT_EQ(at, lod.index_first + lod.index_count);
        EXPECT_EQ(mat, lod.meshlet_first + lod.meshlet_count);
    }

    /* Cooked glTF: two materials, and Duck. */
    mesh_fx_cooker(f);
    de_cook_result r;
    ASSERT_EQ(mesh_fx_cook(f, MOUNT, "props/panels", "two_materials.gltf", NULL, 0, &r), DE_OK);
    de_cooked_mesh_view pv;
    parse_sub(&r, "Panel", &pv);
    EXPECT_EQ(pv.submesh_count, 2u);
    EXPECT_EQ(pv.lod_count, 1u); /* 4 triangles: below the LOD minimum */
    EXPECT_EQ(mesh_check_meshlets(&pv), 2u);
    de_cook_result_free(&r);
    ASSERT_EQ(mesh_fx_cook(f, MOUNT, "props/duck", "Duck.glb", NULL, 0, &r), DE_OK);
    parse_sub(&r, "node2", &pv);
    EXPECT_EQ(de_cooked_mesh_get_lod(&pv, 0).triangle_count, 4212u);
    EXPECT_GT(mesh_check_meshlets(&pv), 0u);
    de_cook_result_free(&r);
}

/* ---- import.mesh.meshlet_bounds_contain ------------------------------------------------------ */

static void check_mesh_bounds(const de_cooked_mesh_view *v) {
    for (u32 i = 0; i < v->vertex_count; ++i) {
        f32 p[3] = {0};
        de_cooked_mesh_position(v, i, p);
        f64 d2 = 0.0;
        for (u32 k = 0; k < 3; ++k) {
            EXPECT_TRUE(p[k] >= v->bounds_min[k] && p[k] <= v->bounds_max[k]);
            d2 += ((f64)p[k] - v->sphere[k]) * ((f64)p[k] - v->sphere[k]);
        }
        EXPECT_TRUE(sqrt(d2) <= (f64)v->sphere[3]);
    }
    /* Submesh bounds hold the vertices of the submesh's LOD 0 triangles. */
    for (u32 j = 0; j < v->submesh_count; ++j) {
        de_cooked_submesh sm = de_cooked_mesh_submesh(v, j);
        de_cooked_mesh_range r = de_cooked_mesh_lod_submesh(v, 0, j);
        for (u32 i = 0; i < r.index_count; ++i) {
            f32 p[3] = {0};
            de_cooked_mesh_position(v, de_cooked_mesh_index(v, r.index_first + i), p);
            for (u32 k = 0; k < 3; ++k)
                EXPECT_TRUE(p[k] >= sm.bounds_min[k] && p[k] <= sm.bounds_max[k]);
        }
    }
}

TEST_F(mesh, meshlet_bounds_contain, mlet_fx) {
    /* Quantized positions (size 1: UNORM16X4), then a far-off mesh with f32 positions. */
    de_model_mesh *m = mesh_fx_bunny(f, 100, 100, true, 1.0);
    de_model_mesh_settings s = mesh_default_settings();
    de_model_buf b;
    de_cooked_mesh_view v;
    mesh_fx_cook_mesh(f, m, &s, &b, &v);
    EXPECT_EQ(v.stream_format[DE_COOKED_STREAM_POSITION], (u32)DE_COOKED_VF_UNORM16X4);
    EXPECT_GT(mesh_check_meshlets(&v), 0u);
    check_mesh_bounds(&v);
    /* 100 times larger (a 16-bit step of about 4 mm) and 1 km away. */
    for (u32 i = 0; i < 3u * m->vertex_count; ++i)
        m->position[i] = m->position[i] * 100.0f + (i % 3u == 0 ? 1000.0f : 0.0f);
    mesh_fx_cook_mesh(f, m, &s, &b, &v);
    EXPECT_EQ(v.stream_format[DE_COOKED_STREAM_POSITION], (u32)DE_COOKED_VF_F32X3);
    EXPECT_GT(mesh_check_meshlets(&v), 0u);
    check_mesh_bounds(&v);
    EXPECT_FLOAT_EQ(v.sphere[0], 1000.0f, 40.0f);

    mesh_fx_cooker(f);
    de_cook_result r;
    ASSERT_EQ(mesh_fx_cook(f, MOUNT, "props/duck", "Duck.glb", NULL, 0, &r), DE_OK);
    parse_sub(&r, "node2", &v);
    EXPECT_GT(mesh_check_meshlets(&v), 0u);
    check_mesh_bounds(&v);
    de_cook_result_free(&r);
}

/* ---- import.mesh.meshlet_cone_conservative --------------------------------------------------- */

/* Sampled camera positions around the mesh: Fibonacci directions at several distances from the
 * sphere centre, inside the mesh too. For every meshlet the apex test culls, no triangle may face
 * the camera. Returns the number of (camera, meshlet) pairs culled. */
static u32 check_cones(const de_cooked_mesh_view *v) {
    const u32 dirs = 48;
    const f64 dist[4] = {0.25, 1.1, 2.0, 6.0};
    u32 culled = 0, bad = 0;
    for (u32 di = 0; di < dirs; ++di) {
        f64 y = 1.0 - 2.0 * ((f64)di + 0.5) / dirs, rr = sqrt(1.0 - y * y);
        f64 ph = (f64)di * 2.39996322972865332;
        f64 dir[3] = {rr * cos(ph), y, rr * sin(ph)};
        for (u32 k = 0; k < 4; ++k) {
            f64 cam[3];
            for (u32 c = 0; c < 3; ++c)
                cam[c] = v->sphere[c] + dir[c] * dist[k] * v->sphere[3];
            for (u32 i = 0; i < v->meshlet_count; ++i) {
                de_cooked_meshlet ml = de_cooked_mesh_meshlet(v, i);
                f64 to[3], len = 0.0, dot = 0.0;
                for (u32 c = 0; c < 3; ++c) {
                    to[c] = (f64)ml.cone_apex[c] - cam[c];
                    len += to[c] * to[c];
                }
                len = sqrt(len);
                if (len == 0.0)
                    continue;
                for (u32 c = 0; c < 3; ++c)
                    dot += to[c] / len * ml.cone_axis[c];
                if (!(dot >= (f64)ml.cone_cutoff))
                    continue;
                ++culled;
                for (u32 t = 0; t < ml.triangle_count; ++t) {
                    const u8 *tri = v->meshlet_triangles + ml.triangle_offset + 3u * t;
                    f32 p[3][3];
                    for (u32 j = 0; j < 3; ++j)
                        de_cooked_mesh_position(
                            v, de_cooked_mesh_meshlet_vertex(v, ml.vertex_offset + tri[j]), p[j]);
                    f64 e1[3], e2[3], w[3];
                    for (u32 c = 0; c < 3; ++c) {
                        e1[c] = (f64)p[1][c] - p[0][c];
                        e2[c] = (f64)p[2][c] - p[0][c];
                        w[c] = cam[c] - p[0][c];
                    }
                    f64 n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                e1[0] * e2[1] - e1[1] * e2[0]};
                    f64 nl = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    f64 wl = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
                    if (nl == 0.0 || wl == 0.0)
                        continue; /* a degenerate triangle faces nowhere */
                    /* Front-facing beyond rounding: the camera sees the triangle's front. */
                    if ((n[0] * w[0] + n[1] * w[1] + n[2] * w[2]) / (nl * wl) > 1e-4)
                        ++bad;
                }
            }
        }
    }
    EXPECT_EQ(bad, 0u);
    return culled;
}

TEST_F(mesh, meshlet_cone_conservative, mlet_fx) {
    de_model_mesh *m = mesh_fx_bunny(f, 120, 120, false, 1.0);
    de_model_mesh_settings s = mesh_default_settings();
    de_model_buf b;
    de_cooked_mesh_view v;
    mesh_fx_cook_mesh(f, m, &s, &b, &v);
    /* The test means something only if cones do cull. */
    EXPECT_GT(check_cones(&v), 1000u);

    mesh_fx_cooker(f);
    de_cook_result r;
    ASSERT_EQ(mesh_fx_cook(f, MOUNT, "props/duck", "Duck.glb", NULL, 0, &r), DE_OK);
    parse_sub(&r, "node2", &v);
    EXPECT_GT(check_cones(&v), 100u);
    de_cook_result_free(&r);
}
