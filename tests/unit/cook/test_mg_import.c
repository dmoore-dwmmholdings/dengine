/*
 * test_mg_import.c: the `material_graph` asset importer (DENG-770,
 * tools/cook/importers/material_graph/mg_import.c) and the DMGR format
 * (src/render/formats/de_cooked_material_graph.h). `de-cook cook` runs in-process without hooks,
 * so the importer comes from the cooker's built-in table. CTest names mg_import.<test>.
 *
 * Cooking needs the runtime importers (DE_HAS_COOKER) and the Slang compiler of this host;
 * without them the cook tests skip.
 */
#include "cook_project_util.h"

#include "asset/dpak/dpak_format.h"
#include "asset/dpak/dpak_reader.h"
#include "cook/importers/material_graph/mg_import.h"
#include "cook/lib/de_cooker.h"
#include "gpu/shader/shader_format.h"
#include "render/formats/de_cooked_material.h"
#include "render/formats/de_cooked_material_graph.h"

#include <stddef.h>

#define NO_COOKER_MSG "this build has no runtime importers (DE_HAS_COOKER: Debug and Dev)"

#define HEADER "kind = \"material\"\nversion = 1\nnext_id = 64\n"

/* The graphs and expect_fails are used only where the cooker runs (DENG-795). */
#if DE_HAS_COOKER
/* Two colour and float parameters, one texture, layered output. */
static const char rock_graph[] =
    HEADER "[[node]]\nid = \"n1\"\ntype = \"param.color\"\n"
           "values = { name = \"tint\", r = 0.5, g = 0.25, b = 1.0 }\n"
           "[[node]]\nid = \"n2\"\ntype = \"param.float\"\n"
           "values = { name = \"wet\", value = 0.75, min = 0.0, max = 2.0 }\n"
           "[[node]]\nid = \"n3\"\ntype = \"param.float\"\n"
           "values = { name = \"rough\", value = 0.375 }\n"
           "[[node]]\nid = \"n4\"\ntype = \"texture.sample\"\nvalues = { texture = \"albedo\" }\n"
           "[[node]]\nid = \"n5\"\ntype = \"output.base\"\n"
           "[[link]]\nfrom = \"n1.out\"\nto = \"n5.base_color\"\n"
           "[[link]]\nfrom = \"n3.out\"\nto = \"n5.roughness\"\n";

/* A link from a node that does not exist. */
static const char broken_graph[] = HEADER "[[node]]\nid = \"n5\"\ntype = \"output.base\"\n"
                                          "[[link]]\nfrom = \"n99.out\"\nto = \"n5.roughness\"\n";
#endif

/* ---- mg_import.table ------------------------------------------------------------------------- */

TEST(mg_import, table) {
    de_asset_type_desc d;
    ASSERT_TRUE(de_cooker_builtin_importer("material_graph", &d));
    EXPECT_STREQ(d.name, "material_graph");
    EXPECT_TRUE(d.import == de_mg_import);
    EXPECT_EQ(d.extension_count, 0u);
    EXPECT_TRUE(de_cooker_is_engine_type("material_graph"));
    /* The version hashes the engine shader sources: stable within a process, never 0. */
    const u32 v = de_mg_importer_version();
    EXPECT_NE(v, 0u);
    EXPECT_EQ(de_mg_importer_version(), v);
    EXPECT_EQ(d.importer_version, v);
    EXPECT_NE(v, DE_MG_IMPORTER_BASE_VERSION);
}

/* ---- DMGR format ----------------------------------------------------------------------------- */

/* A minimal valid DMGR: one float parameter, a 16-byte shader. */
static size_t make_dmgr(u8 *b) {
    const size_t shader_at = 64u + 168u + 8u; /* 16-aligned */
    const size_t total = shader_at + 16u;
    memset(b, 0, total);
    memcpy(b, "DMGR", 4);
    de_cooked_put_u32(b + 4, 1u);
    de_cooked_put_u32(b + 8, (u32)total);
    de_cooked_put_u32(b + 16, 1u);
    de_cooked_put_u32(b + 20, (u32)shader_at);
    de_cooked_put_u32(b + 24, 16u);
    memcpy(b + 32, "rock", 4);
    u8 *p = b + 64;
    de_cooked_put_u32(p, DE_COOKED_MG_PARAM_FLOAT);
    de_cooked_put_u32(p + 4, 2u);
    de_cooked_put_u32(p + 8, 1u);
    de_cooked_put_f32(p + 16, 0.5f);
    de_cooked_put_f32(p + 32, 1.0f);
    memcpy(p + 40, "rough", 5);
    memcpy(p + 104, "n3", 2);
    memset(b + shader_at, 0xab, 16);
    return total;
}

TEST(mg_import, dmgr_parse) {
    u8 b[512];
    const size_t n = make_dmgr(b);
    de_cooked_material_graph_view v;
    const char *why = NULL;
    ASSERT_TRUE(de_cooked_material_graph_parse(b, n, &v, &why));
    EXPECT_STREQ(v.model, "rock");
    ASSERT_EQ(v.param_count, 1u);
    de_cooked_mg_param p = de_cooked_material_graph_param(&v, 0);
    EXPECT_STREQ(p.name, "rough");
    EXPECT_STREQ(p.node, "n3");
    EXPECT_EQ(p.row, 2u);
    EXPECT_EQ(p.component, 1u);
    EXPECT_FLOAT_EQ(p.value[0], 0.5f, 0.0f);
    EXPECT_EQ(v.shader_size, 16u);
    EXPECT_EQ(v.shader[0], 0xab);

    /* Versioning: another version is refused, not misread. */
    u8 c[512];
    memcpy(c, b, n);
    de_cooked_put_u32(c + 4, 2u);
    EXPECT_FALSE(de_cooked_material_graph_parse(c, n, &v, &why));
    EXPECT_STREQ(why, "unsupported format version");
    /* Truncated, bad magic, bad sizes, bad records, bad names. */
    EXPECT_FALSE(de_cooked_material_graph_parse(b, n - 1u, &v, &why));
    EXPECT_FALSE(de_cooked_material_graph_parse(b, 10u, &v, &why));
    EXPECT_FALSE(de_cooked_material_graph_parse(NULL, n, &v, &why));
    memcpy(c, b, n);
    c[0] = 'X';
    EXPECT_FALSE(de_cooked_material_graph_parse(c, n, &v, &why));
    memcpy(c, b, n);
    de_cooked_put_u32(c + 64 + 4, 10u); /* row out of range */
    EXPECT_FALSE(de_cooked_material_graph_parse(c, n, &v, &why));
    EXPECT_STREQ(why, "bad parameter record");
    memcpy(c, b, n);
    de_cooked_put_u32(c + 64, 3u); /* unknown kind */
    EXPECT_FALSE(de_cooked_material_graph_parse(c, n, &v, &why));
    memcpy(c, b, n);
    memset(c + 64 + 40, 'a', 64); /* name not terminated */
    EXPECT_FALSE(de_cooked_material_graph_parse(c, n, &v, &why));
    memcpy(c, b, n);
    memset(c + 32, 0, 32); /* empty model name */
    EXPECT_FALSE(de_cooked_material_graph_parse(c, n, &v, &why));
    memcpy(c, b, n);
    de_cooked_put_u32(c + 20, 64u + 168u + 4u); /* shader offset not aligned */
    EXPECT_FALSE(de_cooked_material_graph_parse(c, n, &v, &why));
    memcpy(c, b, n);
    de_cooked_put_u32(c + 16, DE_COOKED_MATERIAL_GRAPH_MAX_PARAMS + 1u);
    EXPECT_FALSE(de_cooked_material_graph_parse(c, n, &v, &why));
    EXPECT_TRUE(v.params == NULL && v.shader == NULL); /* cleared on failure */
}

TEST(mg_import, row_map_matches_the_pack) {
    /* Every DMAT float a row component maps to is distinct and in range. */
    bool used[DE_COOKED_MATERIAL_PARAM_FLOATS] = {false};
    for (u32 r = 0; r < DE_COOKED_MATERIAL_GRAPH_ROWS; ++r) {
        for (u32 c = 0; c < 4u; ++c) {
            const i32 f = de_cooked_material_graph_param_float(r, c);
            ASSERT_TRUE(f >= 0 && f < (i32)DE_COOKED_MATERIAL_PARAM_FLOATS);
            EXPECT_FALSE(used[f]);
            used[f] = true;
        }
    }
    EXPECT_EQ(de_cooked_material_graph_param_float(10, 0), -1);
    EXPECT_EQ(de_cooked_material_graph_param_float(0, 4), -1);
    /* Spot checks against de_cooked_material_params. */
    EXPECT_EQ(de_cooked_material_graph_param_float(0, 0),
              (i32)(offsetof(de_cooked_material_params, base_color) / 4u));
    EXPECT_EQ(de_cooked_material_graph_param_float(2, 1),
              (i32)(offsetof(de_cooked_material_params, roughness) / 4u));
    EXPECT_EQ(de_cooked_material_graph_param_float(9, 0),
              (i32)(offsetof(de_cooked_material_params, subsurface_radius) / 4u));
}

#if DE_HAS_COOKER

static int cook_run(const char *const *args) {
    cct_reset();
    de_cook_io io = {cct_write, NULL};
    return de_cook_cook_run(cct_argc(args), args, &io, NULL);
}

/* Writes project `sub` (rock graph, checker texture) with the asset line `line`. */
static void put_project(const char *sub, const char *graph_file, const char *graph,
                        const char *line) {
    char p[256], m[2048];
    snprintf(m, sizeof m,
             "[project]\nasset_root = \"assets\"\n[assets]\n"
             "\"tex/a\" = { type = \"texture\", source = \"checker.png\" }\n%s\n",
             line);
    snprintf(p, sizeof p, "%s/proj/assets.toml", sub);
    cct_put(p, m, strlen(m));
    snprintf(p, sizeof p, "%s/proj/assets/%s", sub, graph_file);
    cct_put(p, graph, strlen(graph));
    size_t png_size = 0;
    u8 *png = cct_read(DE_COOK_BUILTIN_PROJECT "/assets/textures/checker.png", &png_size);
    ASSERT_TRUE(png != NULL);
    snprintf(p, sizeof p, "%s/proj/assets/checker.png", sub);
    cct_put(p, png, png_size);
    free(png);
}

/* Cooks project `sub` for linux (SPIR-V). DE_COOK_EXIT_* */
static int cook_project(const char *sub) {
    char mpath[DE_FS_PATH_MAX], out[DE_FS_PATH_MAX], rel[256];
    snprintf(rel, sizeof rel, "%s/proj/assets.toml", sub);
    cct_path(mpath, sizeof mpath, rel);
    cct_path(out, sizeof out, sub);
    const char *a[] = {mpath, "--platform", "linux", "--out", out, "--no-cache", "--quiet", NULL};
    return cook_run(a);
}

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

typedef struct cooked {
    u8 *pack;
    size_t pack_size;
    dpak_reader *r;
    u8 *graph, *material;
    u64 graph_size, material_size;
    u64 graph_type, material_type;
} cooked;

static bool open_cooked(const char *sub, const char *id, cooked *c) {
    memset(c, 0, sizeof *c);
    char rel[256], sid[256];
    snprintf(rel, sizeof rel, "%s/game.dpak", sub);
    char path[DE_FS_PATH_MAX];
    c->pack = cct_read(cct_path(path, sizeof path, rel), &c->pack_size);
    if (c->pack == NULL ||
        dpak_open_memory(c->pack, c->pack_size, &(dpak_open_desc){.verify = true}, &c->r) != DE_OK)
        return false;
    snprintf(sid, sizeof sid, "%s#" DE_COOKED_MATERIAL_GRAPH_MATERIAL, id);
    c->graph = read_entry(c->r, id, &c->graph_size, &c->graph_type);
    c->material = read_entry(c->r, sid, &c->material_size, &c->material_type);
    return c->graph != NULL && c->material != NULL;
}

static void close_cooked(cooked *c) {
    free(c->graph);
    free(c->material);
    if (c->r != NULL)
        dpak_close(c->r);
    free(c->pack);
}

/* Skips when the host has no Slang compiler. */
#define COOK_OR_SKIP(sub)                                                                          \
    do {                                                                                           \
        int rc_ = cook_project(sub);                                                               \
        if (rc_ != DE_COOK_EXIT_OK && strstr(g_cct.err.p, "Slang") != NULL &&                      \
            strstr(g_cct.err.p, "node") == NULL) {                                                 \
            printf("%s", g_cct.err.p);                                                             \
            SKIP("no Slang compiler for this host");                                               \
        }                                                                                          \
        if (rc_ != DE_COOK_EXIT_OK)                                                                \
            printf("%s", g_cct.err.p);                                                             \
        ASSERT_EQ(rc_, DE_COOK_EXIT_OK);                                                           \
    } while (0)

#endif /* DE_HAS_COOKER */

/* ---- mg_import.cooks_graph ------------------------------------------------------------------- */

/* The graph cooks to DMGR (model, parameter layout, a SPIR-V cs_resolve_model) and its default
 * material (DMAT: defaults at their row positions, the model's hash, the bound texture). */
TEST(mg_import, cooks_graph) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    cct_fresh("cook");
    put_project("cook", "rock.mgraph.toml", rock_graph,
                "\"mats/rock\" = { type = \"material_graph\", source = \"rock.mgraph.toml\", "
                "textures = [\"albedo=tex/a\"], alpha_mode = \"mask\", double_sided = true }");
    COOK_OR_SKIP("cook");
    cooked c;
    ASSERT_TRUE(open_cooked("cook", "mats/rock", &c));
    EXPECT_EQ(c.graph_type, de_hash64_str("material_graph"));
    EXPECT_EQ(c.material_type, de_hash64_str("material"));

    de_cooked_material_graph_view v;
    const char *why = NULL;
    ASSERT_TRUE(de_cooked_material_graph_parse(c.graph, c.graph_size, &v, &why));
    EXPECT_STREQ(v.model, "rock"); /* the file name up to its first '.' */
    ASSERT_EQ(v.param_count, 4u);
    de_cooked_mg_param p = de_cooked_material_graph_param(&v, 0);
    EXPECT_STREQ(p.name, "tint");
    EXPECT_STREQ(p.node, "n1");
    EXPECT_EQ(p.kind, (u32)DE_COOKED_MG_PARAM_COLOR);
    EXPECT_FLOAT_EQ(p.value[1], 0.25f, 0.0f);
    p = de_cooked_material_graph_param(&v, 1);
    EXPECT_STREQ(p.name, "wet");
    EXPECT_FLOAT_EQ(p.max, 2.0f, 0.0f);
    const u32 wet_row = p.row, wet_comp = p.component;
    p = de_cooked_material_graph_param(&v, 3);
    EXPECT_STREQ(p.name, "albedo");
    EXPECT_EQ(p.kind, (u32)DE_COOKED_MG_PARAM_TEXTURE);
    EXPECT_EQ(p.slot, 0u);
    ASSERT_GE(v.shader_size, 16u);
    EXPECT_EQ(de_cooked_get_u32(v.shader), DE__SHADER_MAGIC);

    de_cooked_material_view m;
    ASSERT_TRUE(de_cooked_material_parse(c.material, c.material_size, &m, &why));
    EXPECT_EQ(m.version, DE_COOKED_MATERIAL_VERSION);
    EXPECT_EQ(m.model_hash, de_cooked_material_model_hash("rock"));
    EXPECT_EQ(m.alpha_mode, (u32)DE_COOKED_ALPHA_MASK);
    EXPECT_TRUE((m.flags & DE_COOKED_MAT_DOUBLE_SIDED) != 0u);
    EXPECT_FLOAT_EQ(m.params.base_color[0], 0.5f, 0.0f); /* tint: row 0 */
    EXPECT_FLOAT_EQ(m.params.base_color[2], 1.0f, 0.0f);
    f32 f[DE_COOKED_MATERIAL_PARAM_FLOATS];
    memcpy(f, &m.params, sizeof f);
    EXPECT_FLOAT_EQ(f[de_cooked_material_graph_param_float(wet_row, wet_comp)], 0.75f, 0.0f);
    ASSERT_EQ(m.texture_count, 1u);
    EXPECT_EQ(m.textures[0].slot, 0u);
    EXPECT_EQ(m.textures[0].asset_id, de_asset_id_from("tex/a").hash);
    close_cooked(&c);
#endif
}

/* ---- mg_import.cooks_layer_outputs ----------------------------------------------------------- */

/* DENG-772: the transmission, volume and subsurface outputs set their feature bits in DMGR, and
 * the subsurface amount and mean free path land in the DMAT cells the SSS pass reads. */
TEST(mg_import, cooks_layer_outputs) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    static const char g[] =
        HEADER "[[node]]\nid = \"n1\"\ntype = \"output.base\"\n"
               "[[node]]\nid = \"n2\"\ntype = \"output.transmission\"\n"
               "[[node]]\nid = \"n3\"\ntype = \"output.volume\"\n"
               "[[node]]\nid = \"n4\"\ntype = \"output.subsurface\"\n"
               "values = { weight = 0.5, radius_r = 0.25, radius_g = 0.125, radius_b = 0.0625 }\n"
               "[[node]]\nid = \"n5\"\ntype = \"param.float\"\n"
               "values = { name = \"wet\", value = 0.75 }\n";
    cct_fresh("layers");
    put_project("layers", "skin.mgraph.toml", g,
                "\"mats/skin\" = { type = \"material_graph\", source = \"skin.mgraph.toml\" }");
    COOK_OR_SKIP("layers");
    cooked c;
    ASSERT_TRUE(open_cooked("layers", "mats/skin", &c));
    de_cooked_material_graph_view v;
    const char *why = NULL;
    ASSERT_TRUE(de_cooked_material_graph_parse(c.graph, c.graph_size, &v, &why));
    EXPECT_EQ(v.features, 0xB0u); /* TRANSMISSION | VOLUME | SUBSURFACE */
    ASSERT_EQ(v.param_count, 3u);
    de_cooked_mg_param p = de_cooked_material_graph_param(&v, 0);
    EXPECT_STREQ(p.name, "subsurface.weight");
    EXPECT_STREQ(p.node, "n4");
    de_cooked_material_view m;
    ASSERT_TRUE(de_cooked_material_parse(c.material, c.material_size, &m, &why));
    EXPECT_FLOAT_EQ(m.params.subsurface, 0.5f, 0.0f);
    EXPECT_FLOAT_EQ(m.params.subsurface_radius[0], 0.25f, 0.0f);
    EXPECT_FLOAT_EQ(m.params.subsurface_radius[1], 0.125f, 0.0f);
    EXPECT_FLOAT_EQ(m.params.subsurface_radius[2], 0.0625f, 0.0f);
    /* "wet" packs into row 0 (base_color.x), clear of the reserved cells. */
    p = de_cooked_material_graph_param(&v, 2);
    EXPECT_STREQ(p.name, "wet");
    EXPECT_EQ(p.row, 0u);
    close_cooked(&c);
#endif
}

/* ---- mg_import.deterministic ----------------------------------------------------------------- */

TEST(mg_import, deterministic) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    static const char line[] =
        "\"mats/rock\" = { type = \"material_graph\", source = \"rock.mgraph.toml\", "
        "model = \"rock_v\" }";
    cct_fresh("det1");
    cct_fresh("det2");
    put_project("det1", "rock.mgraph.toml", rock_graph, line);
    put_project("det2", "rock.mgraph.toml", rock_graph, line);
    COOK_OR_SKIP("det1");
    COOK_OR_SKIP("det2");
    cooked a, b;
    ASSERT_TRUE(open_cooked("det1", "mats/rock", &a));
    ASSERT_TRUE(open_cooked("det2", "mats/rock", &b));
    ASSERT_EQ(a.graph_size, b.graph_size);
    EXPECT_MEMEQ(a.graph, b.graph, (size_t)a.graph_size);
    ASSERT_EQ(a.material_size, b.material_size);
    EXPECT_MEMEQ(a.material, b.material, (size_t)a.material_size);
    de_cooked_material_graph_view v;
    ASSERT_TRUE(de_cooked_material_graph_parse(a.graph, a.graph_size, &v, NULL));
    EXPECT_STREQ(v.model, "rock_v"); /* the `model` setting */
    close_cooked(&a);
    close_cooked(&b);
#endif
}

/* ---- mg_import.errors ------------------------------------------------------------------------ */

#if DE_HAS_COOKER
static void expect_fails(const char *sub, const char *graph, const char *line,
                         const char *const *needles) {
    cct_fresh(sub);
    put_project(sub, "rock.mgraph.toml", graph, line);
    EXPECT_NE(cook_project(sub), DE_COOK_EXIT_OK);
    for (const char *const *n = needles; *n != NULL; ++n) {
        const bool found = strstr(g_cct.err.p, *n) != NULL;
        if (!found)
            printf("missing \"%s\" in:\n%s\n", *n, g_cct.err.p);
        EXPECT_TRUE(found);
    }
}
#endif

#define ROCK(extra)                                                                                \
    "\"mats/rock\" = { type = \"material_graph\", source = \"rock.mgraph.toml\"" extra " }"

TEST(mg_import, errors) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    {
        const char *n[] = {"rock.mgraph.toml", "n99", NULL}; /* the node in the message */
        expect_fails("e_graph", broken_graph, ROCK(""), n);
    }
    {
        const char *n[] = {"\"standard\"", "built-in", NULL};
        expect_fails("e_builtin", rock_graph, ROCK(", model = \"standard\""), n);
    }
    {
        const char *n[] = {"bad name", NULL};
        expect_fails("e_name", rock_graph, ROCK(", model = \"bad name\""), n);
    }
    {
        const char *n[] = {"no texture parameter", "nope", NULL};
        expect_fails("e_tex", rock_graph, ROCK(", textures = [\"nope=tex/a\"]"), n);
    }
    {
        const char *n[] = {"is not", NULL};
        expect_fails("e_tex2", rock_graph, ROCK(", textures = [\"albedo\"]"), n);
    }
    {
        const char *n[] = {"alpha_mode", NULL};
        expect_fails("e_alpha", rock_graph, ROCK(", alpha_mode = \"glass\""), n);
    }
    {
        const char *n[] = {"not toml", NULL};
        (void)n;
        const char *g[] = {"rock.mgraph.toml", NULL};
        expect_fails("e_parse", "this is [ not toml", ROCK(""), g);
    }
#endif
}
