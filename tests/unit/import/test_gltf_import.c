/*
 * test_gltf_import.c: the glTF model importer (DENG-101, SPEC §11.6, §5.5, §12.6): the glTF front
 * end into the model IR, the axis conversion, the sub-asset names, the cooked mesh, material and
 * model formats, the prefab TOML, the texture and prefab sub-assets cooked through the registry,
 * determinism and corrupt input. CTest names import.gltf.<test>.
 *
 * Fixtures are in data/gltf/ (mounted read-only as gltf:/): Khronos samples (Box, BoxTextured,
 * Duck, SimpleSparseAccessor; see KHRONOS_SAMPLES.txt) and files written by gen_fixtures.py.
 * The cooker tests run the importer through a de_cooker, as `de-cook` and the Dev runtime do.
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
#include "asset/scene/scene_format.h"
#include "asset/slots_internal.h"
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

static void gltf_mount(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("gltf", DE_IMPORT_TEST_DATA_DIR "/gltf", 0, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("gtmp", DE_IMPORT_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    mounted = true;
}

/* Reads gltf:/<rel> into a buffer of the reader (freed by reader_free). */
typedef struct reader {
    const de_allocator *alloc;
    void *bufs[16];
    u64 sizes[16];
    u32 count;
} reader;

static de_result reader_read(void *user, const char *rel, const void **data, u64 *size) {
    reader *r = (reader *)user;
    char path[512];
    snprintf(path, sizeof path, "gltf:/%s", rel);
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

typedef struct imp_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    reader rd;
    de_model_ir ir;
    /* The cooker and, for the prefab importer, an asset system over a registry with the ECS
     * built-in components. */
    de_cooker *c;
    de_task_system *tasks;
    de_type_registry *types;
    de_world *w;
    de_asset_system *s;
} imp_fx;

static void imp_fx_setup(imp_fx *f) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    gltf_mount();
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    f->rd.alloc = &f->alloc;
    de_model_ir_init(&f->ir, &f->alloc, "test");
}

static void imp_fx_teardown(imp_fx *f) {
    de_model_ir_free(&f->ir);
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

/* Loads gltf:/<name> into f->ir (glTF coordinates). */
static de_result load_ir(imp_fx *f, const char *name) {
    const void *data = NULL;
    u64 size = 0;
    DE_TRY(reader_read(&f->rd, name, &data, &size));
    de_model_source src = {data, size, name, reader_read, &f->rd};
    return de_model_gltf_load(&src, &f->ir);
}

static bool has_warning(const de_model_ir *ir, const char *needle) {
    for (u32 i = 0; i < ir->warning_count; ++i)
        if (strstr(ir->warnings[i], needle) != NULL)
            return true;
    return false;
}

/* A stand-in texture importer: writes what it was given, so the test sees the settings. */
static de_result fake_texture_import(de_asset_import_ctx *ctx) {
    u64 n = 0;
    (void)de_asset_import_source(ctx, &n);
    char head[128];
    int k = snprintf(head, sizeof head, "FAKETEX srgb=%d usage=%s size=%llu",
                     de_asset_import_setting_bool(ctx, "srgb", false) ? 1 : 0,
                     de_asset_import_setting_string(ctx, "usage", "?"), (unsigned long long)n);
    return de_asset_import_write(ctx, head, (u64)k);
}

/* The cooker with the model importer; with `prefab`, also the runtime's prefab importer; with
 * `texture`, the stand-in texture importer. */
static void make_cooker(imp_fx *f, bool prefab, bool texture) {
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
    if (texture) {
        de_asset_type_desc t = {
            .name = "texture", .importer_version = 1, .import = fake_texture_import};
        ASSERT_EQ(de_cooker_register_type(f->c, &t), DE_OK);
    }
    if (prefab) {
        ASSERT_EQ(de_type_registry_create(&f->alloc, &f->types), DE_OK);
        ASSERT_EQ(
            de_world_create(&(de_world_desc){.allocator = &f->alloc, .types = f->types}, &f->w),
            DE_OK);
        ASSERT_TRUE(DE_HANDLE_VALID(de_builtin_components(f->w).transform));
        de_fp_env_init_thread();
        ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 1}, &f->tasks),
                  DE_OK);
        de_asset_system_desc d = {.allocator = &f->alloc,
                                  .tasks = f->tasks,
                                  .io_threads = 1,
                                  .cache_dir = "nocache:/decache",
                                  .types = f->types};
        ASSERT_EQ(de_asset_system_create(&d, &f->s), DE_OK);
        const de_asset_type_desc *td =
            de__asset_type_get(f->s, de__asset_type_find(f->s, "prefab"));
        ASSERT_TRUE(td != NULL);
        ASSERT_EQ(de_cooker_register_type(f->c, td), DE_OK);
    }
}

static de_result cook(imp_fx *f, const char *id, const char *source, const de__manifest_kv *kv,
                      u32 kv_count, de_cook_result *out) {
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = id;
    a.type = "model";
    a.source = source;
    a.file = "gltf:/assets.toml";
    a.settings = kv;
    a.setting_count = kv_count;
    return de_cooker_cook(f->c, &(de_cook_input){.asset = &a, .asset_root = "gltf:/"}, out);
}

static de_result cook_tmp(imp_fx *f, const char *id, const char *source, de_cook_result *out) {
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = id;
    a.type = "model";
    a.source = source;
    a.file = "gtmp:/assets.toml";
    return de_cooker_cook(f->c, &(de_cook_input){.asset = &a, .asset_root = "gtmp:/"}, out);
}

static const de_cook_subasset *sub(const de_cook_result *r, const char *name) {
    const de_cook_subasset *s = de_cook_result_subasset(r, name);
    if (s == NULL)
        printf("  no sub-asset \"%s\"\n", name);
    return s;
}

static bool has_dep(const de_cook_result *r, const char *id) {
    de_asset_id want = de_asset_id_from(id);
    for (u32 i = 0; i < r->dependency_count; ++i)
        if (r->dependencies[i].hash == want.hash)
            return true;
    return false;
}

static f32 rd_f32(const u8 *p) {
    return de_cooked_get_f32(p);
}

/* ---- import.gltf.import_box ------------------------------------------------------------------ */

TEST_F(gltf, import_box, imp_fx) {
    ASSERT_EQ(load_ir(f, "Box.gltf"), DE_OK);
    de_model_ir *ir = &f->ir;
    ASSERT_EQ(ir->mesh_count, 1u);
    EXPECT_EQ(ir->meshes[0].vertex_count, 24u);
    EXPECT_EQ(ir->meshes[0].index_count, 36u);
    EXPECT_EQ(ir->meshes[0].submesh_count, 1u);
    EXPECT_EQ(ir->meshes[0].attr_mask,
              DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_POSITION) | DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_NORMAL));
    ASSERT_EQ(ir->material_count, 1u);
    EXPECT_STREQ(ir->materials[0].name, "Red");
    EXPECT_FLOAT_EQ(ir->materials[0].params.base_color[0], 0.8f, 1e-6f);
    EXPECT_FLOAT_EQ(ir->materials[0].params.base_color[1], 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(ir->materials[0].params.metallic, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(ir->materials[0].params.roughness, 1.0f, 0.0f);
    EXPECT_EQ(ir->texture_count, 0u);
    ASSERT_EQ(ir->node_count, 2u);
    EXPECT_EQ(ir->nodes[1].parent, 0);
    EXPECT_EQ(ir->nodes[1].mesh, 0);
    EXPECT_EQ(ir->root_count, 1u);
    EXPECT_EQ(ir->warning_count, 0u);
    /* Box's root matrix rotates -90 deg about X (a Z-up to Y-up fix of its exporter). */
    EXPECT_FLOAT_EQ(ir->nodes[0].rotation[0], -sqrtf(0.5f), 1e-6f);
    EXPECT_FLOAT_EQ(ir->nodes[0].rotation[3], sqrtf(0.5f), 1e-6f);

    /* Cooked: the mesh is named after the node that uses it (unnamed: node1). */
    make_cooker(f, false, false);
    de_cook_result r;
    ASSERT_EQ(cook(f, "props/box", "Box.gltf", NULL, 0, &r), DE_OK);
    const de_cook_subasset *m = sub(&r, "node1");
    const de_cook_subasset *mat = sub(&r, "material:Red");
    ASSERT_TRUE(m != NULL && mat != NULL);
    EXPECT_STREQ(m->type, "mesh");
    EXPECT_STREQ(mat->type, "material");
    EXPECT_TRUE(sub(&r, "prefab") != NULL);
    de_cooked_mesh_view mv;
    const char *why = NULL;
    ASSERT_TRUE(de_cooked_mesh_parse(m->data, m->size, &mv, &why));
    EXPECT_EQ(mv.vertex_count, 24u);
    EXPECT_EQ(mv.index_count, 36u);
    EXPECT_EQ(mv.submesh_count, 1u);
    EXPECT_TRUE(mv.streams[DE_COOKED_STREAM_NORMAL] != NULL);
    EXPECT_TRUE(mv.streams[DE_COOKED_STREAM_UV0] == NULL);
    de_cooked_submesh s0 = de_cooked_mesh_submesh(&mv, 0);
    EXPECT_EQ(s0.material_id, de_asset_id_from("props/box#material:Red").hash);
    /* The unit cube: bounds ±0.5 on every axis after the conversion too. */
    for (u32 k = 0; k < 3; ++k) {
        EXPECT_FLOAT_EQ(mv.bounds_min[k], -0.5f, 1e-6f);
        EXPECT_FLOAT_EQ(mv.bounds_max[k], 0.5f, 1e-6f);
    }
    de_cooked_material_view matv;
    ASSERT_TRUE(de_cooked_material_parse(mat->data, mat->size, &matv, &why));
    EXPECT_FLOAT_EQ(matv.params.base_color[0], 0.8f, 1e-6f);
    EXPECT_EQ(matv.texture_count, 0u);
    EXPECT_EQ(matv.alpha_mode, (u32)DE_COOKED_ALPHA_OPAQUE);

    de_cooked_model_view md;
    ASSERT_TRUE(de_cooked_model_parse(r.data, r.size, &md, &why));
    EXPECT_EQ(md.node_count, 2u);
    EXPECT_EQ(md.root_count, 1u);
    EXPECT_EQ(md.mesh_count, 1u);
    EXPECT_EQ(md.material_count, 1u);
    EXPECT_EQ(md.prefab_id, de_asset_id_from("props/box#prefab").hash);
    de_cooked_model_node n1 = de_cooked_model_get_node(&md, 1);
    EXPECT_STREQ(n1.name, "node1");
    EXPECT_EQ(n1.parent, 0u);
    EXPECT_EQ(n1.mesh, 0u);
    EXPECT_EQ(de_cooked_model_mesh_id(&md, 0), de_asset_id_from("props/box#node1").hash);
    EXPECT_TRUE(has_dep(&r, "props/box#node1"));
    EXPECT_TRUE(has_dep(&r, "props/box#material:Red"));
    EXPECT_TRUE(has_dep(&r, "props/box#prefab"));
    /* The external buffer is an extra source of the cook. */
    ASSERT_EQ(r.extra_source_count, 1u);
    EXPECT_STREQ(r.extra_sources[0], "Box0.bin");
    de_cook_result_free(&r);
}

/* ---- import.gltf.import_duck ----------------------------------------------------------------- */

TEST_F(gltf, import_duck, imp_fx) {
    ASSERT_EQ(load_ir(f, "Duck.glb"), DE_OK);
    de_model_ir *ir = &f->ir;
    ASSERT_EQ(ir->mesh_count, 1u);
    EXPECT_STREQ(ir->meshes[0].name, "LOD3spShape");
    EXPECT_EQ(ir->meshes[0].vertex_count, 2399u);
    EXPECT_EQ(ir->meshes[0].index_count, 12636u);
    EXPECT_TRUE((ir->meshes[0].attr_mask & DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_UV0)) != 0);
    ASSERT_EQ(ir->material_count, 1u);
    EXPECT_STREQ(ir->materials[0].name, "blinn3-fx");
    ASSERT_EQ(ir->texture_count, 1u);
    EXPECT_EQ((int)ir->textures[0].usage, (int)DE_MODEL_TEX_COLOR);
    EXPECT_STREQ(ir->textures[0].mime, "image/png");
    ASSERT_GT(ir->textures[0].size, 8u);
    EXPECT_MEMEQ(ir->textures[0].data, "\x89PNG", 4);
    EXPECT_EQ(ir->materials[0].tex[DE_COOKED_TEX_BASE_COLOR].texture, 0);
    EXPECT_EQ(ir->node_count, 3u);
    EXPECT_EQ(ir->root_count, 1u);
}

/* ---- import.gltf.import_two_materials ----------------------------------------------------------
 */

TEST_F(gltf, import_two_materials, imp_fx) {
    ASSERT_EQ(load_ir(f, "two_materials.gltf"), DE_OK);
    const de_model_mesh *m = &f->ir.meshes[0];
    EXPECT_EQ(m->vertex_count, 8u);
    EXPECT_EQ(m->index_count, 12u);
    ASSERT_EQ(m->submesh_count, 2u);
    EXPECT_EQ(m->submeshes[0].material, 0);
    EXPECT_EQ(m->submeshes[1].material, 1);
    EXPECT_EQ(m->submeshes[1].index_first, 6u);
    EXPECT_EQ(m->submeshes[1].vertex_first, 4u);
    /* Indices of the second primitive are offset by its first vertex. */
    EXPECT_EQ(m->indices[6], 4u);
    EXPECT_FLOAT_EQ(f->ir.materials[1].params.metallic, 0.5f, 0.0f);
    EXPECT_FLOAT_EQ(f->ir.materials[1].params.roughness, 0.25f, 0.0f);

    make_cooker(f, false, false);
    de_cook_result r;
    ASSERT_EQ(cook(f, "props/panels", "two_materials.gltf", NULL, 0, &r), DE_OK);
    const de_cook_subasset *ms = sub(&r, "Panel");
    ASSERT_TRUE(ms != NULL);
    de_cooked_mesh_view mv;
    ASSERT_TRUE(de_cooked_mesh_parse(ms->data, ms->size, &mv, NULL));
    ASSERT_EQ(mv.submesh_count, 2u);
    EXPECT_EQ(de_cooked_mesh_submesh(&mv, 0).material_id,
              de_asset_id_from("props/panels#material:Red").hash);
    EXPECT_EQ(de_cooked_mesh_submesh(&mv, 1).material_id,
              de_asset_id_from("props/panels#material:Blue").hash);
    /* Submesh bounds: the second quad is at x in [2, 3]. */
    EXPECT_FLOAT_EQ(de_cooked_mesh_submesh(&mv, 1).bounds_min[0], 2.0f, 0.0f);
    EXPECT_FLOAT_EQ(de_cooked_mesh_submesh(&mv, 1).bounds_max[0], 3.0f, 0.0f);
    de_cook_result_free(&r);
}

/* ---- import.gltf.import_textured_subassets ----------------------------------------------------
 */

TEST_F(gltf, import_textured_subassets, imp_fx) {
    /* No texture importer registered: the sub-asset holds the PNG as it is. */
    make_cooker(f, false, false);
    de_cook_result r;
    ASSERT_EQ(cook(f, "props/boxtex", "BoxTextured.gltf", NULL, 0, &r), DE_OK);
    const de_cook_subasset *t = sub(&r, "texture:0");
    ASSERT_TRUE(t != NULL);
    EXPECT_STREQ(t->type, "texture");
    EXPECT_EQ(t->size, 3750u);
    EXPECT_MEMEQ(t->data, "\x89PNG", 4);
    const de_cook_subasset *mat = sub(&r, "material:Texture");
    ASSERT_TRUE(mat != NULL);
    de_cooked_material_view mv;
    ASSERT_TRUE(de_cooked_material_parse(mat->data, mat->size, &mv, NULL));
    const de_cooked_material_tex *bc = de_cooked_material_texture(&mv, DE_COOKED_TEX_BASE_COLOR);
    ASSERT_TRUE(bc != NULL);
    EXPECT_EQ(bc->asset_id, de_asset_id_from("props/boxtex#texture:0").hash);
    EXPECT_EQ(bc->texcoord, 0u);
    EXPECT_TRUE(has_dep(&r, "props/boxtex#texture:0"));
    /* The image file and the buffer are extra sources, in read order. */
    ASSERT_EQ(r.extra_source_count, 2u);
    EXPECT_STREQ(r.extra_sources[0], "BoxTextured0.bin");
    EXPECT_STREQ(r.extra_sources[1], "CesiumLogoFlat.png");
    de_cook_result_free(&r);
    de_cooker_destroy(f->c);
    f->c = NULL;

    /* With a texture importer: the sub-asset is its output, with the inferred settings. */
    make_cooker(f, false, true);
    ASSERT_EQ(cook(f, "props/boxtex", "BoxTextured.gltf", NULL, 0, &r), DE_OK);
    t = sub(&r, "texture:0");
    ASSERT_TRUE(t != NULL);
    static const char want[] = "FAKETEX srgb=1 usage=color size=3750";
    ASSERT_EQ(t->size, (u64)(sizeof want - 1u));
    EXPECT_MEMEQ(t->data, want, sizeof want - 1u);
    de_cook_result_free(&r);

    /* Usage inference: colour (sRGB), normal map, linear mask; one texture per (image, usage). */
    ASSERT_EQ(cook(f, "props/cc", "khr_clearcoat.gltf", NULL, 0, &r), DE_OK);
    const de_cook_subasset *lin = sub(&r, "texture:lin");
    const de_cook_subasset *nrm = sub(&r, "texture:nrm");
    ASSERT_TRUE(lin != NULL && nrm != NULL);
    EXPECT_TRUE(strstr((const char *)lin->data, "srgb=0 usage=mask") != NULL);
    EXPECT_TRUE(strstr((const char *)nrm->data, "srgb=0 usage=normal") != NULL);
    de_cook_result_free(&r);
    ASSERT_EQ(cook(f, "props/sh", "khr_sheen.gltf", NULL, 0, &r), DE_OK);
    const de_cook_subasset *col = sub(&r, "texture:col");
    ASSERT_TRUE(col != NULL);
    EXPECT_TRUE(strstr((const char *)col->data, "srgb=1 usage=color") != NULL);
    de_cook_result_free(&r);
}

/* ---- import.gltf.import_sparse --------------------------------------------------------------- */

TEST_F(gltf, import_sparse, imp_fx) {
    ASSERT_EQ(load_ir(f, "SimpleSparseAccessor.gltf"), DE_OK);
    const de_model_mesh *m = &f->ir.meshes[0];
    ASSERT_EQ(m->vertex_count, 14u);
    EXPECT_EQ(m->index_count, 36u);
    /* Base grid: vertex i is (i % 7, i / 7, 0); the sparse values replace 8, 10 and 12. */
    EXPECT_FLOAT_EQ(m->position[3 * 7 + 1], 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(m->position[3 * 8 + 0], 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(m->position[3 * 8 + 1], 2.0f, 0.0f);
    EXPECT_FLOAT_EQ(m->position[3 * 10 + 0], 3.0f, 0.0f);
    EXPECT_FLOAT_EQ(m->position[3 * 10 + 1], 3.0f, 0.0f);
    EXPECT_FLOAT_EQ(m->position[3 * 12 + 0], 5.0f, 0.0f);
    EXPECT_FLOAT_EQ(m->position[3 * 12 + 1], 4.0f, 0.0f);
    EXPECT_FLOAT_EQ(m->position[3 * 9 + 1], 1.0f, 0.0f); /* not replaced */
    /* No normals in the file: generated, unit length. */
    EXPECT_TRUE((m->attr_mask & DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_NORMAL)) != 0);
    EXPECT_FLOAT_EQ(fabsf(m->normal[2]), 1.0f, 1e-6f);
}

/* ---- import.gltf.import_quantized ------------------------------------------------------------ */

TEST_F(gltf, import_quantized, imp_fx) {
    ASSERT_EQ(load_ir(f, "quantized.gltf"), DE_OK);
    const de_model_mesh *m = &f->ir.meshes[0];
    ASSERT_EQ(m->vertex_count, 3u);
    /* KHR_mesh_quantization: integer positions as they are, normalized normals and UVs. */
    EXPECT_FLOAT_EQ(m->position[3], 100.0f, 0.0f);
    EXPECT_FLOAT_EQ(m->position[7], 100.0f, 0.0f);
    EXPECT_FLOAT_EQ(m->normal[2], 1.0f, 1e-6f);
    EXPECT_FLOAT_EQ(m->uv[0][2], 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(m->uv[0][5], 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(f->ir.nodes[0].scale[0], 0.01f, 0.0f);
    EXPECT_EQ(f->ir.warning_count, 0u);
}

/* ---- import.gltf.import_strips_to_lists ------------------------------------------------------ */

TEST_F(gltf, import_strips_to_lists, imp_fx) {
    ASSERT_EQ(load_ir(f, "strips_fans.gltf"), DE_OK);
    const de_model_mesh *m = &f->ir.meshes[0];
    /* The strip and the fan (5 vertices, 3 triangles each); points and lines are skipped. */
    ASSERT_EQ(m->submesh_count, 2u);
    EXPECT_EQ(m->vertex_count, 10u);
    ASSERT_EQ(m->index_count, 18u);
    static const u32 want[18] = {0, 1, 2, 1, 3, 2, 2, 3, 4,  /* strip, winding kept */
                                 6, 7, 5, 7, 8, 5, 8, 9, 5}; /* fan around vertex 0 (5) */
    for (u32 i = 0; i < 18; ++i)
        EXPECT_EQ(m->indices[i], want[i]);
    EXPECT_TRUE(has_warning(&f->ir, "points and lines are not imported"));
    u32 skipped = 0;
    for (u32 i = 0; i < f->ir.warning_count; ++i)
        skipped += strstr(f->ir.warnings[i], "points and lines") != NULL;
    EXPECT_EQ(skipped, 2u);
}

/* ---- import.gltf.generate_normals_tangents --------------------------------------------------- */

TEST_F(gltf, generate_normals_tangents, imp_fx) {
    ASSERT_EQ(load_ir(f, "normals_tangents.gltf"), DE_OK);
    const de_model_mesh *m = &f->ir.meshes[0];
    ASSERT_EQ(m->vertex_count, 4u);
    u32 need = DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_NORMAL) | DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_TANGENT);
    EXPECT_EQ(m->attr_mask & need, need);
    for (u32 v = 0; v < 4; ++v) {
        /* The quad lies in z = 0 with counter-clockwise winding: normal +Z (glTF space). */
        EXPECT_FLOAT_EQ(m->normal[3 * v + 2], 1.0f, 1e-6f);
        /* UV u runs along +X: tangent +X, right-handed. */
        EXPECT_FLOAT_EQ(m->tangent[4 * v + 0], 1.0f, 1e-5f);
        EXPECT_FLOAT_EQ(m->tangent[4 * v + 1], 0.0f, 1e-5f);
        EXPECT_FLOAT_EQ(m->tangent[4 * v + 3], 1.0f, 0.0f);
    }
    EXPECT_FLOAT_EQ(f->ir.materials[0].params.normal_scale, 0.5f, 0.0f);
    EXPECT_EQ((int)f->ir.textures[0].usage, (int)DE_MODEL_TEX_NORMAL);

    /* Area weighting: a vertex shared by a large and a small triangle leans to the large one. */
    const f32 pos[] = {0, 0, 0, 10, 0, 0, 0, 10, 0, 0, 0, 1};
    const u32 idx[] = {0, 1, 2, 0, 3, 1}; /* big triangle in z = 0, small one in y = 0 */
    f32 nrm[12];
    de_model_generate_normals(pos, nrm, 0, 4, idx, 6);
    EXPECT_TRUE(nrm[2] > 0.99f);
    EXPECT_TRUE(fabsf(nrm[1]) < 0.1f);
    EXPECT_FLOAT_EQ(nrm[2] * nrm[2] + nrm[1] * nrm[1] + nrm[0] * nrm[0], 1.0f, 1e-6f);
}

/* ---- import.gltf.axis_conversion_points ------------------------------------------------------ */

static f64 det3(const f64 m[3][3]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

static void quat_to_mat(const f32 q[4], f64 m[3][3]) {
    f64 x = q[0], y = q[1], z = q[2], w = q[3];
    m[0][0] = 1 - 2 * (y * y + z * z);
    m[0][1] = 2 * (x * y - z * w);
    m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w);
    m[1][1] = 1 - 2 * (x * x + z * z);
    m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w);
    m[2][1] = 2 * (y * z + x * w);
    m[2][2] = 1 - 2 * (x * x + y * y);
}

TEST_F(gltf, axis_conversion_points, imp_fx) {
    ASSERT_EQ(load_ir(f, "axis.gltf"), DE_OK);
    de_model_ir_convert_axes(&f->ir, DE_MODEL_AXES_GLTF, 1.0f);
    const de_model_mesh *m = &f->ir.meshes[0];
    /* glTF (0,1,0) (up) → engine (0,0,1); glTF (0,0,1) (front) → (0,-1,0); X stays. */
    static const f32 want[9] = {0, 0, 1, 0, -1, 0, 1, 0, 0};
    for (u32 i = 0; i < 9; ++i) {
        EXPECT_FLOAT_EQ(m->position[i], want[i], 0.0f);
        EXPECT_FLOAT_EQ(m->normal[i], want[i], 0.0f);
    }
    /* Tangents: xyz rotated, w (handedness) unchanged. */
    EXPECT_FLOAT_EQ(m->tangent[0], 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(m->tangent[3], 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(m->tangent[4 + 2], 1.0f, 0.0f); /* (0,1,0) → (0,0,1) */
    EXPECT_FLOAT_EQ(m->tangent[4 + 3], -1.0f, 0.0f);
    EXPECT_FLOAT_EQ(m->tangent[8 + 1], -1.0f, 0.0f); /* (0,0,1) → (0,-1,0) */
    /* The conversion is a proper rotation. */
    f32 cf[3][3];
    de_model_axes_matrix(DE_MODEL_AXES_GLTF, cf);
    f64 c[3][3];
    for (u32 i = 0; i < 3; ++i)
        for (u32 j = 0; j < 3; ++j)
            c[i][j] = cf[i][j];
    EXPECT_FLOAT_EQ(det3(c), 1.0, 0.0);
    /* Every node rotation still has det = +1. */
    for (u32 n = 0; n < f->ir.node_count; ++n) {
        f64 r[3][3];
        quat_to_mat(f->ir.nodes[n].rotation, r);
        EXPECT_FLOAT_EQ(det3(r), 1.0, 1e-5);
    }
}

/* ---- import.gltf.axis_conversion_node_trs ---------------------------------------------------- */

/* Column-major 4x4 of a node's TRS. */
static void trs_mat(const de_model_node *n, f64 out[16]) {
    f64 r[3][3];
    quat_to_mat(n->rotation, r);
    memset(out, 0, sizeof(f64) * 16);
    for (u32 col = 0; col < 3; ++col)
        for (u32 row = 0; row < 3; ++row)
            out[4 * col + row] = r[row][col] * n->scale[col];
    for (u32 k = 0; k < 3; ++k)
        out[12 + k] = n->translation[k];
    out[15] = 1.0;
}

static void mat_mul(const f64 a[16], const f64 b[16], f64 out[16]) {
    f64 t[16];
    for (u32 col = 0; col < 4; ++col)
        for (u32 row = 0; row < 4; ++row) {
            f64 s = 0;
            for (u32 k = 0; k < 4; ++k)
                s += a[4 * k + row] * b[4 * col + k];
            t[4 * col + row] = s;
        }
    memcpy(out, t, sizeof t);
}

static void world_of(const de_model_ir *ir, u32 node, f64 out[16]) {
    trs_mat(&ir->nodes[node], out);
    for (i32 p = ir->nodes[node].parent; p >= 0; p = ir->nodes[p].parent) {
        f64 pm[16];
        trs_mat(&ir->nodes[p], pm);
        mat_mul(pm, out, out);
    }
}

TEST_F(gltf, axis_conversion_node_trs, imp_fx) {
    ASSERT_EQ(load_ir(f, "axis.gltf"), DE_OK);
    de_model_ir *ir = &f->ir;
    ASSERT_EQ(ir->node_count, 3u);
    /* The matrix node is split into TRS: rotation 90 deg about Z, scale 2, translation (1,0,0). */
    EXPECT_FLOAT_EQ(ir->nodes[2].scale[0], 2.0f, 1e-6f);
    EXPECT_FLOAT_EQ(ir->nodes[2].scale[2], 2.0f, 1e-6f);
    EXPECT_FLOAT_EQ(ir->nodes[2].rotation[2], sqrtf(0.5f), 1e-6f);
    EXPECT_FLOAT_EQ(ir->nodes[2].translation[0], 1.0f, 0.0f);
    f64 src_world[3][16];
    for (u32 n = 0; n < 3; ++n)
        world_of(ir, n, src_world[n]);

    de_model_ir_convert_axes(ir, DE_MODEL_AXES_GLTF, 1.0f);
    /* C as a 4x4 (column-major) and its transpose. */
    f64 c[16] = {0}, ct[16] = {0};
    c[0] = 1;  /* x' = x */
    c[9] = -1; /* y' = -z: row 1, column 2 */
    c[6] = 1;  /* z' = y: row 2, column 1 */
    c[15] = 1;
    for (u32 col = 0; col < 4; ++col)
        for (u32 row = 0; row < 4; ++row)
            ct[4 * col + row] = c[4 * row + col];
    for (u32 n = 0; n < 3; ++n) {
        f64 want[16], got[16];
        mat_mul(c, src_world[n], want);
        mat_mul(want, ct, want);
        world_of(ir, n, got);
        for (u32 k = 0; k < 16; ++k)
            EXPECT_FLOAT_EQ(got[k], want[k], 1e-5);
    }
    /* The parent's translation (1,2,3) → (1,-3,2); its scale (1,2,3) → (1,3,2). */
    EXPECT_FLOAT_EQ(ir->nodes[0].translation[1], -3.0f, 0.0f);
    EXPECT_FLOAT_EQ(ir->nodes[0].translation[2], 2.0f, 0.0f);
    EXPECT_FLOAT_EQ(ir->nodes[0].scale[1], 3.0f, 0.0f);
    EXPECT_FLOAT_EQ(ir->nodes[0].scale[2], 2.0f, 0.0f);
}

/* ---- import.gltf.skin_anim_converted --------------------------------------------------------- */

TEST_F(gltf, skin_anim_converted, imp_fx) {
    ASSERT_EQ(load_ir(f, "skinned.gltf"), DE_OK);
    de_model_ir *ir = &f->ir;
    ASSERT_EQ(ir->skin_count, 1u);
    ASSERT_EQ(ir->skins[0].joint_count, 2u);
    EXPECT_EQ(ir->skins[0].joints[1], 2u);
    EXPECT_EQ(ir->skins[0].skeleton_root, 1);
    const de_model_mesh *m = &ir->meshes[0];
    EXPECT_EQ(m->joints0[1], 1u);
    EXPECT_FLOAT_EQ(m->weights0[0], 0.75f, 0.0f);
    ASSERT_EQ(ir->anim_count, 1u);
    ASSERT_EQ(ir->anims[0].channel_count, 2u);
    EXPECT_FLOAT_EQ(ir->anims[0].duration, 1.0f, 0.0f);
    EXPECT_EQ((int)ir->anims[0].channels[1].interp, (int)DE_MODEL_INTERP_STEP);

    de_model_ir_convert_axes(ir, DE_MODEL_AXES_GLTF, 2.0f);
    /* The second inverse bind matrix translates by glTF (0,-1,0): engine (0,0,-1), scaled by 2. */
    const f32 *ibm = ir->skins[0].inverse_bind + 16;
    EXPECT_FLOAT_EQ(ibm[12], 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(ibm[13], 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(ibm[14], -2.0f, 0.0f);
    EXPECT_FLOAT_EQ(ibm[0], 1.0f, 0.0f);
    /* Translation keys (0,1,0) and (0,0,1) → (0,0,2) and (0,-2,0). */
    const f32 *t = ir->anims[0].channels[0].values;
    EXPECT_FLOAT_EQ(t[2], 2.0f, 0.0f);
    EXPECT_FLOAT_EQ(t[4], -2.0f, 0.0f);
    /* Rotation keys: 90 deg about glTF +Y becomes 90 deg about engine +Z. */
    const f32 *q = ir->anims[0].channels[1].values + 4;
    EXPECT_FLOAT_EQ(q[1], 0.0f, 1e-7f);
    EXPECT_FLOAT_EQ(q[2], 0.7071068f, 1e-6f);
    EXPECT_FLOAT_EQ(q[3], 0.7071068f, 1e-6f);
}

/* ---- import.gltf.scale_setting --------------------------------------------------------------- */

TEST_F(gltf, scale_setting, imp_fx) {
    make_cooker(f, false, false);
    de__manifest_kv kv[1];
    memset(kv, 0, sizeof kv);
    kv[0].key = "scale";
    kv[0].value.type = DE__MANIFEST_FLOAT;
    kv[0].value.u.f = 0.01;
    de_cook_result a, b;
    ASSERT_EQ(cook(f, "props/axis", "axis.gltf", NULL, 0, &a), DE_OK);
    ASSERT_EQ(cook(f, "props/axis", "axis.gltf", kv, 1, &b), DE_OK);
    EXPECT_NE(a.settings_hash, b.settings_hash);
    de_cooked_model_view ma, mb;
    ASSERT_TRUE(de_cooked_model_parse(a.data, a.size, &ma, NULL));
    ASSERT_TRUE(de_cooked_model_parse(b.data, b.size, &mb, NULL));
    /* Node translations scale; rotations and scales do not. */
    de_cooked_model_node na = de_cooked_model_get_node(&ma, 0);
    de_cooked_model_node nb = de_cooked_model_get_node(&mb, 0);
    EXPECT_FLOAT_EQ(na.translation[1], -3.0f, 0.0f);
    EXPECT_FLOAT_EQ(nb.translation[1], -0.03f, 1e-7f);
    EXPECT_FLOAT_EQ(nb.scale[1], na.scale[1], 0.0f);
    EXPECT_FLOAT_EQ(nb.rotation[3], na.rotation[3], 0.0f);
    /* Vertex positions scale too. */
    de_cooked_mesh_view va, vb;
    const de_cook_subasset *sa = sub(&a, "Child"), *sb = sub(&b, "Child");
    ASSERT_TRUE(sa != NULL && sb != NULL);
    ASSERT_TRUE(de_cooked_mesh_parse(sa->data, sa->size, &va, NULL));
    ASSERT_TRUE(de_cooked_mesh_parse(sb->data, sb->size, &vb, NULL));
    EXPECT_FLOAT_EQ(rd_f32(va.streams[DE_COOKED_STREAM_POSITION] + 8), 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(rd_f32(vb.streams[DE_COOKED_STREAM_POSITION] + 8), 0.01f, 1e-8f);
    EXPECT_FLOAT_EQ(vb.bounds_max[0], 0.01f, 1e-8f);
    de_cook_result_free(&a);
    de_cook_result_free(&b);

    /* A bad scale fails the cook with a message. */
    kv[0].value.u.f = -1.0;
    EXPECT_EQ(cook(f, "props/axis", "axis.gltf", kv, 1, &a), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(de_error_last()->message, "scale") != NULL);
}

/* ---- import.gltf.material_khr_<ext> ---------------------------------------------------------- */

/* Cooks khr_<ext>.gltf and decodes its material. */
static void cook_material(imp_fx *f, const char *file, de_cooked_material_view *out,
                          de_cook_result *r) {
    if (f->c != NULL) {
        de_cooker_destroy(f->c);
        f->c = NULL;
    }
    make_cooker(f, false, false);
    char src[64];
    snprintf(src, sizeof src, "khr_%s.gltf", file);
    ASSERT_EQ(cook(f, "props/khr", src, NULL, 0, r), DE_OK);
    const de_cook_subasset *m = NULL;
    for (u32 i = 0; i < r->subasset_count; ++i)
        if (strcmp(r->subassets[i].type, "material") == 0)
            m = &r->subassets[i];
    ASSERT_TRUE(m != NULL);
    const char *why = NULL;
    ASSERT_TRUE(de_cooked_material_parse(m->data, m->size, out, &why));
}

TEST_F(gltf, material_khr_emissive_strength, imp_fx) {
    de_cooked_material_view m;
    de_cook_result r;
    cook_material(f, "emissive_strength", &m, &r);
    EXPECT_TRUE((m.flags & DE_COOKED_MAT_EMISSIVE_STRENGTH) != 0);
    EXPECT_FLOAT_EQ(m.params.emissive_strength, 5.0f, 0.0f);
    EXPECT_FLOAT_EQ(m.params.emissive[1], 0.5f, 0.0f);
    de_cook_result_free(&r);
}

TEST_F(gltf, material_khr_ior, imp_fx) {
    de_cooked_material_view m;
    de_cook_result r;
    cook_material(f, "ior", &m, &r);
    EXPECT_TRUE((m.flags & DE_COOKED_MAT_IOR) != 0);
    EXPECT_FLOAT_EQ(m.params.ior, 1.33f, 1e-6f);
    de_cook_result_free(&r);
}

TEST_F(gltf, material_khr_specular, imp_fx) {
    de_cooked_material_view m;
    de_cook_result r;
    cook_material(f, "specular", &m, &r);
    EXPECT_TRUE((m.flags & DE_COOKED_MAT_SPECULAR) != 0);
    EXPECT_FLOAT_EQ(m.params.specular, 0.5f, 0.0f);
    EXPECT_FLOAT_EQ(m.params.specular_color[2], 0.25f, 0.0f);
    EXPECT_TRUE(de_cooked_material_texture(&m, DE_COOKED_TEX_SPECULAR) != NULL);
    EXPECT_TRUE(de_cooked_material_texture(&m, DE_COOKED_TEX_SPECULAR_COLOR) != NULL);
    de_cook_result_free(&r);
}

TEST_F(gltf, material_khr_clearcoat, imp_fx) {
    de_cooked_material_view m;
    de_cook_result r;
    cook_material(f, "clearcoat", &m, &r);
    EXPECT_TRUE((m.flags & DE_COOKED_MAT_CLEARCOAT) != 0);
    EXPECT_FLOAT_EQ(m.params.clearcoat, 0.75f, 0.0f);
    EXPECT_FLOAT_EQ(m.params.clearcoat_roughness, 0.125f, 0.0f);
    EXPECT_TRUE(de_cooked_material_texture(&m, DE_COOKED_TEX_CLEARCOAT) != NULL);
    EXPECT_TRUE(de_cooked_material_texture(&m, DE_COOKED_TEX_CLEARCOAT_ROUGHNESS) != NULL);
    const de_cooked_material_tex *n =
        de_cooked_material_texture(&m, DE_COOKED_TEX_CLEARCOAT_NORMAL);
    ASSERT_TRUE(n != NULL);
    EXPECT_EQ(n->asset_id, de_asset_id_from("props/khr#texture:nrm").hash);
    /* The clearcoat normal map makes the importer generate tangents. */
    const de_cook_subasset *mesh = sub(&r, "N");
    ASSERT_TRUE(mesh != NULL);
    de_cooked_mesh_view mv;
    ASSERT_TRUE(de_cooked_mesh_parse(mesh->data, mesh->size, &mv, NULL));
    EXPECT_TRUE(mv.streams[DE_COOKED_STREAM_TANGENT] != NULL);
    de_cook_result_free(&r);
}

TEST_F(gltf, material_khr_sheen, imp_fx) {
    de_cooked_material_view m;
    de_cook_result r;
    cook_material(f, "sheen", &m, &r);
    EXPECT_TRUE((m.flags & DE_COOKED_MAT_SHEEN) != 0);
    EXPECT_FLOAT_EQ(m.params.sheen_color[1], 0.25f, 0.0f);
    EXPECT_FLOAT_EQ(m.params.sheen_roughness, 0.5f, 0.0f);
    EXPECT_TRUE(de_cooked_material_texture(&m, DE_COOKED_TEX_SHEEN_COLOR) != NULL);
    EXPECT_TRUE(de_cooked_material_texture(&m, DE_COOKED_TEX_SHEEN_ROUGHNESS) != NULL);
    de_cook_result_free(&r);
}

TEST_F(gltf, material_khr_transmission, imp_fx) {
    de_cooked_material_view m;
    de_cook_result r;
    cook_material(f, "transmission", &m, &r);
    EXPECT_TRUE((m.flags & DE_COOKED_MAT_TRANSMISSION) != 0);
    EXPECT_FLOAT_EQ(m.params.transmission, 0.9f, 1e-6f);
    EXPECT_TRUE(de_cooked_material_texture(&m, DE_COOKED_TEX_TRANSMISSION) != NULL);
    de_cook_result_free(&r);
}

TEST_F(gltf, material_khr_volume, imp_fx) {
    de_cooked_material_view m;
    de_cook_result r;
    cook_material(f, "volume", &m, &r);
    EXPECT_TRUE((m.flags & DE_COOKED_MAT_VOLUME) != 0);
    EXPECT_FLOAT_EQ(m.params.thickness, 2.0f, 0.0f);
    EXPECT_FLOAT_EQ(m.params.attenuation_distance, 0.5f, 0.0f);
    EXPECT_FLOAT_EQ(m.params.attenuation_color[1], 0.75f, 0.0f);
    EXPECT_TRUE(de_cooked_material_texture(&m, DE_COOKED_TEX_THICKNESS) != NULL);
    de_cook_result_free(&r);
    /* Without the extension the distance is +infinity (the spec's default). */
    de_cooked_material_view d;
    cook_material(f, "ior", &d, &r);
    EXPECT_TRUE(isinf(d.params.attenuation_distance));
    de_cook_result_free(&r);
}

TEST_F(gltf, material_khr_diffuse_transmission, imp_fx) {
    de_cooked_material_view m;
    de_cook_result r;
    cook_material(f, "diffuse_transmission", &m, &r);
    EXPECT_TRUE((m.flags & DE_COOKED_MAT_DIFFUSE_TRANSMISSION) != 0);
    EXPECT_FLOAT_EQ(m.params.diffuse_transmission, 0.25f, 0.0f);
    EXPECT_FLOAT_EQ(m.params.diffuse_transmission_color[1], 0.5f, 0.0f);
    EXPECT_TRUE(de_cooked_material_texture(&m, DE_COOKED_TEX_DIFFUSE_TRANSMISSION) != NULL);
    EXPECT_TRUE(de_cooked_material_texture(&m, DE_COOKED_TEX_DIFFUSE_TRANSMISSION_COLOR) != NULL);
    de_cook_result_free(&r);
}

TEST_F(gltf, material_khr_anisotropy, imp_fx) {
    de_cooked_material_view m;
    de_cook_result r;
    cook_material(f, "anisotropy", &m, &r);
    EXPECT_TRUE((m.flags & DE_COOKED_MAT_ANISOTROPY) != 0);
    EXPECT_FLOAT_EQ(m.params.anisotropy, 0.6f, 1e-6f);
    EXPECT_FLOAT_EQ(m.params.anisotropy_rotation, 1.5f, 0.0f);
    EXPECT_TRUE(de_cooked_material_texture(&m, DE_COOKED_TEX_ANISOTROPY) != NULL);
    de_cook_result_free(&r);
}

TEST_F(gltf, material_khr_iridescence, imp_fx) {
    de_cooked_material_view m;
    de_cook_result r;
    cook_material(f, "iridescence", &m, &r);
    EXPECT_TRUE((m.flags & DE_COOKED_MAT_IRIDESCENCE) != 0);
    EXPECT_FLOAT_EQ(m.params.iridescence, 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(m.params.iridescence_ior, 1.8f, 1e-6f);
    EXPECT_FLOAT_EQ(m.params.iridescence_thickness_min, 200.0f, 0.0f);
    EXPECT_FLOAT_EQ(m.params.iridescence_thickness_max, 800.0f, 0.0f);
    EXPECT_TRUE(de_cooked_material_texture(&m, DE_COOKED_TEX_IRIDESCENCE) != NULL);
    EXPECT_TRUE(de_cooked_material_texture(&m, DE_COOKED_TEX_IRIDESCENCE_THICKNESS) != NULL);
    de_cook_result_free(&r);
}

TEST_F(gltf, material_khr_unlit, imp_fx) {
    de_cooked_material_view m;
    de_cook_result r;
    cook_material(f, "unlit", &m, &r);
    EXPECT_TRUE((m.flags & DE_COOKED_MAT_UNLIT) != 0);
    EXPECT_FLOAT_EQ(m.params.base_color[1], 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(m.params.base_color[0], 0.0f, 0.0f);
    de_cook_result_free(&r);
}

/* ---- import.gltf.texture_transform_baked ----------------------------------------------------- */

TEST_F(gltf, texture_transform_baked, imp_fx) {
    ASSERT_EQ(load_ir(f, "texture_transform.gltf"), DE_OK);
    /* "Tiled": both textures share one transform (scale 2, offset (0.5, 0)): baked. */
    const f32 *uv = f->ir.meshes[0].uv[0];
    EXPECT_FLOAT_EQ(uv[0], 0.5f, 1e-6f); /* (0,0) */
    EXPECT_FLOAT_EQ(uv[2], 2.5f, 1e-6f); /* (1,0) */
    EXPECT_FLOAT_EQ(uv[5], 2.0f, 1e-6f); /* (1,1) */
    /* "Mixed": two transforms on one UV set: not applied, with a warning. */
    const f32 *uv2 = f->ir.meshes[1].uv[0];
    EXPECT_FLOAT_EQ(uv2[2], 1.0f, 0.0f);
    EXPECT_TRUE(has_warning(&f->ir, "different KHR_texture_transform"));
}

/* ---- import.gltf.subasset_ids_dedupe --------------------------------------------------------- */

TEST_F(gltf, subasset_ids_dedupe, imp_fx) {
    ASSERT_EQ(load_ir(f, "dup_names.gltf"), DE_OK);
    de_model_names n;
    ASSERT_EQ(de_model_names_build(&f->ir, &n), DE_OK);
    EXPECT_STREQ(n.prefab, "prefab");
    static const char *const nodes[8] = {"A",        "A~2",   "node2", "A~2~2",
                                         "prefab~2", "a_b__", "node6", "material:M"};
    for (u32 i = 0; i < 8; ++i)
        EXPECT_STREQ(n.nodes[i], nodes[i]);
    EXPECT_STREQ(n.materials[0], "material:M~2");
    EXPECT_STREQ(n.materials[1], "material:M~3");
    EXPECT_STREQ(n.materials[2], "material:2");
    EXPECT_STREQ(n.textures[0], "texture:img");
    EXPECT_STREQ(n.textures[1], "texture:img~2");
    /* The shared mesh takes the first node's name; the unused one its own. */
    EXPECT_STREQ(n.meshes[0], "A");
    EXPECT_STREQ(n.meshes[1], "mesh:Unused");

    /* Through the cooker: two cooks give the same names, every ID is distinct. */
    make_cooker(f, false, false);
    de_cook_result a, b;
    ASSERT_EQ(cook(f, "props/dup", "dup_names.gltf", NULL, 0, &a), DE_OK);
    ASSERT_EQ(cook(f, "props/dup", "dup_names.gltf", NULL, 0, &b), DE_OK);
    ASSERT_EQ(a.subasset_count, b.subasset_count);
    for (u32 i = 0; i < a.subasset_count; ++i) {
        EXPECT_STREQ(a.subassets[i].name, b.subassets[i].name);
        for (u32 j = i + 1; j < a.subasset_count; ++j)
            EXPECT_NE(a.subassets[i].id.hash, a.subassets[j].id.hash);
    }
    EXPECT_TRUE(sub(&a, "mesh:Unused") != NULL);
    /* Both nodes that use the shared mesh point at mesh 0 in the model. */
    de_cooked_model_view md;
    ASSERT_TRUE(de_cooked_model_parse(a.data, a.size, &md, NULL));
    EXPECT_EQ(de_cooked_model_get_node(&md, 0).mesh, 0u);
    EXPECT_EQ(de_cooked_model_get_node(&md, 1).mesh, 0u);
    EXPECT_STREQ(de_cooked_model_get_node(&md, 1).name, "A~2");
    de_cook_result_free(&a);
    de_cook_result_free(&b);
}

/* ---- import.gltf.prefab_golden_text ---------------------------------------------------------- */

TEST_F(gltf, prefab_golden_text, imp_fx) {
    ASSERT_EQ(load_ir(f, "hierarchy.gltf"), DE_OK);
    de_model_ir_convert_axes(&f->ir, DE_MODEL_AXES_GLTF, 1.0f);
    de_model_names n;
    ASSERT_EQ(de_model_names_build(&f->ir, &n), DE_OK);
    de_model_buf b;
    de_model_buf_init(&b, &f->ir);
    ASSERT_TRUE(de_model_prefab_toml(&b, &f->ir, &n, "props/robot", "hierarchy.gltf"));
    void *golden = NULL;
    u64 golden_size = 0;
    ASSERT_EQ(de_file_read_all("gltf:/hierarchy.prefab.toml", &f->alloc, &golden, &golden_size),
              DE_OK);
    bool same = golden_size == b.size && memcmp(golden, b.data, (size_t)b.size) == 0;
    if (!same) {
        /* Written next to the build for a diff (never into the source tree). */
        (void)de_file_write_all_atomic("gtmp:/hierarchy.prefab.toml.actual", b.data, b.size);
        printf(
            "  prefab TOML differs from the golden file; actual: %s/hierarchy.prefab.toml.actual\n",
            DE_IMPORT_TEST_TMP_DIR);
    }
    EXPECT_TRUE(same);
    DE_FREE(&f->alloc, golden, (size_t)golden_size + 1u);

    /* The prefab sub-asset cooks through the registered prefab importer (DSCN): the model root,
     * the 4 scene nodes (NotInScene is left out), parents as in the file. */
    make_cooker(f, true, false);
    de_cook_result r;
    ASSERT_EQ(cook(f, "props/robot", "hierarchy.gltf", NULL, 0, &r), DE_OK);
    const de_cook_subasset *p = sub(&r, "prefab");
    ASSERT_TRUE(p != NULL);
    EXPECT_STREQ(p->type, "prefab");
    de__scene_view v;
    char err[256];
    ASSERT_EQ(de__scene_view_parse(p->data, p->size, &v, err, sizeof err), DE_OK);
    EXPECT_EQ((int)v.kind, (int)DE__SCENE_KIND_PREFAB);
    EXPECT_EQ(v.entity_count, 5u);
    EXPECT_EQ(v.root_count, 1u);
    u32 hand = de__scene_find_entity(&v, de_hash64_str("Hand"));
    u32 arm = de__scene_find_entity(&v, de_hash64_str("Arm"));
    ASSERT_NE(hand, DE__SCENE_NO_PARENT);
    EXPECT_EQ(de__scene_entity_at(&v, hand).parent, arm);
    /* DeTransform is known to the registry; DeMeshRenderer is not registered before S12. */
    ASSERT_EQ(v.type_count, 1u);
    EXPECT_STREQ(de__scene_type_at(&v, 0).name, "DeTransform");
    de_cook_result_free(&r);
}

/* ---- import.gltf.cook_deterministic ---------------------------------------------------------- */

TEST_F(gltf, cook_deterministic, imp_fx) {
    /* verify_determinism is on: every cook runs the importer twice and compares. */
    make_cooker(f, true, true);
    de_cook_result a, b;
    ASSERT_EQ(cook(f, "props/duck", "Duck.glb", NULL, 0, &a), DE_OK);
    ASSERT_EQ(cook(f, "props/duck", "Duck.glb", NULL, 0, &b), DE_OK);
    EXPECT_EQ(a.output_hash, b.output_hash);
    ASSERT_EQ(a.size, b.size);
    EXPECT_MEMEQ(a.data, b.data, (size_t)a.size);
    ASSERT_EQ(a.subasset_count, b.subasset_count);
    for (u32 i = 0; i < a.subasset_count; ++i) {
        EXPECT_STREQ(a.subassets[i].name, b.subassets[i].name);
        ASSERT_EQ(a.subassets[i].size, b.subassets[i].size);
        EXPECT_MEMEQ(a.subassets[i].data, b.subassets[i].data, (size_t)a.subassets[i].size);
    }
    /* The order of the sub-assets: textures, materials, meshes, prefab. */
    ASSERT_EQ(a.subasset_count, 4u);
    EXPECT_STREQ(a.subassets[0].name, "texture:0");
    EXPECT_STREQ(a.subassets[1].name, "material:blinn3-fx");
    EXPECT_STREQ(a.subassets[2].name, "node2");
    EXPECT_STREQ(a.subassets[3].name, "prefab");
    de_cook_result_free(&a);
    de_cook_result_free(&b);

    /* A second cooker gives the same bytes. */
    de_cooker_destroy(f->c);
    f->c = NULL;
    de_asset_system_destroy(f->s);
    f->s = NULL;
    de_world_destroy(f->w);
    f->w = NULL;
    de_task_system_destroy(f->tasks);
    f->tasks = NULL;
    de_type_registry_destroy(f->types);
    f->types = NULL;
    make_cooker(f, true, true);
    ASSERT_EQ(cook(f, "props/duck", "Duck.glb", NULL, 0, &b), DE_OK);
    ASSERT_EQ(cook(f, "props/duck", "Duck.glb", NULL, 0, &a), DE_OK);
    EXPECT_EQ(a.output_hash, b.output_hash);
    de_cook_result_free(&a);

    /* Settings are in the settings hash, read or not. */
    de__manifest_kv kv[1];
    memset(kv, 0, sizeof kv);
    kv[0].key = "generate_lods";
    kv[0].value.type = DE__MANIFEST_BOOL;
    kv[0].value.u.b = true;
    ASSERT_EQ(cook(f, "props/duck", "Duck.glb", kv, 1, &a), DE_OK);
    EXPECT_NE(a.settings_hash, b.settings_hash);
    de_cook_result_free(&a);
    de_cook_result_free(&b);
}

/* ---- import.gltf.reject_truncated ------------------------------------------------------------ */

TEST_F(gltf, reject_truncated, imp_fx) {
    void *duck = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all("gltf:/Duck.glb", &f->alloc, &duck, &size), DE_OK);
    u8 *buf = (u8 *)DE_ALLOC(&f->alloc, (size_t)size, 16, DE_MEM_ASSET);
    ASSERT_TRUE(buf != NULL);
    /* Every cut in the header and JSON, then every 97th byte: an error with a message, never a
     * crash (run under ASan in CI). */
    u32 tried = 0;
    for (u64 cut = 0; cut < size; cut += cut < 4096u ? 1u : 97u) {
        memcpy(buf, duck, (size_t)cut);
        de_model_ir ir;
        de_model_ir_init(&ir, &f->alloc, "trunc");
        de_model_source src = {buf, cut, "Duck.glb", NULL, NULL};
        de_error_clear();
        de_result r = de_model_gltf_load(&src, &ir);
        EXPECT_NE(r, DE_OK);
        if (r != DE_OK) {
            const de_error *e = de_error_last();
            EXPECT_TRUE(e != NULL && e->message[0] != '\0');
        }
        de_model_ir_free(&ir);
        ++tried;
    }
    EXPECT_GT(tried, 4000u);
    /* Flipped bytes anywhere: any result but a crash. */
    for (u64 at = 0; at < size; at += 13u) {
        memcpy(buf, duck, (size_t)size);
        buf[at] ^= 0x5Au;
        de_model_ir ir;
        de_model_ir_init(&ir, &f->alloc, "flip");
        de_model_source src = {buf, size, "Duck.glb", NULL, NULL};
        (void)de_model_gltf_load(&src, &ir);
        de_model_ir_free(&ir);
    }
    /* Through the cooker: a truncated .glb fails the cook with a message naming the asset. */
    ASSERT_EQ(de_file_write_all_atomic("gtmp:/trunc.glb", duck, size / 2u), DE_OK);
    make_cooker(f, false, false);
    de_cook_result r;
    de_result rc = cook_tmp(f, "props/trunc", "trunc.glb", &r);
    EXPECT_EQ(rc, DE_ERR_PARSE);
    EXPECT_TRUE(strstr(de_error_last()->message, "props/trunc") != NULL);
    EXPECT_TRUE(strstr(de_error_last()->message, "truncated") != NULL);
    DE_FREE(&f->alloc, buf, (size_t)size);
    DE_FREE(&f->alloc, duck, (size_t)size + 1u);

    /* Draco and meshopt compression: rejected with a clear error. */
    de_model_ir_free(&f->ir);
    EXPECT_EQ(load_ir(f, "draco_required.gltf"), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(strstr(de_error_last()->message, "KHR_draco_mesh_compression") != NULL);
    de_model_ir_free(&f->ir);
    EXPECT_EQ(load_ir(f, "meshopt_required.gltf"), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(strstr(de_error_last()->message, "EXT_meshopt_compression") != NULL);
    /* Not glTF at all. */
    de_model_ir_free(&f->ir);
    de_model_source junk = {"hello", 5, "junk.gltf", NULL, NULL};
    EXPECT_EQ(de_model_gltf_load(&junk, &f->ir), DE_ERR_PARSE);
}

/* ---- import.gltf.runtime_nested_types ------------------------------------------------------- *
 *
 * In a Dev build the runtime cooks in-process (the cook stage, DENG-209). Its cooker must know
 * every runtime type, not only the type of the asset being cooked, so the model's texture
 * sub-asset is cooked by the registered texture importer the first time already. The model,
 * mesh and material loaders are stand-ins (DENG-105 writes the real ones). */

typedef struct rt_state {
    const de_allocator *alloc;
    char texture_head[16]; /* the first bytes the texture loader saw */
} rt_state;

static de_result rt_load(de_asset_load_ctx *ctx) {
    rt_state *st = (rt_state *)de_asset_load_ctx_user(ctx);
    u64 n = 0;
    (void)de_asset_load_ctx_data(ctx, &n);
    u64 *o = (u64 *)DE_ALLOC(st->alloc, sizeof(u64), DE_ALIGNOF(u64), DE_MEM_ASSET);
    if (o == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    *o = n;
    de_asset_load_ctx_set_object(ctx, o, sizeof *o);
    return DE_OK;
}

static de_result rt_texture_load(de_asset_load_ctx *ctx) {
    rt_state *st = (rt_state *)de_asset_load_ctx_user(ctx);
    u64 n = 0;
    const u8 *p = (const u8 *)de_asset_load_ctx_data(ctx, &n);
    memcpy(st->texture_head, p, n < 15u ? (size_t)n : 15u);
    return rt_load(ctx);
}

static void rt_unload(void *object, void *user) {
    DE_FREE(((rt_state *)user)->alloc, object, sizeof(u64));
}

static void copy_file(imp_fx *f, const char *from, const char *to) {
    void *p = NULL;
    u64 n = 0;
    ASSERT_EQ(de_file_read_all(from, &f->alloc, &p, &n), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic(to, p, n), DE_OK);
    DE_FREE(&f->alloc, p, (size_t)n + 1u);
}

TEST_F(gltf, runtime_nested_types, imp_fx) {
#if !DE_HAS_COOKER
    SKIP("this build does not link the cooker into the runtime (DE_HAS_COOKER)");
#else
    if (!de_path_exists("gtmp:/rt"))
        ASSERT_EQ(de_dir_create("gtmp:/rt"), DE_OK);
    if (!de_path_exists("gtmp:/rt/assets"))
        ASSERT_EQ(de_dir_create("gtmp:/rt/assets"), DE_OK);
    copy_file(f, "gltf:/BoxTextured.gltf", "gtmp:/rt/assets/BoxTextured.gltf");
    copy_file(f, "gltf:/BoxTextured0.bin", "gtmp:/rt/assets/BoxTextured0.bin");
    copy_file(f, "gltf:/CesiumLogoFlat.png", "gtmp:/rt/assets/CesiumLogoFlat.png");
    static const char manifest[] = "[project]\nasset_root = \"assets\"\n[assets]\n"
                                   "\"props/boxtex\" = { type = \"model\", source = "
                                   "\"BoxTextured.gltf\" }\n";
    ASSERT_EQ(de_file_write_all_atomic("gtmp:/rt/assets.toml", manifest, sizeof manifest - 1u),
              DE_OK);

    rt_state st;
    memset(&st, 0, sizeof st);
    st.alloc = &f->alloc;
    ASSERT_EQ(de_type_registry_create(&f->alloc, &f->types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &f->alloc, .types = f->types}, &f->w),
              DE_OK);
    ASSERT_TRUE(DE_HANDLE_VALID(de_builtin_components(f->w).transform));
    de_fp_env_init_thread();
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &f->tasks), DE_OK);
    de_asset_system_desc d = {.allocator = &f->alloc,
                              .tasks = f->tasks,
                              .io_threads = 1,
                              .cache_dir = "nocache:/decache",
                              .types = f->types};
    ASSERT_EQ(de_asset_system_create(&d, &f->s), DE_OK);
    de_asset_type_desc model = {.name = "model",
                                .importer_version = DE_MODEL_IMPORTER_VERSION,
                                .import = de_model_import,
                                .load = rt_load,
                                .unload = rt_unload,
                                .user = &st};
    de_asset_type_desc mesh = {.name = "mesh", .load = rt_load, .unload = rt_unload, .user = &st};
    de_asset_type_desc material = {
        .name = "material", .load = rt_load, .unload = rt_unload, .user = &st};
    de_asset_type_desc texture = {.name = "texture",
                                  .importer_version = 1,
                                  .import = fake_texture_import,
                                  .load = rt_texture_load,
                                  .unload = rt_unload,
                                  .user = &st};
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->s, &model)));
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->s, &mesh)));
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->s, &material)));
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->s, &texture)));
    ASSERT_EQ(de_asset_manifest_load(f->s, "gtmp:/rt/assets.toml"), DE_OK);

    /* The model is READY with its sub-assets (they are its dependencies). */
    de_load_handle h = de_asset_load(f->s, de_asset_id_from("props/boxtex"), 0);
    de_load_state ls = de_asset_wait(f->s, h, (u64)60 * 1000000000u);
    de_load_info info;
    (void)de_asset_status(f->s, h, &info);
    if (ls != DE_LOAD_READY)
        printf("  %s\n", info.first_error_msg);
    EXPECT_EQ((int)ls, (int)DE_LOAD_READY);
    /* The texture was cooked by the texture importer, not passed through as a PNG. */
    EXPECT_MEMEQ(st.texture_head, "FAKETEX srgb=1", 14);
    /* The prefab sub-asset loads with the built-in prefab loader (a cooked DSCN prefab). */
    de_load_handle hp = de_asset_load(f->s, de_asset_id_from("props/boxtex#prefab"), 0);
    EXPECT_EQ((int)de_asset_wait(f->s, hp, (u64)60 * 1000000000u), (int)DE_LOAD_READY);
    de_asset_release(f->s, hp);
    de_asset_release(f->s, h);
    de_asset_system_destroy(f->s);
    f->s = NULL;
#endif
}
