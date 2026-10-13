/*
 * test_fbx_import.c: the FBX front end of the model importer (DENG-102, SPEC §11.6, §5.5, §12.6):
 * ufbx into the model IR, units and axes (file settings and the `axis_up` / `axis_front`
 * overrides), n-gons and the material split, PBR and legacy materials, skins and baked animation
 * stacks, determinism and corrupt input. CTest names import_fbx.fbx.<test>.
 *
 * Fixtures are in data/fbx/ (mounted read-only as fbx:/), written by gen_fixtures.py: binary and
 * ASCII FBX 7.4 files and the glTF reference cube; test_import_fbx_tmp/ in the build tree is
 * ftmp:/. The cooker tests run the importer through a de_cooker, as `de-cook` and the Dev runtime
 * do.
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_task.h>

#include "asset/asset_internal.h"
#include "asset/manifest_internal.h"
#include "asset/slots_internal.h"
#include "cook/importers/model/fbx_import.h"
#include "cook/importers/model/model_import.h"
#include "cook/importers/model/model_internal.h"
#include "cook/importers/model/model_ir.h"
#include "cook/lib/de_cooker.h"
#include "render/formats/de_cooked_material.h"
#include "render/formats/de_cooked_mesh.h"
#include "render/formats/de_cooked_model.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- Files ----------------------------------------------------------------------------------- */

static void fbx_mount(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("fbx", DE_IMPORT_TEST_DATA_DIR "/fbx", 0, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("ftmp", DE_IMPORT_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    mounted = true;
}

/* Reads fbx:/<rel> into a buffer of the reader (freed by reader_free). */
typedef struct reader {
    const de_allocator *alloc;
    void *bufs[16];
    u64 sizes[16];
    u32 count;
} reader;

static de_result reader_read(void *user, const char *rel, const void **data, u64 *size) {
    reader *r = (reader *)user;
    char path[512];
    snprintf(path, sizeof path, "fbx:/%s", rel);
    if (r->count == 16)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "test reader: too many files");
    void *p = NULL;
    u64 n = 0;
    DE_TRY(de_file_read_all(path, r->alloc, &p, &n));
    r->bufs[r->count] = p;
    r->sizes[r->count] = n;
    r->count++;
    *data = p;
    *size = n;
    return DE_OK;
}

static void reader_free(reader *r) {
    for (u32 i = 0; i < r->count; ++i)
        DE_FREE(r->alloc, r->bufs[i], (size_t)r->sizes[i] + 1u);
    r->count = 0;
}

/* ---- Fixture --------------------------------------------------------------------------------- */

typedef struct fbx_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    reader rd;
    de_model_ir ir, ref;
    de_cooker *c;
    de_task_system *tasks;
    de_type_registry *types;
    de_world *w;
    de_asset_system *s;
} fbx_fx;

static void fbx_fx_setup(fbx_fx *f) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    fbx_mount();
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    f->rd.alloc = &f->alloc;
    de_model_ir_init(&f->ir, &f->alloc, "test");
    de_model_ir_init(&f->ref, &f->alloc, "ref");
}

static void fbx_fx_teardown(fbx_fx *f) {
    de_model_ir_free(&f->ir);
    de_model_ir_free(&f->ref);
    reader_free(&f->rd);
    if (f->c != NULL)
        de_cooker_destroy(f->c);
    if (f->s != NULL)
        de_asset_system_destroy(f->s);
    if (f->w != NULL)
        de_world_destroy(f->w);
    if (f->tasks != NULL)
        de_task_system_destroy(f->tasks);
    if (f->types != NULL)
        de_type_registry_destroy(f->types);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    de_log_shutdown();
}

/* Loads fbx:/<name> into `ir` the way the importer does: the FBX front end (engine axes,
 * metres), then the shared conversion with `scale`, then the bounds. */
static de_result load_fbx(fbx_fx *f, de_model_ir *ir, const char *name, const char *up,
                          const char *front, f32 scale) {
    const void *data = NULL;
    u64 size = 0;
    DE_TRY(reader_read(&f->rd, name, &data, &size));
    de_model_source src = {data, size, name, reader_read, &f->rd};
    de_model_fbx_options opt = {up, front};
    DE_TRY(de_model_fbx_load(&src, &opt, ir));
    de_model_ir_convert_axes(ir, DE_MODEL_AXES_ENGINE, scale);
    de_model_ir_compute_bounds(ir);
    return DE_OK;
}

static de_result load_gltf(fbx_fx *f, de_model_ir *ir, const char *name) {
    const void *data = NULL;
    u64 size = 0;
    DE_TRY(reader_read(&f->rd, name, &data, &size));
    de_model_source src = {data, size, name, reader_read, &f->rd};
    DE_TRY(de_model_gltf_load(&src, ir));
    de_model_ir_convert_axes(ir, DE_MODEL_AXES_GLTF, 1.0f);
    de_model_ir_compute_bounds(ir);
    return DE_OK;
}

static bool has_warning(const de_model_ir *ir, const char *needle) {
    for (u32 i = 0; i < ir->warning_count; ++i)
        if (strstr(ir->warnings[i], needle) != NULL)
            return true;
    return false;
}

static i32 node_named(const de_model_ir *ir, const char *name) {
    for (u32 i = 0; i < ir->node_count; ++i)
        if (strcmp(ir->nodes[i].name, name) == 0)
            return (i32)i;
    return -1;
}

static de_result fake_texture_import(de_asset_import_ctx *ctx) {
    u64 n = 0;
    (void)de_asset_import_source(ctx, &n);
    char head[128];
    int k = snprintf(head, sizeof head, "FAKETEX srgb=%d usage=%s size=%llu",
                     de_asset_import_setting_bool(ctx, "srgb", false) ? 1 : 0,
                     de_asset_import_setting_string(ctx, "usage", "?"), (unsigned long long)n);
    return de_asset_import_write(ctx, head, (u64)k);
}

/* The cooker with the model importer, the stand-in texture importer and (with `prefab`) the
 * runtime's prefab importer. */
static void make_cooker(fbx_fx *f, bool prefab) {
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.allocator = &f->alloc,
                                                 .verify_determinism = DE_COOK_VERIFY_ON},
                               &f->c),
              DE_OK);
    de_asset_type_desc model = {.name = "model",
                                .extensions = de_model_import_extensions,
                                .extension_count = DE_MODEL_IMPORT_EXTENSION_COUNT,
                                .importer_version = DE_MODEL_IMPORTER_VERSION,
                                .import = de_model_import};
    ASSERT_EQ(de_cooker_register_type(f->c, &model), DE_OK);
    de_asset_type_desc t = {
        .name = "texture", .importer_version = 1, .import = fake_texture_import};
    ASSERT_EQ(de_cooker_register_type(f->c, &t), DE_OK);
    if (!prefab)
        return;
    ASSERT_EQ(de_type_registry_create(&f->alloc, &f->types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &f->alloc, .types = f->types}, &f->w),
              DE_OK);
    de_fp_env_init_thread();
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 1}, &f->tasks), DE_OK);
    de_asset_system_desc d = {.allocator = &f->alloc,
                              .tasks = f->tasks,
                              .io_threads = 1,
                              .cache_dir = "nocache:/decache",
                              .types = f->types};
    ASSERT_EQ(de_asset_system_create(&d, &f->s), DE_OK);
    const de_asset_type_desc *td = de__asset_type_get(f->s, de__asset_type_find(f->s, "prefab"));
    ASSERT_TRUE(td != NULL);
    ASSERT_EQ(de_cooker_register_type(f->c, td), DE_OK);
}

static de_result cook(fbx_fx *f, const char *id, const char *source, const de__manifest_kv *kv,
                      u32 kv_count, de_cook_result *out) {
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = id;
    a.type = "model";
    a.source = source;
    a.file = "fbx:/assets.toml";
    a.settings = kv;
    a.setting_count = kv_count;
    return de_cooker_cook(f->c, &(de_cook_input){.asset = &a, .asset_root = "fbx:/"}, out);
}

static de__manifest_kv kv_str(const char *key, const char *value) {
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = key;
    kv.value.type = DE__MANIFEST_STRING;
    kv.value.u.s = value;
    return kv;
}

/* ---- Geometry helpers ------------------------------------------------------------------------ */

/* The world matrix (column-major 4x4, f64) of node `n`: parent chain of T·R·S. */
static void node_world(const de_model_ir *ir, i32 n, f64 out[16]) {
    f64 m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (u32 guard = 0; n >= 0 && guard < 64u; ++guard) {
        const de_model_node *nd = &ir->nodes[n];
        f64 x = nd->rotation[0], y = nd->rotation[1], z = nd->rotation[2], w = nd->rotation[3];
        f64 r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w),     2 * (x * z - y * w),
                    2 * (x * y - z * w),     1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                    2 * (x * z + y * w),     2 * (y * z - x * w),     1 - 2 * (x * x + y * y)};
        f64 l[16];
        for (u32 c = 0; c < 3; ++c) {
            for (u32 k = 0; k < 3; ++k)
                l[4 * c + k] = r[3 * c + k] * nd->scale[c];
            l[4 * c + 3] = 0.0;
        }
        for (u32 k = 0; k < 3; ++k)
            l[12 + k] = nd->translation[k];
        l[15] = 1.0;
        f64 p[16];
        for (u32 c = 0; c < 4; ++c)
            for (u32 k = 0; k < 4; ++k) {
                f64 acc = 0.0;
                for (u32 j = 0; j < 4; ++j)
                    acc += l[4 * j + k] * m[4 * c + j];
                p[4 * c + k] = acc;
            }
        memcpy(m, p, sizeof m);
        n = nd->parent;
    }
    memcpy(out, m, sizeof m);
}

static void xform_point(const f64 m[16], const f32 *p, f64 o[3]) {
    for (u32 k = 0; k < 3; ++k)
        o[k] = m[k] * p[0] + m[4 + k] * p[1] + m[8 + k] * p[2] + m[12 + k];
}

static void xform_dir(const f64 m[16], const f32 *d, f64 o[3]) {
    f64 l = 0.0;
    for (u32 k = 0; k < 3; ++k) {
        o[k] = m[k] * d[0] + m[4 + k] * d[1] + m[8 + k] * d[2];
        l += o[k] * o[k];
    }
    l = sqrt(l);
    for (u32 k = 0; k < 3; ++k)
        o[k] /= l;
}

/* A world-space vertex: position, normal, uv0. */
typedef struct wvert {
    f64 p[3], n[3], uv[2];
} wvert;

/* The world-space vertices of the first node with a mesh (at most `cap`). */
static u32 world_verts(const de_model_ir *ir, wvert *out, u32 cap) {
    for (u32 i = 0; i < ir->node_count; ++i) {
        i32 mi = ir->nodes[i].mesh;
        if (mi < 0)
            continue;
        const de_model_mesh *m = &ir->meshes[mi];
        f64 w[16];
        node_world(ir, (i32)i, w);
        u32 n = m->vertex_count < cap ? m->vertex_count : cap;
        for (u32 v = 0; v < n; ++v) {
            xform_point(w, m->position + 3u * v, out[v].p);
            if (m->normal != NULL)
                xform_dir(w, m->normal + 3u * v, out[v].n);
            for (u32 k = 0; k < 2; ++k)
                out[v].uv[k] = m->uv[0] != NULL ? m->uv[0][2u * v + k] : 0.0;
        }
        return n;
    }
    return 0;
}

static bool near3(const f64 *a, const f64 *b, f64 eps) {
    return fabs(a[0] - b[0]) <= eps && fabs(a[1] - b[1]) <= eps && fabs(a[2] - b[2]) <= eps;
}

/* True when the two vertex sets are equal (any order) within the tolerances; `verbose` prints
 * the first vertex without a match. */
static bool same_vertex_sets(const wvert *a, u32 na, const wvert *b, u32 nb, bool verbose) {
    if (na != nb)
        return false;
    bool used[64] = {false};
    for (u32 i = 0; i < na; ++i) {
        bool found = false;
        for (u32 j = 0; j < nb && !found; ++j) {
            if (used[j] || !near3(a[i].p, b[j].p, 1e-4) || !near3(a[i].n, b[j].n, 1e-5) ||
                fabs(a[i].uv[0] - b[j].uv[0]) > 1e-6 || fabs(a[i].uv[1] - b[j].uv[1]) > 1e-6)
                continue;
            used[j] = found = true;
        }
        if (!found) {
            if (verbose)
                printf("  no match for vertex %u (%g %g %g)\n", i, a[i].p[0], a[i].p[1], a[i].p[2]);
            return false;
        }
    }
    return true;
}

/* Every triangle's face normal (from the winding) points the way its vertex normals do. */
static bool winding_matches_normals(const de_model_mesh *m) {
    for (u32 t = 0; t + 2u < m->index_count; t += 3u) {
        const f32 *p0 = m->position + 3u * m->indices[t];
        const f32 *p1 = m->position + 3u * m->indices[t + 1u];
        const f32 *p2 = m->position + 3u * m->indices[t + 2u];
        f64 e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        f64 e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        f64 n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                    e1[0] * e2[1] - e1[1] * e2[0]};
        const f32 *vn = m->normal + 3u * m->indices[t];
        if (n[0] * vn[0] + n[1] * vn[1] + n[2] * vn[2] <= 0.0)
            return false;
    }
    return true;
}

/* The box of gen_fixtures.py in engine space: local x in [-0.5, 0.5], y in [-1, 1], z in [0, 4];
 * the node at (2, 3, 0.5), rotated 30 degrees about +Z. */
static void expect_box(const de_model_ir *ir) {
    ASSERT_EQ(ir->mesh_count, 1u);
    const de_model_mesh *m = &ir->meshes[0];
    EXPECT_STREQ(m->name, "Box");
    EXPECT_EQ(m->vertex_count, 24u);
    EXPECT_EQ(m->index_count, 36u);
    ASSERT_EQ(m->submesh_count, 1u);
    EXPECT_EQ(m->submeshes[0].material, -1);
    EXPECT_EQ(m->attr_mask, DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_POSITION) |
                                DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_NORMAL) |
                                DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_UV0));
    const f32 lo[3] = {-0.5f, -1.0f, 0.0f}, hi[3] = {0.5f, 1.0f, 4.0f};
    for (u32 k = 0; k < 3; ++k) {
        EXPECT_FLOAT_EQ(m->bounds_min[k], lo[k], 1e-5f);
        EXPECT_FLOAT_EQ(m->bounds_max[k], hi[k], 1e-5f);
    }
    EXPECT_TRUE(winding_matches_normals(m));
    ASSERT_EQ(ir->node_count, 1u);
    const de_model_node *n = &ir->nodes[0];
    EXPECT_STREQ(n->name, "Box");
    EXPECT_EQ(n->mesh, 0);
    EXPECT_EQ(n->parent, -1);
    EXPECT_EQ(ir->root_count, 1u);
    EXPECT_FLOAT_EQ(n->translation[0], 2.0f, 1e-5f);
    EXPECT_FLOAT_EQ(n->translation[1], 3.0f, 1e-5f);
    EXPECT_FLOAT_EQ(n->translation[2], 0.5f, 1e-5f);
    f32 s15 = sinf(0.2617993878f), c15 = cosf(0.2617993878f);
    EXPECT_FLOAT_EQ(n->rotation[0], 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ(n->rotation[1], 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ(n->rotation[2], s15, 1e-6f);
    EXPECT_FLOAT_EQ(n->rotation[3], c15, 1e-6f);
    for (u32 k = 0; k < 3; ++k)
        EXPECT_FLOAT_EQ(n->scale[k], 1.0f, 1e-6f);
}

/* ---- import_fbx.fbx.import_cube_yup_cm ------------------------------------------------------- */

TEST_F(fbx, import_cube_yup_cm, fbx_fx) {
    /* Binary, Maya axes (Y-up, +Z front), centimetres. */
    ASSERT_EQ(load_fbx(f, &f->ir, "cube_yup_cm.fbx", NULL, NULL, 1.0f), DE_OK);
    expect_box(&f->ir);
    EXPECT_EQ(f->ir.warning_count, 0u);
}

/* ---- import_fbx.fbx.import_cube_zup_m -------------------------------------------------------- */

TEST_F(fbx, import_cube_zup_m, fbx_fx) {
    /* ASCII, 3ds Max axes (Z-up, -Y front), metres. */
    ASSERT_EQ(load_fbx(f, &f->ir, "cube_zup_m.fbx", "auto", "auto", 1.0f), DE_OK);
    expect_box(&f->ir);
    EXPECT_EQ(f->ir.warning_count, 0u);
}

/* ---- import_fbx.fbx.matches_gltf_reference --------------------------------------------------- */

TEST_F(fbx, matches_gltf_reference, fbx_fx) {
    /* The same box through the glTF importer (DENG-101): world-space positions within 1e-4 m,
     * normals and UVs (FBX's bottom-left UV origin flipped to glTF's) the same. */
    ASSERT_EQ(load_gltf(f, &f->ref, "cube_ref.gltf"), DE_OK);
    wvert ref[64], got[64];
    u32 nref = world_verts(&f->ref, ref, 64);
    ASSERT_EQ(nref, 24u);
    /* The left-handed file is mirrored into engine axes: the winding flips with it, so the
     * triangles still face along their normals. */
    const char *files[] = {"cube_yup_cm.fbx", "cube_zup_m.fbx", "cube_lefthanded.fbx"};
    for (u32 i = 0; i < 3; ++i) {
        de_model_ir_free(&f->ir);
        ASSERT_EQ(load_fbx(f, &f->ir, files[i], NULL, NULL, 1.0f), DE_OK);
        u32 n = world_verts(&f->ir, got, 64);
        EXPECT_TRUE(same_vertex_sets(got, n, ref, nref, true));
        EXPECT_TRUE(winding_matches_normals(&f->ir.meshes[0]));
        EXPECT_EQ(has_warning(&f->ir, "left-handed"), i == 2);
        /* Local geometry and node transform agree too, not only their product. */
        const de_model_mesh *a = &f->ir.meshes[0], *b = &f->ref.meshes[0];
        for (u32 k = 0; k < 3; ++k) {
            EXPECT_FLOAT_EQ(a->bounds_min[k], b->bounds_min[k], 1e-4f);
            EXPECT_FLOAT_EQ(a->bounds_max[k], b->bounds_max[k], 1e-4f);
            EXPECT_FLOAT_EQ(f->ir.nodes[0].translation[k], f->ref.nodes[0].translation[k], 1e-4f);
        }
        for (u32 k = 0; k < 4; ++k)
            EXPECT_FLOAT_EQ(f->ir.nodes[0].rotation[k], f->ref.nodes[0].rotation[k], 1e-6f);
    }
}

/* ---- import_fbx.fbx.axis_override ------------------------------------------------------------ */

TEST_F(fbx, axis_override, fbx_fx) {
    ASSERT_EQ(load_gltf(f, &f->ref, "cube_ref.gltf"), DE_OK);
    wvert ref[64], got[64];
    u32 nref = world_verts(&f->ref, ref, 64);

    /* cube_wrong_axes.fbx holds Z-up data but says Y-up: as the file says, it is wrong. */
    ASSERT_EQ(load_fbx(f, &f->ir, "cube_wrong_axes.fbx", NULL, NULL, 1.0f), DE_OK);
    u32 n = world_verts(&f->ir, got, 64);
    EXPECT_FALSE(same_vertex_sets(got, n, ref, nref, false));

    /* axis_up = "z" (front: the usual -Y for Z-up): right. */
    de_model_ir_free(&f->ir);
    ASSERT_EQ(load_fbx(f, &f->ir, "cube_wrong_axes.fbx", "z", "auto", 1.0f), DE_OK);
    n = world_verts(&f->ir, got, 64);
    EXPECT_TRUE(same_vertex_sets(got, n, ref, nref, true));

    /* axis_front = "+y": the model faces the other way, a half turn about up. */
    de_model_ir_free(&f->ir);
    ASSERT_EQ(load_fbx(f, &f->ir, "cube_wrong_axes.fbx", "+Z", "+y", 1.0f), DE_OK);
    n = world_verts(&f->ir, got, 64);
    for (u32 i = 0; i < nref; ++i) {
        ref[i].p[0] = -ref[i].p[0];
        ref[i].p[1] = -ref[i].p[1];
        ref[i].n[0] = -ref[i].n[0];
        ref[i].n[1] = -ref[i].n[1];
    }
    EXPECT_TRUE(same_vertex_sets(got, n, ref, nref, true));
    EXPECT_TRUE(winding_matches_normals(&f->ir.meshes[0]));

    /* Bad overrides. */
    de_model_ir_free(&f->ir);
    EXPECT_EQ(load_fbx(f, &f->ir, "cube_wrong_axes.fbx", "w", NULL, 1.0f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(de_error_last()->message, "axis_up") != NULL);
    de_model_ir_free(&f->ir);
    EXPECT_EQ(load_fbx(f, &f->ir, "cube_wrong_axes.fbx", "z", "-z", 1.0f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(de_error_last()->message, "axis_front") != NULL);

    /* Through the cooker: the overrides and `scale` change the output and the settings hash. */
    make_cooker(f, false);
    de_cook_result plain, up, front, scaled;
    ASSERT_EQ(cook(f, "env/box", "cube_wrong_axes.fbx", NULL, 0, &plain), DE_OK);
    de__manifest_kv kv[2] = {kv_str("axis_up", "z"), kv_str("axis_front", "+y")};
    ASSERT_EQ(cook(f, "env/box", "cube_wrong_axes.fbx", kv, 1, &up), DE_OK);
    ASSERT_EQ(cook(f, "env/box", "cube_wrong_axes.fbx", kv, 2, &front), DE_OK);
    de__manifest_kv sc[2] = {kv_str("axis_up", "z")};
    sc[1].key = "scale";
    sc[1].value.type = DE__MANIFEST_FLOAT;
    sc[1].value.u.f = 0.5;
    ASSERT_EQ(cook(f, "env/box", "cube_wrong_axes.fbx", sc, 2, &scaled), DE_OK);
    EXPECT_NE(plain.settings_hash, up.settings_hash);
    EXPECT_NE(up.settings_hash, front.settings_hash);
    EXPECT_NE(up.settings_hash, scaled.settings_hash);
    EXPECT_NE(plain.output_hash, up.output_hash);
    EXPECT_NE(up.output_hash, front.output_hash);
    EXPECT_NE(up.output_hash, scaled.output_hash);
    /* The model's node: translation (2, 3, 0.5) with axis_up = z, halved with scale 0.5. */
    de_cooked_model_view mv;
    ASSERT_TRUE(de_cooked_model_parse(up.data, up.size, &mv, NULL));
    de_cooked_model_node nd = de_cooked_model_get_node(&mv, 0);
    EXPECT_FLOAT_EQ(nd.translation[0], 2.0f, 1e-5f);
    EXPECT_FLOAT_EQ(nd.translation[1], 3.0f, 1e-5f);
    EXPECT_FLOAT_EQ(nd.translation[2], 0.5f, 1e-5f);
    ASSERT_TRUE(de_cooked_model_parse(scaled.data, scaled.size, &mv, NULL));
    nd = de_cooked_model_get_node(&mv, 0);
    EXPECT_FLOAT_EQ(nd.translation[0], 1.0f, 1e-5f);
    EXPECT_FLOAT_EQ(nd.translation[1], 1.5f, 1e-5f);
    EXPECT_FLOAT_EQ(nd.translation[2], 0.25f, 1e-5f);
    const de_cook_subasset *mesh = de_cook_result_subasset(&scaled, "Box");
    ASSERT_TRUE(mesh != NULL);
    de_cooked_mesh_view mesh_v;
    ASSERT_TRUE(de_cooked_mesh_parse(mesh->data, mesh->size, &mesh_v, NULL));
    EXPECT_FLOAT_EQ(mesh_v.bounds_max[2], 2.0f, 1e-5f);
    de_cook_result_free(&plain);
    de_cook_result_free(&up);
    de_cook_result_free(&front);
    de_cook_result_free(&scaled);

    /* A bad override fails the cook. */
    de__manifest_kv bad[1] = {kv_str("axis_front", "sideways")};
    de_cook_result r;
    EXPECT_EQ(cook(f, "env/box", "cube_wrong_axes.fbx", bad, 1, &r), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(de_error_last()->message, "env/box") != NULL);
}

/* ---- import_fbx.fbx.ngon_triangulate --------------------------------------------------------- */

TEST_F(fbx, ngon_triangulate, fbx_fx) {
    /* A pentagon and a triangle with Mat0, a hexagon and a quad with Mat1, a line face (Mat0) that
     * has no triangles; no normals in the file. */
    ASSERT_EQ(load_fbx(f, &f->ir, "ngon_two_mats.fbx", NULL, NULL, 1.0f), DE_OK);
    de_model_ir *ir = &f->ir;
    ASSERT_EQ(ir->material_count, 2u);
    EXPECT_STREQ(ir->materials[0].name, "Mat0");
    EXPECT_STREQ(ir->materials[1].name, "Mat1");
    ASSERT_EQ(ir->mesh_count, 1u);
    const de_model_mesh *m = &ir->meshes[0];
    ASSERT_EQ(m->submesh_count, 2u);
    EXPECT_EQ(m->index_count, 30u);
    EXPECT_EQ(m->vertex_count, 18u);
    const de_model_submesh *s0 = &m->submeshes[0], *s1 = &m->submeshes[1];
    EXPECT_EQ(s0->material, 0);
    EXPECT_EQ(s0->index_first, 0u);
    EXPECT_EQ(s0->index_count, 12u); /* 3 + 1 triangles */
    EXPECT_EQ(s0->vertex_first, 0u);
    EXPECT_EQ(s0->vertex_count, 8u);
    EXPECT_EQ(s1->material, 1);
    EXPECT_EQ(s1->index_first, 12u);
    EXPECT_EQ(s1->index_count, 18u); /* 4 + 2 triangles */
    EXPECT_EQ(s1->vertex_first, 8u);
    EXPECT_EQ(s1->vertex_count, 10u);
    /* Every index stays inside its submesh's vertex range. */
    for (u32 s = 0; s < 2; ++s) {
        const de_model_submesh *sm = &m->submeshes[s];
        for (u32 i = 0; i < sm->index_count; ++i) {
            u32 v = m->indices[sm->index_first + i];
            EXPECT_TRUE(v >= sm->vertex_first && v < sm->vertex_first + sm->vertex_count);
        }
    }
    /* Generated normals: the shapes lie in z = 0, counter-clockwise from +Z. */
    ASSERT_TRUE(m->normal != NULL);
    for (u32 v = 0; v < m->vertex_count; ++v)
        EXPECT_FLOAT_EQ(m->normal[3u * v + 2u], 1.0f, 1e-5f);
    EXPECT_TRUE(winding_matches_normals(m));
    /* Lambert: no specular, rough, not metallic. */
    EXPECT_FLOAT_EQ(ir->materials[0].params.base_color[0], 0.8f, 1e-6f);
    EXPECT_FLOAT_EQ(ir->materials[0].params.roughness, 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(ir->materials[0].params.metallic, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(ir->materials[0].params.specular, 0.0f, 0.0f);

    /* Cooked: two submeshes with their material IDs. */
    make_cooker(f, false);
    de_cook_result r;
    ASSERT_EQ(cook(f, "props/shapes", "ngon_two_mats.fbx", NULL, 0, &r), DE_OK);
    const de_cook_subasset *mesh = de_cook_result_subasset(&r, "Shapes");
    ASSERT_TRUE(mesh != NULL);
    de_cooked_mesh_view v;
    ASSERT_TRUE(de_cooked_mesh_parse(mesh->data, mesh->size, &v, NULL));
    EXPECT_EQ(v.submesh_count, 2u);
    EXPECT_EQ(v.index_count, 30u);
    EXPECT_EQ(de_cooked_mesh_submesh(&v, 1).material_id,
              de_asset_id_from("props/shapes#material:Mat1").hash);
    de_cook_result_free(&r);
}

/* ---- import_fbx.fbx.material_pbr_map --------------------------------------------------------- */

TEST_F(fbx, material_pbr_map, fbx_fx) {
    ASSERT_EQ(load_fbx(f, &f->ir, "materials.fbx", NULL, NULL, 1.0f), DE_OK);
    de_model_ir *ir = &f->ir;
    ASSERT_EQ(ir->material_count, 3u);
    const de_model_material *m = &ir->materials[2];
    EXPECT_STREQ(m->name, "OpenPBR");
    const de_cooked_material_params *p = &m->params;
    /* base_color × base_weight (0.5); opacity 0.6 → blended alpha. */
    EXPECT_FLOAT_EQ(p->base_color[0], 0.3f, 1e-6f);
    EXPECT_FLOAT_EQ(p->base_color[1], 0.2f, 1e-6f);
    EXPECT_FLOAT_EQ(p->base_color[2], 0.1f, 1e-6f);
    EXPECT_FLOAT_EQ(p->base_color[3], 0.6f, 1e-6f);
    EXPECT_EQ(m->alpha_mode, (u32)DE_COOKED_ALPHA_BLEND);
    EXPECT_FLOAT_EQ(p->metallic, 0.8f, 1e-6f);
    EXPECT_FLOAT_EQ(p->roughness, 0.3f, 1e-6f);
    EXPECT_FLOAT_EQ(p->specular, 0.7f, 1e-6f);
    EXPECT_FLOAT_EQ(p->specular_color[1], 0.9f, 1e-6f);
    EXPECT_FLOAT_EQ(p->ior, 1.45f, 1e-6f);
    EXPECT_FLOAT_EQ(p->transmission, 0.2f, 1e-6f);
    EXPECT_FLOAT_EQ(p->clearcoat, 0.4f, 1e-6f);
    EXPECT_FLOAT_EQ(p->clearcoat_roughness, 0.15f, 1e-6f);
    /* fuzz_color × fuzz_weight (0.5). */
    EXPECT_FLOAT_EQ(p->sheen_color[0], 0.1f, 1e-6f);
    EXPECT_FLOAT_EQ(p->sheen_color[2], 0.3f, 1e-6f);
    EXPECT_FLOAT_EQ(p->sheen_roughness, 0.35f, 1e-6f);
    /* emission_color × emission_weight (3): a unit colour and a strength. */
    EXPECT_FLOAT_EQ(p->emissive[0], 1.0f, 1e-6f);
    EXPECT_FLOAT_EQ(p->emissive[1], 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(p->emissive[2], 0.25f, 1e-6f);
    EXPECT_FLOAT_EQ(p->emissive_strength, 3.0f, 1e-6f);
    EXPECT_EQ(m->flags,
              (u32)(DE_COOKED_MAT_SPECULAR | DE_COOKED_MAT_IOR | DE_COOKED_MAT_EMISSIVE_STRENGTH |
                    DE_COOKED_MAT_CLEARCOAT | DE_COOKED_MAT_SHEEN | DE_COOKED_MAT_TRANSMISSION));
    /* Its base colour texture is the embedded checker image Phong uses too: one texture. */
    ASSERT_GE(m->tex[DE_COOKED_TEX_BASE_COLOR].texture, 0);
    EXPECT_EQ(m->tex[DE_COOKED_TEX_BASE_COLOR].texture,
              ir->materials[0].tex[DE_COOKED_TEX_BASE_COLOR].texture);
}

/* ---- import_fbx.fbx.material_legacy_phong ---------------------------------------------------- */

TEST_F(fbx, material_legacy_phong, fbx_fx) {
    ASSERT_EQ(load_fbx(f, &f->ir, "materials.fbx", NULL, NULL, 1.0f), DE_OK);
    de_model_ir *ir = &f->ir;
    const de_model_material *ph = &ir->materials[0];
    EXPECT_STREQ(ph->name, "Phong");
    /* Diffuse (0.8, 0.4, 0.2) × 0.5; TransparencyFactor 0.25 → alpha 0.75. */
    EXPECT_FLOAT_EQ(ph->params.base_color[0], 0.4f, 1e-6f);
    EXPECT_FLOAT_EQ(ph->params.base_color[1], 0.2f, 1e-6f);
    EXPECT_FLOAT_EQ(ph->params.base_color[2], 0.1f, 1e-6f);
    EXPECT_FLOAT_EQ(ph->params.base_color[3], 0.75f, 1e-6f);
    EXPECT_EQ(ph->alpha_mode, (u32)DE_COOKED_ALPHA_BLEND);
    EXPECT_FLOAT_EQ(ph->params.metallic, 0.0f, 0.0f);
    /* Exponent 30: roughness (2 / 32)^(1/4) = 0.5. */
    EXPECT_FLOAT_EQ(ph->params.roughness, 0.5f, 1e-6f);
    /* Specular (1, 0.5, 0.25) × 0.25. */
    EXPECT_FLOAT_EQ(ph->params.specular, 0.25f, 1e-6f);
    EXPECT_FLOAT_EQ(ph->params.specular_color[0], 1.0f, 1e-6f);
    EXPECT_FLOAT_EQ(ph->params.specular_color[2], 0.25f, 1e-6f);
    /* Emissive (1, 0.5, 0) × 2. */
    EXPECT_FLOAT_EQ(ph->params.emissive[1], 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(ph->params.emissive_strength, 2.0f, 1e-6f);
    EXPECT_EQ(ph->flags, (u32)(DE_COOKED_MAT_SPECULAR | DE_COOKED_MAT_EMISSIVE_STRENGTH));

    /* Textures: the embedded diffuse (colour) and the external normal map (read next to the
     * file); Lambert's texture file does not exist (a warning, no slot). */
    ASSERT_EQ(ir->texture_count, 2u);
    i32 base = ph->tex[DE_COOKED_TEX_BASE_COLOR].texture,
        nrm = ph->tex[DE_COOKED_TEX_NORMAL].texture;
    ASSERT_EQ(base, 0);
    ASSERT_EQ(nrm, 1);
    EXPECT_STREQ(ir->textures[0].name, "checker");
    EXPECT_EQ(ir->textures[0].usage, DE_MODEL_TEX_COLOR);
    EXPECT_STREQ(ir->textures[0].mime, "image/png");
    EXPECT_GT(ir->textures[0].size, 8u);
    EXPECT_MEMEQ(ir->textures[0].data, "\x89PNG", 4);
    EXPECT_STREQ(ir->textures[1].name, "normal");
    EXPECT_EQ(ir->textures[1].usage, DE_MODEL_TEX_NORMAL);
    EXPECT_MEMEQ(ir->textures[1].data, "\x89PNG", 4);
    const de_model_material *la = &ir->materials[1];
    EXPECT_STREQ(la->name, "Lambert");
    EXPECT_EQ(la->tex[DE_COOKED_TEX_BASE_COLOR].texture, -1);
    EXPECT_TRUE(has_warning(ir, "missing.png"));
    EXPECT_FLOAT_EQ(la->params.base_color[2], 0.6f, 1e-6f);
    EXPECT_FLOAT_EQ(la->params.roughness, 1.0f, 0.0f);

    /* The normal-mapped submesh gets MikkTSpace tangents; the UVs are flipped to the top-left
     * origin: the panel's first corner (FBX uv (0, 1)) is uv (0, 0) here. */
    const de_model_mesh *m = &ir->meshes[0];
    ASSERT_EQ(m->submesh_count, 3u);
    EXPECT_TRUE(m->tangent != NULL);
    EXPECT_TRUE(m->submeshes[0].attr_mask & DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_TANGENT));
    EXPECT_FALSE(m->submeshes[1].attr_mask & DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_TANGENT));
    ASSERT_TRUE(m->uv[0] != NULL);
    EXPECT_FLOAT_EQ(m->uv[0][0], 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(m->uv[0][1], 0.0f, 0.0f);

    /* Cooked: texture sub-assets with the inferred settings, the normal map read as an extra
     * source of the cook. */
    make_cooker(f, false);
    de_cook_result r;
    ASSERT_EQ(cook(f, "props/panel", "materials.fbx", NULL, 0, &r), DE_OK);
    const de_cook_subasset *t0 = de_cook_result_subasset(&r, "texture:checker");
    const de_cook_subasset *t1 = de_cook_result_subasset(&r, "texture:normal");
    ASSERT_TRUE(t0 != NULL && t1 != NULL);
    EXPECT_TRUE(strstr((const char *)t0->data, "srgb=1 usage=color") != NULL);
    EXPECT_TRUE(strstr((const char *)t1->data, "srgb=0 usage=normal") != NULL);
    bool normal_src = false;
    for (u32 i = 0; i < r.extra_source_count; ++i)
        normal_src = normal_src || strcmp(r.extra_sources[i], "normal.png") == 0;
    EXPECT_TRUE(normal_src);
    const de_cook_subasset *mat = de_cook_result_subasset(&r, "material:Phong");
    ASSERT_TRUE(mat != NULL);
    de_cooked_material_view mv;
    ASSERT_TRUE(de_cooked_material_parse(mat->data, mat->size, &mv, NULL));
    EXPECT_EQ(mv.texture_count, 2u);
    EXPECT_FLOAT_EQ(mv.params.roughness, 0.5f, 1e-6f);
    de_cook_result_free(&r);
}

/* ---- import_fbx.fbx.skin_ir ------------------------------------------------------------------ */

TEST_F(fbx, skin_ir, fbx_fx) {
    /* Maya axes, centimetres: a 2 m strip up engine +Z skinned to a chain of 5 bones 0.5 m
     * apart. */
    ASSERT_EQ(load_fbx(f, &f->ir, "skinned.fbx", NULL, NULL, 1.0f), DE_OK);
    de_model_ir *ir = &f->ir;
    ASSERT_EQ(ir->skin_count, 1u);
    const de_model_skin *sk = &ir->skins[0];
    ASSERT_EQ(sk->joint_count, 5u);
    const char *names[5] = {"Root", "Bone", "B2", "B3", "B4"};
    for (u32 j = 0; j < 5; ++j) {
        EXPECT_STREQ(ir->nodes[sk->joints[j]].name, names[j]);
        /* Inverse bind: the inverse of the bone's bind position (0, 0, 0.5 j) in metres. */
        const f32 *ib = sk->inverse_bind + 16u * j;
        EXPECT_FLOAT_EQ(ib[0], 1.0f, 1e-6f);
        EXPECT_FLOAT_EQ(ib[5], 1.0f, 1e-6f);
        EXPECT_FLOAT_EQ(ib[10], 1.0f, 1e-6f);
        EXPECT_FLOAT_EQ(ib[15], 1.0f, 0.0f);
        EXPECT_FLOAT_EQ(ib[12], 0.0f, 1e-6f);
        EXPECT_FLOAT_EQ(ib[13], 0.0f, 1e-6f);
        EXPECT_FLOAT_EQ(ib[14], -0.5f * (f32)j, 1e-6f);
    }
    /* The bones' local translations: 0.5 m along engine +Z. */
    i32 bone = node_named(ir, "Bone");
    ASSERT_GE(bone, 0);
    EXPECT_EQ(ir->nodes[bone].parent, node_named(ir, "Root"));
    EXPECT_FLOAT_EQ(ir->nodes[bone].translation[2], 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(ir->nodes[bone].translation[1], 0.0f, 1e-6f);
    i32 body = node_named(ir, "Body");
    ASSERT_GE(body, 0);
    EXPECT_EQ(ir->nodes[body].skin, 0);

    const de_model_mesh *m = &ir->meshes[0];
    EXPECT_TRUE(m->attr_mask & DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_JOINTS0));
    EXPECT_TRUE(m->attr_mask & DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_WEIGHTS0));
    EXPECT_FLOAT_EQ(m->bounds_max[2], 2.0f, 1e-5f);
    EXPECT_FLOAT_EQ(m->bounds_max[0], 1.0f, 1e-5f);
    /* Every vertex's weights sum to 1; the vertex at (1, 0, 2) had 5 influences and keeps the
     * 4 largest, renormalised: 0.4, 0.3, 0.15, 0.1 over 0.95, joints Bone, B2, B3, B4. */
    bool found = false;
    for (u32 v = 0; v < m->vertex_count; ++v) {
        const f32 *w = m->weights0 + 4u * v;
        EXPECT_FLOAT_EQ(w[0] + w[1] + w[2] + w[3], 1.0f, 1e-6f);
        const f32 *p = m->position + 3u * v;
        if (fabsf(p[0] - 1.0f) < 1e-5f && fabsf(p[2] - 2.0f) < 1e-5f) {
            found = true;
            const u16 *j = m->joints0 + 4u * v;
            EXPECT_EQ(j[0], 1u);
            EXPECT_EQ(j[1], 2u);
            EXPECT_EQ(j[2], 3u);
            EXPECT_EQ(j[3], 4u);
            EXPECT_FLOAT_EQ(w[0], 0.4f / 0.95f, 1e-6f);
            EXPECT_FLOAT_EQ(w[3], 0.1f / 0.95f, 1e-6f);
        }
    }
    EXPECT_TRUE(found);
}

/* ---- import_fbx.fbx.anim_stack_ir ------------------------------------------------------------ */

TEST_F(fbx, anim_stack_ir, fbx_fx) {
    ASSERT_EQ(load_fbx(f, &f->ir, "skinned.fbx", NULL, NULL, 1.0f), DE_OK);
    de_model_ir *ir = &f->ir;
    ASSERT_EQ(ir->anim_count, 2u);
    i32 root = node_named(ir, "Root"), bone = node_named(ir, "Bone");

    /* "Wave": 1 s; Bone turns 0 → 90 degrees about X. FBX interpolates Euler angles, so the
     * rotation is resampled at the file's 30 fps: 31 keys. */
    const de_model_anim *wave = &ir->anims[0];
    EXPECT_STREQ(wave->name, "Wave");
    EXPECT_FLOAT_EQ(wave->duration, 1.0f, 1e-6f);
    const de_model_anim_channel *rot = NULL;
    for (u32 c = 0; c < wave->channel_count; ++c) {
        EXPECT_EQ(wave->channels[c].node, (u32)bone);
        EXPECT_EQ(wave->channels[c].interp, DE_MODEL_INTERP_LINEAR);
        if (wave->channels[c].path == DE_MODEL_ANIM_ROTATION)
            rot = &wave->channels[c];
    }
    ASSERT_TRUE(rot != NULL);
    EXPECT_EQ(rot->key_count, 31u);
    EXPECT_EQ(rot->components, 4u);
    EXPECT_FLOAT_EQ(rot->times[0], 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(rot->times[30], 1.0f, 1e-6f);
    const f32 *q = rot->values + 4u * 30u;
    EXPECT_FLOAT_EQ(q[0], sqrtf(0.5f), 1e-5f);
    EXPECT_FLOAT_EQ(q[1], 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ(q[2], 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ(q[3], sqrtf(0.5f), 1e-5f);

    /* "Idle": 0.5 s; Root moves to (0, -0.5, 0) m (file +Z in cm). Linear translation keys
     * are kept as they are: 2 keys. */
    const de_model_anim *idle = &ir->anims[1];
    EXPECT_STREQ(idle->name, "Idle");
    EXPECT_FLOAT_EQ(idle->duration, 0.5f, 1e-6f);
    const de_model_anim_channel *tr = NULL;
    for (u32 c = 0; c < idle->channel_count; ++c)
        if (idle->channels[c].path == DE_MODEL_ANIM_TRANSLATION)
            tr = &idle->channels[c];
    ASSERT_TRUE(tr != NULL);
    EXPECT_EQ(tr->node, (u32)root);
    EXPECT_EQ(tr->key_count, 2u);
    const f32 *t = tr->values + 3u * (tr->key_count - 1u);
    EXPECT_FLOAT_EQ(t[0], 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ(t[1], -0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(t[2], 0.0f, 1e-6f);

    /* `scale` reaches the tracks through the shared conversion. */
    de_model_ir_free(&f->ir);
    ASSERT_EQ(load_fbx(f, &f->ir, "skinned.fbx", NULL, NULL, 2.0f), DE_OK);
    for (u32 c = 0; c < f->ir.anims[1].channel_count; ++c)
        if (f->ir.anims[1].channels[c].path == DE_MODEL_ANIM_TRANSLATION)
            tr = &f->ir.anims[1].channels[c];
    EXPECT_FLOAT_EQ(tr->values[3u * (tr->key_count - 1u) + 1u], -1.0f, 1e-6f);
}

/* ---- import_fbx.fbx.cook_deterministic ------------------------------------------------------- */

TEST_F(fbx, cook_deterministic, fbx_fx) {
    /* verify_determinism is on: every cook runs the importer twice and compares. */
    make_cooker(f, true);
    const char *files[] = {"materials.fbx", "skinned.fbx", "cube_yup_cm.fbx", "ngon_two_mats.fbx"};
    for (u32 k = 0; k < 4; ++k) {
        de_cook_result a, b;
        ASSERT_EQ(cook(f, "props/model", files[k], NULL, 0, &a), DE_OK);
        ASSERT_EQ(cook(f, "props/model", files[k], NULL, 0, &b), DE_OK);
        EXPECT_EQ(a.output_hash, b.output_hash);
        ASSERT_EQ(a.size, b.size);
        EXPECT_MEMEQ(a.data, b.data, (size_t)a.size);
        ASSERT_EQ(a.subasset_count, b.subasset_count);
        for (u32 i = 0; i < a.subasset_count; ++i) {
            EXPECT_STREQ(a.subassets[i].name, b.subassets[i].name);
            ASSERT_EQ(a.subassets[i].size, b.subassets[i].size);
            EXPECT_MEMEQ(a.subassets[i].data, b.subassets[i].data, (size_t)a.subassets[i].size);
        }
        if (k == 0) {
            /* The sub-asset order: textures, materials, meshes, prefab (as for glTF). */
            ASSERT_EQ(a.subasset_count, 7u);
            EXPECT_STREQ(a.subassets[0].name, "texture:checker");
            EXPECT_STREQ(a.subassets[1].name, "texture:normal");
            EXPECT_STREQ(a.subassets[2].name, "material:Phong");
            EXPECT_STREQ(a.subassets[3].name, "material:Lambert");
            EXPECT_STREQ(a.subassets[4].name, "material:OpenPBR");
            EXPECT_STREQ(a.subassets[5].name, "Panel");
            EXPECT_STREQ(a.subassets[6].name, "prefab");
            EXPECT_STREQ(a.subassets[6].type, "prefab");
        }
        de_cook_result_free(&a);
        de_cook_result_free(&b);
    }
}

/* ---- import_fbx.fbx.reject_corrupt ----------------------------------------------------------- */

static de_result load_bytes(fbx_fx *f, const void *data, u64 size, const char *name) {
    de_model_ir ir;
    de_model_ir_init(&ir, &f->alloc, "corrupt");
    de_model_source src = {data, size, name, NULL, NULL};
    de_error_clear();
    de_result r = de_model_fbx_load(&src, NULL, &ir);
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        EXPECT_TRUE(e != NULL && e->message[0] != '\0');
    }
    de_model_ir_free(&ir);
    return r;
}

TEST_F(fbx, reject_corrupt, fbx_fx) {
    /* Bounded on purpose (a few hundred loads of 4-9 KB files): well under a second natively,
     * and still quick under the sanitizers. DENG-106 fuzzes this path for real. */
    const char *files[] = {"cube_yup_cm.fbx", "skinned.fbx", "materials.fbx"};
    for (u32 k = 0; k < 3; ++k) {
        void *orig = NULL;
        u64 size = 0;
        char path[64];
        snprintf(path, sizeof path, "fbx:/%s", files[k]);
        ASSERT_EQ(de_file_read_all(path, &f->alloc, &orig, &size), DE_OK);
        u8 *buf = (u8 *)DE_ALLOC(&f->alloc, (size_t)size, 16, DE_MEM_ASSET);
        ASSERT_TRUE(buf != NULL);
        /* Truncated: 48 cuts across the file. A cut inside the objects is an error. */
        for (u32 i = 1; i < 48u; ++i) {
            u64 cut = size * i / 48u;
            memcpy(buf, orig, (size_t)cut);
            de_result r = load_bytes(f, buf, cut, files[k]);
            if (cut < size / 2u)
                EXPECT_NE(r, DE_OK);
        }
        /* Flipped bytes: any result but a crash (CI runs this under ASan and UBSan). */
        for (u32 i = 0; i < 64u; ++i) {
            memcpy(buf, orig, (size_t)size);
            u64 at = (size * i) / 64u + (i % 7u);
            if (at >= size)
                at = size - 1u;
            buf[at] ^= (u8)(0x5Au + i);
            (void)load_bytes(f, buf, size, files[k]);
        }
        DE_FREE(&f->alloc, buf, (size_t)size);
        DE_FREE(&f->alloc, orig, (size_t)size + 1u);
    }
    /* Not FBX at all, empty, an OBJ file (ufbx reads OBJ too, the importer must not). */
    EXPECT_EQ(load_bytes(f, "hello world", 11, "junk.fbx"), DE_ERR_PARSE);
    EXPECT_EQ(load_bytes(f, "", 0, "empty.fbx"), DE_ERR_PARSE);
    static const char obj[] = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    EXPECT_EQ(load_bytes(f, obj, sizeof obj - 1u, "tri.fbx"), DE_ERR_PARSE);
    static const char bad_bin[] = "Kaydara FBX Binary  \0\x1a\0\xe8\x1c\0\0\xff\xff\xff\x7f";
    EXPECT_EQ(load_bytes(f, bad_bin, sizeof bad_bin - 1u, "bad.fbx"), DE_ERR_PARSE);

    /* Through the cooker: a truncated .fbx fails the cook with a message naming the asset. */
    void *cube = NULL;
    u64 cube_size = 0;
    ASSERT_EQ(de_file_read_all("fbx:/cube_yup_cm.fbx", &f->alloc, &cube, &cube_size), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("ftmp:/trunc.fbx", cube, cube_size / 3u), DE_OK);
    DE_FREE(&f->alloc, cube, (size_t)cube_size + 1u);
    make_cooker(f, false);
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = "props/trunc";
    a.type = "model";
    a.source = "trunc.fbx";
    a.file = "ftmp:/assets.toml";
    de_cook_result r;
    EXPECT_EQ(de_cooker_cook(f->c, &(de_cook_input){.asset = &a, .asset_root = "ftmp:/"}, &r),
              DE_ERR_PARSE);
    EXPECT_TRUE(strstr(de_error_last()->message, "props/trunc") != NULL);
    EXPECT_TRUE(strstr(de_error_last()->message, "FBX") != NULL);
}
