/*
 * mesh_fixtures.h: what test_meshlets.c and test_mesh_lod.c share (DENG-103): a generated
 * "bunny-class" mesh (a closed, bumpy, organic surface of about 50k triangles), a cooker with the
 * model importer, and checks over cooked DMSH v2 blobs. Everything is static inline, so a test
 * file that does not use a helper gets no unused-function warning.
 */
#ifndef DE_TEST_IMPORT_MESH_FIXTURES_H
#define DE_TEST_IMPORT_MESH_FIXTURES_H

#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/manifest_internal.h"
#include "cook/importers/model/model_import.h"
#include "cook/importers/model/model_internal.h"
#include "cook/importers/model/model_ir.h"
#include "cook/lib/de_cooker.h"
#include "render/formats/de_cooked_mesh.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Fixture --------------------------------------------------------------------------------- */

typedef struct mesh_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_model_ir ir;
    de_cooker *c;
    void *file; /* a source read with mesh_fx_read */
    u64 file_size;
} mesh_fx;

static inline void mesh_fx_setup(mesh_fx *f, const char *mount) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    /* Each test file mounts data/gltf under its own name (tests may share a process). */
    char probe[64], native[1024];
    snprintf(probe, sizeof probe, "%s:/Duck.glb", mount);
    if (de_fs_resolve(probe, native, sizeof native) != DE_OK)
        ASSERT_EQ(de_fs_mount(mount, DE_IMPORT_TEST_DATA_DIR "/gltf", 0, 0), DE_OK);
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    de_model_ir_init(&f->ir, &f->alloc, "test");
}

static inline void mesh_fx_teardown(mesh_fx *f) {
    de_model_ir_free(&f->ir);
    if (f->c != NULL)
        de_cooker_destroy(f->c);
    if (f->file != NULL)
        DE_FREE(&f->alloc, f->file, (size_t)f->file_size + 1u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    de_log_shutdown();
}

/* A cooker with the model importer (no texture or prefab importer: those pass through). */
static inline void mesh_fx_cooker(mesh_fx *f) {
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
}

/* Cooks `<mount>:/<source>` as model `id` with the settings `kv`. */
static inline de_result mesh_fx_cook(mesh_fx *f, const char *mount, const char *id,
                                     const char *source, const de__manifest_kv *kv, u32 kv_count,
                                     de_cook_result *out) {
    char file[64], root[64];
    snprintf(file, sizeof file, "%s:/assets.toml", mount);
    snprintf(root, sizeof root, "%s:/", mount);
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = id;
    a.type = "model";
    a.source = source;
    a.file = file;
    a.settings = kv;
    a.setting_count = kv_count;
    return de_cooker_cook(f->c, &(de_cook_input){.asset = &a, .asset_root = root}, out);
}

static inline de__manifest_kv mesh_kv_bool(const char *key, bool b) {
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = key;
    kv.value.type = DE__MANIFEST_BOOL;
    kv.value.u.b = b;
    return kv;
}

static inline de__manifest_kv mesh_kv_float(const char *key, f64 v) {
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = key;
    kv.value.type = DE__MANIFEST_FLOAT;
    kv.value.u.f = v;
    return kv;
}

/* Loads `<mount>:/<name>` into f->ir and converts it to engine axes (as the importer does). */
static inline de_result mesh_fx_load_gltf(mesh_fx *f, const char *mount, const char *name) {
    char path[128];
    snprintf(path, sizeof path, "%s:/%s", mount, name);
    DE_TRY(de_file_read_all(path, &f->alloc, &f->file, &f->file_size));
    de_model_source src = {f->file, f->file_size, name, NULL, NULL};
    DE_TRY(de_model_gltf_load(&src, &f->ir));
    de_model_ir_convert_axes(&f->ir, DE_MODEL_AXES_GLTF, 1.0f);
    de_model_ir_compute_bounds(&f->ir);
    return DE_OK;
}

/* ---- The bunny-class mesh -------------------------------------------------------------------- */

/* Radius of the bumpy surface in direction (theta, phi), times `size`. */
static inline f64 bunny_radius(f64 th, f64 ph, f64 size) {
    return size *
           (1.0 + 0.12 * sin(3.0 * th) * cos(2.0 * ph) +
            0.08 * sin(5.0 * th + 1.0) * sin(3.0 * ph) + 0.05 * cos(7.0 * ph) * sin(th) * sin(th) +
            0.03 * sin(11.0 * th) * cos(9.0 * ph));
}

static inline void bunny_point(u32 ring, u32 rings, u32 seg, u32 segs, f64 size, f32 out[3]) {
    f64 th = 3.14159265358979323846 * (f64)ring / (f64)rings;
    f64 ph = 2.0 * 3.14159265358979323846 * (f64)seg / (f64)segs;
    f64 r = bunny_radius(th, ph, size);
    out[0] = (f32)(r * sin(th) * cos(ph));
    out[1] = (f32)(r * sin(th) * sin(ph));
    out[2] = (f32)(r * cos(th));
}

/*
 * Adds a closed latitude-longitude surface of `rings` x `segs` (2 * segs * (rings - 1)
 * triangles, outward winding) to f->ir as one mesh, with area-weighted normals and planar UV0.
 * With `split`, the northern and southern halves are two submeshes with their own vertices: the
 * equator ring is duplicated, a seam between submeshes. Returns the mesh.
 */
static inline de_model_mesh *mesh_fx_bunny(mesh_fx *f, u32 rings, u32 segs, bool split, f64 size) {
    de_model_ir *ir = &f->ir;
    ir->meshes = (de_model_mesh *)de_model_ir_array(ir, 1, sizeof(de_model_mesh));
    ir->mesh_count = 1;
    de_model_mesh *m = &ir->meshes[0];
    m->name = "bunny";
    u32 half = rings / 2u;
    /* Parts (submeshes): whole, or north half (with the north pole) and south half (with the
     * south pole). Rings first_ring .. last_ring of each part are its vertex rows. */
    u32 parts = split ? 2u : 1u;
    u32 first_ring[2] = {1u, half}, last_ring[2] = {split ? half : rings - 1u, rings - 1u};
    u32 vcount = 0, icount = 0;
    for (u32 p = 0; p < parts; ++p) {
        u32 nr = last_ring[p] - first_ring[p] + 1u;
        u32 poles = (p == 0 ? 1u : 0u) + (p == parts - 1u ? 1u : 0u);
        vcount += nr * segs + poles;
        icount += 3u * (poles * segs + 2u * segs * (nr - 1u));
    }
    m->vertex_count = vcount;
    m->index_count = icount;
    m->submesh_count = parts;
    m->attr_mask = DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_POSITION) |
                   DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_NORMAL) | DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_UV0);
    m->position = (f32 *)de_model_ir_array(ir, (u64)vcount * 3u, sizeof(f32));
    m->normal = (f32 *)de_model_ir_array(ir, (u64)vcount * 3u, sizeof(f32));
    m->uv[0] = (f32 *)de_model_ir_array(ir, (u64)vcount * 2u, sizeof(f32));
    m->indices = (u32 *)de_model_ir_array(ir, icount, sizeof(u32));
    m->submeshes = (de_model_submesh *)de_model_ir_array(ir, parts, sizeof(de_model_submesh));
    u32 v = 0, ii = 0;
    for (u32 p = 0; p < parts; ++p) {
        de_model_submesh *sm = &m->submeshes[p];
        sm->index_first = ii;
        sm->vertex_first = v;
        sm->material = -1;
        sm->attr_mask = m->attr_mask;
        bool north = p == 0, south = p == parts - 1u;
        u32 nr = last_ring[p] - first_ring[p] + 1u;
        u32 npole = 0, spole = 0;
        if (north) {
            npole = v;
            bunny_point(0, rings, 0, segs, size, m->position + 3u * (u64)v++);
        }
        u32 ring0 = v;
        for (u32 r = first_ring[p]; r <= last_ring[p]; ++r)
            for (u32 s = 0; s < segs; ++s, ++v)
                bunny_point(r, rings, s, segs, size, m->position + 3u * (u64)v);
        if (south) {
            spole = v;
            bunny_point(rings, rings, 0, segs, size, m->position + 3u * (u64)v++);
        }
        /* Outward winding: counter-clockwise seen from outside. */
        for (u32 s = 0; s < segs && north; ++s) {
            m->indices[ii++] = npole;
            m->indices[ii++] = ring0 + s;
            m->indices[ii++] = ring0 + (s + 1u) % segs;
        }
        for (u32 r = 0; r + 1u < nr; ++r)
            for (u32 s = 0; s < segs; ++s) {
                u32 a = ring0 + r * segs + s, b = ring0 + r * segs + (s + 1u) % segs;
                u32 c = a + segs, d = b + segs;
                m->indices[ii++] = a;
                m->indices[ii++] = c;
                m->indices[ii++] = d;
                m->indices[ii++] = a;
                m->indices[ii++] = d;
                m->indices[ii++] = b;
            }
        u32 last = ring0 + (nr - 1u) * segs;
        for (u32 s = 0; s < segs && south; ++s) {
            m->indices[ii++] = spole;
            m->indices[ii++] = last + (s + 1u) % segs;
            m->indices[ii++] = last + s;
        }
        sm->index_count = ii - sm->index_first;
        sm->vertex_count = v - sm->vertex_first;
    }
    /* Area-weighted normals (per vertex, so the duplicated equator gets its own side's). */
    for (u32 t = 0; t < icount; t += 3) {
        const f32 *a = m->position + 3u * (u64)m->indices[t];
        const f32 *b = m->position + 3u * (u64)m->indices[t + 1];
        const f32 *c = m->position + 3u * (u64)m->indices[t + 2];
        f64 e1[3], e2[3];
        for (u32 k = 0; k < 3; ++k) {
            e1[k] = (f64)b[k] - a[k];
            e2[k] = (f64)c[k] - a[k];
        }
        f64 n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                    e1[0] * e2[1] - e1[1] * e2[0]};
        for (u32 j = 0; j < 3; ++j)
            for (u32 k = 0; k < 3; ++k)
                m->normal[3u * (u64)m->indices[t + j] + k] += (f32)n[k];
    }
    for (u32 i = 0; i < vcount; ++i) {
        f32 *n = m->normal + 3u * (u64)i;
        f64 l = sqrt((f64)n[0] * n[0] + (f64)n[1] * n[1] + (f64)n[2] * n[2]);
        for (u32 k = 0; k < 3; ++k)
            n[k] = l > 0.0 ? (f32)(n[k] / l) : (k == 2 ? 1.0f : 0.0f);
        const f32 *q = m->position + 3u * (u64)i;
        m->uv[0][2u * i] = (f32)(q[0] / (2.6 * size) + 0.5);
        m->uv[0][2u * i + 1u] = (f32)(q[1] / (2.6 * size) + 0.5);
    }
    de_model_ir_compute_bounds(ir);
    return m;
}

/* Cooks mesh `m` of f->ir into `b` (fresh) with `s` and parses it into `v`. */
static inline void mesh_fx_cook_mesh(mesh_fx *f, const de_model_mesh *m,
                                     const de_model_mesh_settings *s, de_model_buf *b,
                                     de_cooked_mesh_view *v) {
    de_model_buf_init(b, &f->ir);
    ASSERT_EQ(de_model_mesh_cook(&f->ir, m, s, NULL, b), DE_OK);
    const char *why = "";
    bool ok = de_cooked_mesh_parse(b->data, b->size, v, &why);
    if (!ok)
        printf("  parse failed: %s\n", why);
    ASSERT_TRUE(ok);
}

static inline de_model_mesh_settings mesh_default_settings(void) {
    de_model_mesh_settings s = {
        .generate_lods = true, .meshlets = true, .lod_error = 0.01f, .max_position_error = 0.0005f};
    return s;
}

/* ---- Checks over a parsed blob --------------------------------------------------------------- */

/* A triangle as three mesh vertex indices, rotated so the smallest comes first (winding kept). */
typedef struct mesh_tri {
    u32 v[3];
} mesh_tri;

static inline mesh_tri mesh_tri_canon(u32 a, u32 b, u32 c) {
    mesh_tri t;
    if (a <= b && a <= c) {
        t.v[0] = a, t.v[1] = b, t.v[2] = c;
    } else if (b <= a && b <= c) {
        t.v[0] = b, t.v[1] = c, t.v[2] = a;
    } else {
        t.v[0] = c, t.v[1] = a, t.v[2] = b;
    }
    return t;
}

static inline int mesh_tri_cmp(const void *pa, const void *pb) {
    const mesh_tri *a = (const mesh_tri *)pa, *b = (const mesh_tri *)pb;
    for (u32 k = 0; k < 3; ++k)
        if (a->v[k] != b->v[k])
            return a->v[k] < b->v[k] ? -1 : 1;
    return 0;
}

/*
 * The meshlet checks over every LOD and submesh of a parsed blob: limits (<= 64 vertices,
 * <= 124 triangles), coverage (the meshlets of a (LOD, submesh) hold exactly its triangles, each
 * once, winding kept) and containment (each sphere contains its decoded vertices). Returns the
 * number of meshlets checked.
 */
static inline u32 mesh_check_meshlets(const de_cooked_mesh_view *v) {
    u32 checked = 0;
    for (u32 l = 0; l < v->lod_count; ++l) {
        for (u32 s = 0; s < v->submesh_count; ++s) {
            de_cooked_mesh_range r = de_cooked_mesh_lod_submesh(v, l, s);
            u32 nt = r.index_count / 3u;
            mesh_tri *want = (mesh_tri *)calloc((size_t)nt + 1u, sizeof(mesh_tri));
            mesh_tri *got = (mesh_tri *)calloc((size_t)nt + 1u, sizeof(mesh_tri));
            ASSERT_TRUE(want != NULL && got != NULL);
            for (u32 t = 0; t < nt; ++t) {
                u32 i = r.index_first + 3u * t;
                want[t] = mesh_tri_canon(de_cooked_mesh_index(v, i), de_cooked_mesh_index(v, i + 1),
                                         de_cooked_mesh_index(v, i + 2));
            }
            u32 ng = 0;
            bool over = false;
            for (u32 k = 0; k < r.meshlet_count; ++k) {
                de_cooked_meshlet ml = de_cooked_mesh_meshlet(v, r.meshlet_first + k);
                EXPECT_TRUE(ml.vertex_count <= DE_COOKED_MESHLET_MAX_VERTICES);
                EXPECT_TRUE(ml.triangle_count <= DE_COOKED_MESHLET_MAX_TRIANGLES);
                EXPECT_EQ(ml.submesh, s);
                EXPECT_EQ(ml.triangle_offset % 4u, 0u);
                for (u32 t = 0; t < ml.triangle_count; ++t) {
                    const u8 *tri = v->meshlet_triangles + ml.triangle_offset + 3u * t;
                    u32 g[3];
                    for (u32 j = 0; j < 3; ++j)
                        g[j] = de_cooked_mesh_meshlet_vertex(v, ml.vertex_offset + tri[j]);
                    if (ng == nt) {
                        over = true;
                        break;
                    }
                    got[ng++] = mesh_tri_canon(g[0], g[1], g[2]);
                }
                /* Containment, in f64 against the stored f32 radius. */
                for (u32 j = 0; j < ml.vertex_count; ++j) {
                    f32 p[3] = {0};
                    de_cooked_mesh_position(
                        v, de_cooked_mesh_meshlet_vertex(v, ml.vertex_offset + j), p);
                    f64 d2 = 0.0;
                    for (u32 c = 0; c < 3; ++c)
                        d2 += ((f64)p[c] - ml.center[c]) * ((f64)p[c] - ml.center[c]);
                    EXPECT_TRUE(sqrt(d2) <= (f64)ml.radius);
                }
                ++checked;
            }
            EXPECT_FALSE(over);
            EXPECT_EQ(ng, nt);
            if (ng == nt && nt > 0) {
                qsort(want, nt, sizeof(mesh_tri), mesh_tri_cmp);
                qsort(got, nt, sizeof(mesh_tri), mesh_tri_cmp);
                EXPECT_MEMEQ(want, got, (size_t)nt * sizeof(mesh_tri));
            }
            free(want);
            free(got);
        }
    }
    return checked;
}

#endif /* DE_TEST_IMPORT_MESH_FIXTURES_H */
