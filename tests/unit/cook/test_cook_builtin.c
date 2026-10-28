/*
 * test_cook_builtin.c: the cooker library's built-in importers (DENG-409): the table
 * (de_cooker_builtin_importer, tools/cook/importers/engine_importers.c) and `de-cook cook` on a
 * project of engine types only (tests/unit/cook/data/builtin_project/: two `.slang` shaders, a
 * PNG texture, a glTF model), run in-process without hooks, so every importer comes from the tool
 * itself. CTest names cook_builtin.<test>.
 *
 * Cooking needs the runtime importers (DE_HAS_COOKER: Debug and Dev); the shader checks need the
 * Slang compiler of this host. Without them the tests skip. DENG-178 adds
 * prefab_keeps_mesh_renderer (the render components of de-cook's tool world).
 */
#include "cook_project_util.h"

#include "asset/dpak/dpak_format.h"
#include "asset/dpak/dpak_reader.h"
#include "asset/scene/scene_format.h"
#include "cook/lib/de_cooker.h"

#if DE_COOK_TEST_SHADER_VIEW
#include "cook/importers/shader/slang_compile.h"
#include "gpu/shader/shader_format.h"
#include "gpu/shader/shader_internal.h"
#endif

#define NO_COOKER_MSG "this build has no runtime importers (DE_HAS_COOKER: Debug and Dev)"

#if DE_HAS_COOKER
/* Runs `de-cook cook` in-process with `hooks` (NULL: only the tool's own types). */
static int cook_no_hooks(const char *const *args, const de_cook_hooks *hooks) {
    cct_reset();
    de_cook_io io = {cct_write, NULL};
    return de_cook_cook_run(cct_argc(args), args, &io, hooks);
}
#endif

/* ---- cook_builtin.table ---------------------------------------------------------------------- */

TEST(cook_builtin, table) {
    static const char *const names[] = {"texture", "model", "shader", "sound", "sprite_clip", "tilemap"};
    for (u32 i = 0; i < (u32)(sizeof names / sizeof names[0]); ++i) {
        de_asset_type_desc d;
        ASSERT_TRUE(de_cooker_builtin_importer(names[i], &d));
        EXPECT_STREQ(d.name, names[i]);
        EXPECT_TRUE(d.import != NULL);
        EXPECT_NE(d.importer_version, 0u);
        EXPECT_GT(d.extension_count, 0u);
        ASSERT_TRUE(d.extensions != NULL);
        EXPECT_TRUE(d.load == NULL && d.unload == NULL && d.user == NULL);
        /* An engine type name, so `de-cook --check` knows it. */
        EXPECT_TRUE(de_cooker_is_engine_type(names[i]));
    }
    de_asset_type_desc d;
    ASSERT_TRUE(de_cooker_builtin_importer("shader", &d));
    EXPECT_STREQ(d.extensions[0], ".slang");
    /* Engine types without a built-in importer (yet), other names, NULL. */
    d.name = "x";
    EXPECT_FALSE(de_cooker_builtin_importer("video", &d));
    EXPECT_TRUE(d.name == NULL && d.import == NULL); /* zeroed */
    EXPECT_FALSE(de_cooker_builtin_importer("blob", &d));
    EXPECT_FALSE(de_cooker_builtin_importer("Shader", &d));
    EXPECT_FALSE(de_cooker_builtin_importer(NULL, &d));
    EXPECT_FALSE(de_cooker_builtin_importer("shader", NULL));
}

#if DE_HAS_COOKER

static u8 *read_pack(const char *rel, size_t *size) {
    char path[DE_FS_PATH_MAX];
    return cct_read(cct_path(path, sizeof path, rel), size);
}

/* The bytes of entry `id` of a pack in memory (malloc'd), its type hash in `*type`. */
static u8 *read_entry(const dpak_reader *r, const char *id, u64 *size, u64 *type) {
    u32 k = dpak_find(r, de_asset_id_from(id).hash);
    if (k == DPAK_NOT_FOUND)
        return NULL;
    dpak_toc_entry e;
    dpak_entry(r, k, &e);
    *size = e.raw_size;
    *type = e.type_hash;
    u8 *p = (u8 *)malloc((size_t)e.raw_size + 1u);
    if (p != NULL && dpak_read_entry(r, k, NULL, p, e.raw_size) != DE_OK) {
        free(p);
        p = NULL;
    }
    return p;
}

#if DE_COOK_TEST_SHADER_VIEW
/* The number of records of target `t` in a checked cooked shader. */
static u32 records_of(const de__shader_view *v, u32 t) {
    u32 n = 0;
    for (u32 i = 0; i < v->header.record_count; ++i)
        n += de__shader_view_record(v, i).target == t;
    return n;
}

static bool host_can(de__slang_target t) {
    return de__slang_target_supported(t);
}
#endif

#endif /* DE_HAS_COOKER */

/* ---- cook_builtin.project -------------------------------------------------------------------- */

/* The fixture project cooks for this host's platform with no game and no hooks: four assets and
 * the model's three sub-assets, with the right types. Shaders: each target the host can compile
 * has one record per (permutation, entry point); `shaders/triangle` has the platform's default
 * targets, `shaders/blur` asks for all three. A second run with --jobs 1 gives the same pack. */
TEST(cook_builtin, project) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    cct_fresh("project");
    char out[DE_FS_PATH_MAX];
    cct_path(out, sizeof out, "project");
    const char *platform = de_cook_platform_name(de_cook_platform_host());
    const char *a[] = {DE_COOK_BUILTIN_PROJECT "/assets.toml",
                       "--platform",
                       platform,
                       "--out",
                       out,
                       "--no-cache",
                       "--pack-name",
                       "jobs2",
                       "--jobs",
                       "2",
                       NULL};
    int rc = cook_no_hooks(a, NULL);
    if (rc != DE_COOK_EXIT_OK && strstr(g_cct.err.p, "Slang") != NULL) {
        printf("%s", g_cct.err.p);
        SKIP("no Slang compiler for this host");
    }
    ASSERT_EQ(rc, DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "assets cooked: 4 (+3 sub-assets), bundles: 1") != NULL);
    const char *b[] = {DE_COOK_BUILTIN_PROJECT "/assets.toml",
                       "--platform",
                       platform,
                       "--out",
                       out,
                       "--no-cache",
                       "--pack-name",
                       "jobs1",
                       "--jobs",
                       "1",
                       "--quiet",
                       NULL};
    ASSERT_EQ(cook_no_hooks(b, NULL), DE_COOK_EXIT_OK);

    size_t n = 0, n1 = 0;
    u8 *pack = read_pack("project/jobs2.dpak", &n);
    u8 *pack1 = read_pack("project/jobs1.dpak", &n1);
    ASSERT_TRUE(pack != NULL && pack1 != NULL);
    ASSERT_EQ(n, n1);
    EXPECT_MEMEQ(pack, pack1, n);
    free(pack1);

    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open_memory(pack, n, &(dpak_open_desc){.verify = true}, &r), DE_OK);
    EXPECT_EQ(dpak_entry_count(r), 7u);
    static const struct {
        const char *id, *type;
    } k[] = {{"shaders/triangle", "shader"},
             {"shaders/blur", "shader"},
             {"textures/checker", "texture"},
             {"models/box", "model"}};
    for (u32 i = 0; i < 4; ++i) {
        u64 size = 0, type = 0;
        u8 *p = read_entry(r, k[i].id, &size, &type);
        ASSERT_TRUE(p != NULL);
        EXPECT_EQ(type, de_hash64_str(k[i].type));
        EXPECT_GT(size, 0u);
        free(p);
    }
    /* The model's sub-assets (mesh, material, prefab) are entries of their own. */
    u32 subs = 0;
    for (u32 i = 0; i < dpak_entry_count(r); ++i) {
        dpak_toc_entry e;
        dpak_entry(r, i, &e);
        subs += (e.flags & DPAK_ENTRY_SUB_ASSET) != 0;
    }
    EXPECT_EQ(subs, 3u);

#if DE_COOK_TEST_SHADER_VIEW
    const de_cook_platform host = de_cook_platform_host();
    /* triangle: the default targets of the platform (shader_importer.h), what the host can do. */
    u64 size = 0, type = 0;
    u8 *tri = read_entry(r, "shaders/triangle", &size, &type);
    ASSERT_TRUE(tri != NULL);
    de__shader_view v;
    ASSERT_EQ(de__shader_view_init(tri, size, &v), DE_OK);
    EXPECT_EQ(v.header.entry_count, 2u);
    EXPECT_EQ(v.header.permutation_count, 1u);
    EXPECT_EQ(records_of(&v, DE__SHADER_TARGET_SPIRV), 2u);
    EXPECT_EQ(records_of(&v, DE__SHADER_TARGET_METALLIB),
              host == DE_COOK_PLATFORM_MACOS && host_can(DE__SLANG_METALLIB) ? 2u : 0u);
    EXPECT_EQ(records_of(&v, DE__SHADER_TARGET_DXIL),
              host == DE_COOK_PLATFORM_WINDOWS && host_can(DE__SLANG_DXIL) ? 2u : 0u);
    const char *name = NULL;
    (void)de__shader_view_entry(&v, 0, &name);
    EXPECT_STREQ(name, "vs_main");
    free(tri);

    /* blur: every target requested; 2 permutations x 1 entry point per compiled target. */
    u8 *blur = read_entry(r, "shaders/blur", &size, &type);
    ASSERT_TRUE(blur != NULL);
    ASSERT_EQ(de__shader_view_init(blur, size, &v), DE_OK);
    EXPECT_EQ(v.header.permutation_count, 2u);
    EXPECT_STREQ(de__shader_view_permutation(&v, 1), "WIDE=1");
    EXPECT_EQ(records_of(&v, DE__SHADER_TARGET_SPIRV), 2u);
    EXPECT_EQ(records_of(&v, DE__SHADER_TARGET_METALLIB), host_can(DE__SLANG_METALLIB) ? 2u : 0u);
    EXPECT_EQ(records_of(&v, DE__SHADER_TARGET_DXIL), host_can(DE__SLANG_DXIL) ? 2u : 0u);
    i32 rec = de__shader_view_find(&v, DE__SHADER_TARGET_SPIRV, 1, 0);
    ASSERT_GE(rec, 0);
    de__shader_record rr = de__shader_view_record(&v, (u32)rec);
    EXPECT_EQ(rr.stage, (u32)DE__SHADER_STAGE_COMPUTE);
    EXPECT_EQ(rr.threadgroup[0], 64u);
    EXPECT_EQ(rr.push_constant_bytes, 12u);
    /* SPIR-V magic at the record's code. */
    u32 magic = 0;
    memcpy(&magic, blur + rr.code_offset, 4);
    EXPECT_EQ(magic, 0x07230203u);
    free(blur);
#endif
    dpak_close(r);
    free(pack);
#endif
}

/* ---- cook_builtin.project_type_wins ---------------------------------------------------------- */

#if DE_HAS_COOKER
/* A project `shader` type whose importer copies the source: it replaces the built-in one. */
static de_result copy_import(de_asset_import_ctx *ctx) {
    u64 n = 0;
    const void *src = de_asset_import_source(ctx, &n);
    return de_asset_import_write(ctx, src, n);
}

static de_result register_copy_shader(void *user, de_asset_system *s, de_type_registry *types) {
    (void)user;
    (void)types;
    static const char *const exts[] = {".slang"};
    de_asset_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = "shader";
    d.extensions = exts;
    d.extension_count = 1;
    d.importer_version = 7;
    d.import = copy_import;
    return DE_HANDLE_VALID(de_asset_type_register(s, &d)) ? DE_OK : DE_ERR_INTERNAL;
}
#endif

/* A type the project (the hooks, or a --game library) registers wins over the built-in importer
 * of the same name; the other built-ins stay. */
TEST(cook_builtin, project_type_wins) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    static const char manifest[] = "[project]\nasset_root = \"assets\"\n[assets]\n"
                                   "\"s\" = { type = \"shader\", source = \"s.slang\" }\n"
                                   "\"t\" = { type = \"texture\", source = \"t.png\" }\n";
    static const char shader_src[] = "this is not Slang";
    cct_fresh("wins");
    cct_put("wins/proj/assets.toml", manifest, sizeof manifest - 1);
    cct_put("wins/proj/assets/s.slang", shader_src, sizeof shader_src - 1);
    size_t png_size = 0;
    u8 *png = cct_read(DE_COOK_BUILTIN_PROJECT "/assets/textures/checker.png", &png_size);
    ASSERT_TRUE(png != NULL);
    cct_put("wins/proj/assets/t.png", png, png_size);
    free(png);
    char mpath[DE_FS_PATH_MAX], out[DE_FS_PATH_MAX];
    cct_path(mpath, sizeof mpath, "wins/proj/assets.toml");
    cct_path(out, sizeof out, "wins");
    const char *a[] = {mpath, "--platform", "linux", "--out", out, "--no-cache", "--quiet", NULL};
    const de_cook_hooks hooks = {register_copy_shader, NULL, NULL};
    ASSERT_EQ(cook_no_hooks(a, &hooks), DE_COOK_EXIT_OK);

    size_t n = 0;
    u8 *pack = read_pack("wins/game.dpak", &n);
    ASSERT_TRUE(pack != NULL);
    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open_memory(pack, n, &(dpak_open_desc){.verify = true}, &r), DE_OK);
    u64 size = 0, type = 0;
    u8 *s = read_entry(r, "s", &size, &type);
    ASSERT_TRUE(s != NULL);
    EXPECT_EQ(type, de_hash64_str("shader"));
    ASSERT_EQ(size, (u64)(sizeof shader_src - 1));
    EXPECT_MEMEQ(s, shader_src, sizeof shader_src - 1);
    free(s);
    u8 *t = read_entry(r, "t", &size, &type);
    ASSERT_TRUE(t != NULL);
    EXPECT_EQ(type, de_hash64_str("texture"));
    EXPECT_MEMEQ(t, "DTEX", 4); /* cooked by the built-in texture importer */
    free(t);
    dpak_close(r);
    free(pack);
#endif
}

/* ---- cook_builtin.prefab_keeps_mesh_renderer ------------------------------------------------- */

/* DENG-178: de-cook's tool world has the render components, so the prefab of a glTF model keeps
 * its DeMeshRenderer (before, it was skipped as an unknown component). The cooked prefab's type
 * table names DeMeshRenderer and DeTransform, the mesh node's entity has both, and the mesh and
 * material IDs are its asset references. Headless builds have no render module: skipped. */
TEST(cook_builtin, prefab_keeps_mesh_renderer) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#elif !DE_HAS_RENDER
    SKIP("headless build: no render module, so de-cook has no render components");
#else
    static const char manifest[] = "[project]\nasset_root = \"assets\"\n[assets]\n"
                                   "\"models/box\" = { type = \"model\", source = \"box.glb\" }\n";
    cct_fresh("prefab");
    cct_put("prefab/proj/assets.toml", manifest, sizeof manifest - 1);
    size_t glb_size = 0;
    u8 *glb = cct_read(DE_COOK_BUILTIN_PROJECT "/assets/models/box.glb", &glb_size);
    ASSERT_TRUE(glb != NULL);
    cct_put("prefab/proj/assets/box.glb", glb, glb_size);
    free(glb);
    char mpath[DE_FS_PATH_MAX], out[DE_FS_PATH_MAX];
    cct_path(mpath, sizeof mpath, "prefab/proj/assets.toml");
    cct_path(out, sizeof out, "prefab");
    const char *a[] = {mpath, "--platform", "linux", "--out", out, "--no-cache", "--quiet", NULL};
    ASSERT_EQ(cook_no_hooks(a, NULL), DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.err.p, "DeMeshRenderer") == NULL);

    size_t n = 0;
    u8 *pack = read_pack("prefab/game.dpak", &n);
    ASSERT_TRUE(pack != NULL);
    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open_memory(pack, n, &(dpak_open_desc){.verify = true}, &r), DE_OK);
    u64 size = 0, type = 0;
    u8 *pf = read_entry(r, "models/box#prefab", &size, &type);
    ASSERT_TRUE(pf != NULL);
    EXPECT_EQ(type, de_hash64_str("prefab"));
    de__scene_view v;
    char err[256];
    ASSERT_EQ(de__scene_view_parse(pf, size, &v, err, sizeof err), DE_OK);
    EXPECT_EQ((int)v.kind, (int)DE__SCENE_KIND_PREFAB);
    /* Types sorted by name: DeMeshRenderer, DeTransform. */
    ASSERT_EQ(v.type_count, 2u);
    EXPECT_STREQ(de__scene_type_at(&v, 0).name, "DeMeshRenderer");
    EXPECT_STREQ(de__scene_type_at(&v, 1).name, "DeTransform");
    u32 renderers = 0;
    for (u32 i = 0; i < v.entity_count; ++i) {
        const de__scene_entity_rec e = de__scene_entity_at(&v, i);
        for (u32 k = 0; k < e.component_count; ++k)
            renderers += de__scene_comp_at(&v, e.first_component + k).type_index == 0u;
    }
    EXPECT_GE(renderers, 1u);
    EXPECT_GE(v.asset_count, 2u); /* the mesh and the material */
    free(pf);
    dpak_close(r);
    free(pack);
#endif
}
