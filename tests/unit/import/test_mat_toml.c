/*
 * test_mat_toml.c: the `.mat.toml` material importer (DENG-302, SPEC §11.2, §12.6;
 * tools/cook/importers/material/mat_toml_import.h). CTest names import_material.import.<test>.
 *
 *   mat_toml_import_basic    an examples/assets.toml-style `materials/stone.mat.toml`: every
 *                            value, the defaults, flags, alpha mode, textures (slot, UV set, ID),
 *                            the dependencies, the model hash of a game model; Unlit
 *   mat_toml_unknown_keys    unknown keys fail the import and the message names each with its
 *                            line; wrong value types and conflicting model keys fail too
 *   mat_toml_deterministic   the same material with its keys reordered and reformatted cooks to
 *                            the same bytes (the cooker also runs every import twice)
 *
 * Sources are written to mtmp:/ (test_import_material_tmp/ in the build tree).
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/manifest_internal.h"
#include "cook/importers/material/mat_toml_import.h"
#include "cook/lib/de_cooker.h"
#include "render/formats/de_cooked_material.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct mt_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_cooker *c;
} mt_fx;

static void mt_fx_setup(mt_fx *f) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("mtmp", DE_IMPORT_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
        if (!de_path_exists("mtmp:/materials"))
            ASSERT_EQ(de_dir_create("mtmp:/materials"), DE_OK);
        mounted = true;
    }
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.allocator = &f->alloc,
                                                 .verify_determinism = DE_COOK_VERIFY_ON},
                               &f->c),
              DE_OK);
    de_asset_type_desc t = de_mat_toml_import_type_desc();
    ASSERT_EQ(de_cooker_register_type(f->c, &t), DE_OK);
}

static void mt_fx_teardown(mt_fx *f) {
    if (f->c != NULL)
        de_cooker_destroy(f->c);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    de_log_shutdown();
}

/* Writes `text` to mtmp:/materials/<name> and cooks it as material `env/<name>`. */
static de_result cook_text(mt_fx *f, const char *name, const char *text, de_cook_result *out) {
    char path[128], src[128], id[128];
    snprintf(path, sizeof path, "mtmp:/materials/%s", name);
    snprintf(src, sizeof src, "materials/%s", name);
    snprintf(id, sizeof id, "env/%s", name);
    if (de_file_write_all_atomic(path, text, strlen(text)) != DE_OK)
        return DE_ERR_IO;
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = id;
    a.type = "material";
    a.source = src;
    a.file = "mtmp:/assets.toml";
    de_error_clear();
    return de_cooker_cook(f->c, &(de_cook_input){.asset = &a, .asset_root = "mtmp:/"}, out);
}

static bool error_says(const char *needle) {
    const de_error *e = de_error_last();
    if (e == NULL || strstr(e->message, needle) == NULL) {
        printf("  error: %s (wanted \"%s\")\n", e != NULL ? e->message : "(none)", needle);
        return false;
    }
    return true;
}

static bool has_dep(const de_cook_result *r, const char *path) {
    const u64 h = de_asset_id_from(path).hash;
    for (u32 i = 0; i < r->dependency_count; ++i)
        if (r->dependencies[i].hash == h)
            return true;
    return false;
}

/* ---- mat_toml_import_basic ------------------------------------------------------------------- */

static const char k_stone[] =
    "# examples/assets.toml: \"env/stone\" = { type = \"material\", source = "
    "\"materials/stone.mat.toml\" }\n"
    "base_color = [0.5, 0.45, 0.4]\n"
    "metallic = 0\n"
    "roughness = 0.85\n"
    "normal_scale = 0.75\n"
    "clearcoat = 0.25\n"
    "sheen_color = [0.1, 0.2, 0.3]\n"
    "subsurface = 0.5\n"
    "subsurface_radius = [1.0, 0.5, 0.25]\n"
    "diffuse_transmission_color = [0.9, 0.8, 0.7]\n"
    "alpha_mode = \"mask\"\n"
    "alpha_cutoff = 0.3\n"
    "double_sided = true\n"
    "base_color_tex = \"env/stone_albedo\"\n"
    "normal_tex = \"env/stone_normal\"\n"
    "normal_texcoord = 1\n"
    "occlusion_tex = \"env/stone_ao\"\n";

TEST_F(import, mat_toml_import_basic, mt_fx) {
    de_cook_result r;
    ASSERT_EQ(cook_text(f, "stone.mat.toml", k_stone, &r), DE_OK);
    de_cooked_material_view v;
    const char *why = NULL;
    ASSERT_TRUE(de_cooked_material_parse(r.data, r.size, &v, &why));
    EXPECT_EQ(v.version, DE_COOKED_MATERIAL_VERSION);
    EXPECT_EQ(v.model_hash, 0u); /* Standard */
    EXPECT_FLOAT_EQ(v.params.base_color[0], 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(v.params.base_color[2], 0.4f, 1e-6f);
    EXPECT_FLOAT_EQ(v.params.base_color[3], 1.0f, 1e-6f); /* 3 numbers: alpha 1 */
    EXPECT_FLOAT_EQ(v.params.metallic, 0.0f, 1e-6f);      /* an integer is a number */
    EXPECT_FLOAT_EQ(v.params.roughness, 0.85f, 1e-6f);
    EXPECT_FLOAT_EQ(v.params.normal_scale, 0.75f, 1e-6f);
    EXPECT_FLOAT_EQ(v.params.clearcoat, 0.25f, 1e-6f);
    EXPECT_FLOAT_EQ(v.params.sheen_color[2], 0.3f, 1e-6f);
    EXPECT_FLOAT_EQ(v.params.subsurface, 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(v.params.subsurface_radius[2], 0.25f, 1e-6f);
    EXPECT_FLOAT_EQ(v.params.diffuse_transmission_color[1], 0.8f, 1e-6f);
    EXPECT_FLOAT_EQ(v.params.alpha_cutoff, 0.3f, 1e-6f);
    /* Not in the file: the glTF defaults. */
    const de_cooked_material_params d = de_cooked_material_params_default();
    EXPECT_FLOAT_EQ(v.params.ior, d.ior, 1e-6f);
    EXPECT_FLOAT_EQ(v.params.specular, d.specular, 1e-6f);
    EXPECT_TRUE(isinf(v.params.attenuation_distance));
    EXPECT_EQ(v.alpha_mode, (u32)DE_COOKED_ALPHA_MASK);
    EXPECT_EQ(v.flags, (u32)(DE_COOKED_MAT_DOUBLE_SIDED | DE_COOKED_MAT_CLEARCOAT |
                             DE_COOKED_MAT_SHEEN | DE_COOKED_MAT_DIFFUSE_TRANSMISSION));
    ASSERT_EQ(v.texture_count, 3u);
    EXPECT_EQ(v.textures[0].slot, (u32)DE_COOKED_TEX_BASE_COLOR); /* sorted by slot */
    EXPECT_EQ(v.textures[0].asset_id, de_asset_id_from("env/stone_albedo").hash);
    EXPECT_EQ(v.textures[0].texcoord, 0u);
    EXPECT_EQ(v.textures[1].slot, (u32)DE_COOKED_TEX_NORMAL);
    EXPECT_EQ(v.textures[1].texcoord, 1u);
    EXPECT_EQ(v.textures[2].slot, (u32)DE_COOKED_TEX_OCCLUSION);
    EXPECT_EQ(r.dependency_count, 3u);
    EXPECT_TRUE(has_dep(&r, "env/stone_albedo"));
    EXPECT_TRUE(has_dep(&r, "env/stone_normal"));
    EXPECT_TRUE(has_dep(&r, "env/stone_ao"));
    de_cook_result_free(&r);

    /* A game model by name (resolved at run time), and Unlit by its flag. */
    ASSERT_EQ(cook_text(f, "toon.mat.toml", "model = \"toon\"\nbase_color = [1, 0, 0, 0.5]\n", &r),
              DE_OK);
    ASSERT_TRUE(de_cooked_material_parse(r.data, r.size, &v, &why));
    EXPECT_EQ(v.model_hash, de_cooked_material_model_hash("toon"));
    EXPECT_NE(v.model_hash, 0u);
    EXPECT_FLOAT_EQ(v.params.base_color[3], 0.5f, 1e-6f);
    EXPECT_EQ(r.dependency_count, 0u);
    de_cook_result_free(&r);
    ASSERT_EQ(cook_text(f, "glow.mat.toml", "model = \"unlit\"\nemissive = [1, 1, 1]\n", &r),
              DE_OK);
    ASSERT_TRUE(de_cooked_material_parse(r.data, r.size, &v, &why));
    EXPECT_EQ(v.model_hash, 0u);
    EXPECT_EQ(v.flags & (u32)DE_COOKED_MAT_UNLIT, (u32)DE_COOKED_MAT_UNLIT);
    de_cook_result_free(&r);
    /* An empty file is the default material. */
    ASSERT_EQ(cook_text(f, "empty.mat.toml", "", &r), DE_OK);
    ASSERT_TRUE(de_cooked_material_parse(r.data, r.size, &v, &why));
    EXPECT_FLOAT_EQ(v.params.roughness, d.roughness, 1e-6f);
    EXPECT_EQ(v.texture_count, 0u);
    de_cook_result_free(&r);
}

/* ---- mat_toml_unknown_keys ------------------------------------------------------------------- */

TEST_F(import, mat_toml_unknown_keys, mt_fx) {
    de_cook_result r;
    EXPECT_EQ(cook_text(f, "typo.mat.toml",
                        "roughness = 0.5\nrougness = 0.4\nbase_colour = [1, 1, 1]\nmetallic = 1\n",
                        &r),
              DE_ERR_PARSE);
    EXPECT_TRUE(error_says("unknown keys"));
    EXPECT_TRUE(error_says("`rougness` (line 2)"));
    EXPECT_TRUE(error_says("`base_colour` (line 3)"));
    EXPECT_EQ(cook_text(f, "table.mat.toml", "[textures]\nbase = \"a/b\"\n", &r), DE_ERR_PARSE);
    EXPECT_TRUE(error_says("unknown key `textures`"));
    /* Wrong types and values. */
    EXPECT_EQ(cook_text(f, "type1.mat.toml", "roughness = \"high\"\n", &r), DE_ERR_PARSE);
    EXPECT_TRUE(error_says("`roughness` must be a number"));
    EXPECT_EQ(cook_text(f, "type2.mat.toml", "emissive = [1, 1]\n", &r), DE_ERR_PARSE);
    EXPECT_TRUE(error_says("an array of 3 numbers"));
    EXPECT_EQ(cook_text(f, "type3.mat.toml", "alpha_mode = \"cutout\"\n", &r), DE_ERR_PARSE);
    EXPECT_TRUE(error_says("\"opaque\", \"mask\" or \"blend\""));
    EXPECT_EQ(cook_text(f, "type4.mat.toml", "normal_texcoord = 2\n", &r), DE_ERR_PARSE);
    EXPECT_EQ(cook_text(f, "type5.mat.toml", "base_color_tex = \"/abs/path\"\n", &r), DE_ERR_PARSE);
    EXPECT_TRUE(error_says("not a valid asset ID"));
    EXPECT_EQ(cook_text(f, "conflict.mat.toml", "model = \"toon\"\nunlit = true\n", &r),
              DE_ERR_PARSE);
    EXPECT_EQ(cook_text(f, "syntax.mat.toml", "roughness = = 1\n", &r), DE_ERR_PARSE);
}

/* ---- mat_toml_deterministic ------------------------------------------------------------------ */

TEST_F(import, mat_toml_deterministic, mt_fx) {
    static const char a[] = "roughness = 0.5\nbase_color = [0.1, 0.2, 0.3, 1.0]\n"
                            "normal_tex = \"env/n\"\nbase_color_tex = \"env/c\"\n"
                            "alpha_mode = \"blend\"\nmodel = \"toon\"\n";
    static const char b[] = "# the same, reordered\n"
                            "model    = 'toon'\n"
                            "base_color_tex = \"env/c\"\n"
                            "alpha_mode = 'blend'\n"
                            "base_color = [ 0.1, 0.2, 0.3, 1 ]   # alpha as an integer\n"
                            "normal_tex = \"env/n\"\n"
                            "roughness = 5e-1\n";
    de_cook_result ra, rb, rc;
    ASSERT_EQ(cook_text(f, "det_a.mat.toml", a, &ra), DE_OK);
    ASSERT_EQ(cook_text(f, "det_b.mat.toml", b, &rb), DE_OK);
    ASSERT_EQ(cook_text(f, "det_a.mat.toml", a, &rc), DE_OK);
    ASSERT_EQ(ra.size, rb.size);
    EXPECT_EQ(memcmp(ra.data, rb.data, (size_t)ra.size), 0);
    EXPECT_EQ(ra.size, rc.size);
    EXPECT_EQ(memcmp(ra.data, rc.data, (size_t)ra.size), 0);
    EXPECT_EQ(ra.output_hash, rc.output_hash);
    de_cook_result_free(&ra);
    de_cook_result_free(&rb);
    de_cook_result_free(&rc);
}
