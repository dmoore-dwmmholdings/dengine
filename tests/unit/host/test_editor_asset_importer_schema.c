/* DENG-578 (DIDE-R156): asset.importerSchema serves one JSON Schema per importer, and its keys,
 * defaults and bounds are the ones the cook importers actually read. A form built from a key the
 * importer ignores (the old sound `streaming`/`quality`) would write settings that do nothing. */
#include "de_test.h"
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include "cook/importers/model/anim_cook.h"
#include "cook/importers/model/model_import.h"
#include "cook/importers/texture/texture_import.h"
#include "cook/importers/texture/texture_internal.h"
#include "render/formats/de_cooked_texture.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct schema_fx {
    de_engine engine;
    de__host host;
    de__editor editor;
    char response[16384];
    size_t response_size;
    mpack_tree_t tree;
    u64 arena[4096];
} schema_fx;

static void schema_fx_setup(schema_fx *f) {
    memset(f, 0, sizeof *f);
    f->editor.e = &f->engine;
    f->editor.h = &f->host;
    de__ed_assets_register(&f->editor);
}
static void schema_fx_teardown(schema_fx *f) { (void)f; }

/* Calls the registered handler and returns the response root (valid until the next call). */
static mpack_node_t call_schema(schema_fx *f, const char *type) {
    char params[128];
    mpack_writer_t writer;
    mpack_writer_init(&writer, params, sizeof params);
    mpack_start_map(&writer, 1);
    mpack_write_cstr(&writer, "type");
    mpack_write_cstr(&writer, type);
    mpack_finish_map(&writer);
    size_t params_size = mpack_writer_buffer_used(&writer);
    mpack_writer_destroy(&writer);
    de_ep_request request = {.id = 9, .params = {params, params_size}};
    de_ep_arena arena = {(u8 *)f->arena, sizeof f->arena, 0};
    mpack_writer_init(&writer, f->response, sizeof f->response);
    f->editor.handlers[DE_EP_METHOD_ASSET_IMPORTER_SCHEMA](&f->editor, &request, &arena, &writer);
    f->response_size = mpack_writer_buffer_used(&writer);
    mpack_writer_destroy(&writer);
    mpack_tree_init_data(&f->tree, f->response, f->response_size);
    mpack_tree_parse(&f->tree);
    return mpack_tree_root(&f->tree);
}

static mpack_node_t properties(schema_fx *f, const char *type) {
    mpack_node_t root = call_schema(f, type);
    mpack_node_t schema = mpack_node_map_cstr(mpack_node_map_cstr(root, "result"), "schema");
    char kind[16] = {0};
    mpack_node_copy_cstr(mpack_node_map_cstr(schema, "type"), kind, sizeof kind);
    EXPECT_STREQ(kind, "object");
    return mpack_node_map_cstr(schema, "properties");
}

/* Every property is well formed; the set of keys is exactly `keys`. */
static void expect_keys(mpack_node_t props, const char *const *keys, u32 count) {
    ASSERT_EQ((u32)mpack_node_map_count(props), count);
    for (u32 i = 0; i < count; ++i) {
        mpack_node_t p = mpack_node_map_cstr_optional(props, keys[i]);
        if (mpack_node_is_missing(p)) {
            fprintf(stderr, "importer key missing from schema: %s\n", keys[i]);
            EXPECT_TRUE(false);
            continue;
        }
        char type[16] = {0};
        mpack_node_copy_cstr(mpack_node_map_cstr(p, "type"), type, sizeof type);
        EXPECT_TRUE(!strcmp(type, "string") || !strcmp(type, "boolean") ||
                    !strcmp(type, "number") || !strcmp(type, "integer"));
        EXPECT_TRUE(mpack_node_map_contains_cstr(p, "description"));
        mpack_node_t def = mpack_node_map_cstr_optional(p, "default");
        mpack_node_t values = mpack_node_map_cstr_optional(p, "enum");
        if (!mpack_node_is_missing(values) && !mpack_node_is_missing(def)) {
            bool found = false;
            for (size_t v = 0; v < mpack_node_array_length(values); ++v)
                found |= mpack_node_strlen(mpack_node_array_at(values, v)) ==
                             mpack_node_strlen(def) &&
                         !memcmp(mpack_node_str(mpack_node_array_at(values, v)),
                                 mpack_node_str(def), mpack_node_strlen(def));
            EXPECT_TRUE(found);
        }
        mpack_node_t lo = mpack_node_map_cstr_optional(p, "minimum");
        if (!mpack_node_is_missing(lo) && !mpack_node_is_missing(def)) {
            f64 d = mpack_node_double(def);
            EXPECT_TRUE(d >= mpack_node_double(lo));
            EXPECT_TRUE(d <= mpack_node_double(mpack_node_map_cstr(p, "maximum")));
        }
    }
}

static f64 num(mpack_node_t props, const char *key, const char *field) {
    return mpack_node_double(mpack_node_map_cstr(mpack_node_map_cstr(props, key), field));
}

TEST_F(editor_importer_schema, texture_matches_importer, schema_fx) {
    /* texture_import.c read_settings, the rt_proxy region and max_decoded_mib. */
    static const char *const keys[] = {
        "usage",       "srgb",           "mips",           "pixel_art",       "alpha_coverage",
        "max_size",    "address",        "compression",    "quality",         "rdo_quality",
        "rt_proxy",    "rt_proxy_u",     "rt_proxy_v",     "rt_proxy_width",  "rt_proxy_height",
        "rt_proxy_cutoff", "max_decoded_mib"};
    mpack_node_t props = properties(f, "texture");
    expect_keys(props, keys, (u32)(sizeof keys / sizeof keys[0]));
    EXPECT_TRUE(num(props, "rdo_quality", "maximum") == (f64)DE_TEXTURE_RDO_QUALITY_MAX);
    EXPECT_TRUE(num(props, "max_size", "maximum") == (f64)DE_COOKED_TEXTURE_MAX_SIZE);
    EXPECT_TRUE(num(props, "max_decoded_mib", "default") == (f64)DE_TEX_MAX_DECODED_MIB_DEFAULT);
    EXPECT_TRUE(num(props, "max_decoded_mib", "maximum") == (f64)DE_TEX_MAX_DECODED_MIB_MAX);
    ASSERT_EQ(mpack_tree_destroy(&f->tree), mpack_ok);
}

TEST_F(editor_importer_schema, model_matches_importer, schema_fx) {
    /* model_cook.c de_model_import and anim_skeleton.c de_model_anim_settings_read. */
    static const char *const keys[] = {
        "import_meshes",    "import_materials", "import_skeletons", "import_animations",
        "import_prefab",    "scale",            "import_cameras",   "generate_lods",
        "lod_error",        "meshlets",         "max_position_error", "axis_up",
        "axis_front",       "anim_sample_rate", "anim_error",       "anim_error_angle",
        "anim_joint_names", "max_decoded_mib"};
    mpack_node_t props = properties(f, "model");
    expect_keys(props, keys, (u32)(sizeof keys / sizeof keys[0]));
    EXPECT_TRUE(num(props, "anim_sample_rate", "default") == (f64)DE_MODEL_ANIM_DEFAULT_RATE);
    EXPECT_TRUE(num(props, "anim_error", "default") == (f64)DE_MODEL_ANIM_DEFAULT_ERROR);
    EXPECT_TRUE(num(props, "anim_error_angle", "default") ==
                (f64)DE_MODEL_ANIM_DEFAULT_ERROR_ANGLE);
    EXPECT_TRUE(num(props, "max_decoded_mib", "default") == (f64)DE_MODEL_MAX_DECODED_MIB_DEFAULT);
    EXPECT_TRUE(num(props, "max_decoded_mib", "maximum") == (f64)DE_MODEL_MAX_DECODED_MIB_MAX);
    ASSERT_EQ(mpack_tree_destroy(&f->tree), mpack_ok);
}

TEST_F(editor_importer_schema, sound_matches_importer, schema_fx) {
    /* sound_import.c: the keys de__sound_settings is read from. */
    static const char *const keys[] = {"stream", "stream_above_seconds", "compression", "codec",
                                       "bitrate", "mono", "loop", "loop_start", "loop_end"};
    mpack_node_t props = properties(f, "sound");
    expect_keys(props, keys, (u32)(sizeof keys / sizeof keys[0]));
    /* Absent `stream`, `codec` and `loop` mean "decide from the source"; a default would
     * hide it. */
    EXPECT_FALSE(mpack_node_map_contains_cstr(mpack_node_map_cstr(props, "stream"), "default"));
    EXPECT_FALSE(mpack_node_map_contains_cstr(mpack_node_map_cstr(props, "codec"), "default"));
    EXPECT_FALSE(mpack_node_map_contains_cstr(mpack_node_map_cstr(props, "loop"), "default"));
    ASSERT_EQ(mpack_tree_destroy(&f->tree), mpack_ok);
}

TEST_F(editor_importer_schema, unknown_type_is_not_found, schema_fx) {
    mpack_node_t root = call_schema(f, "no_such_type");
    mpack_node_t error = mpack_node_map_cstr(root, "error");
    EXPECT_EQ(mpack_node_i64(mpack_node_map_cstr(error, "code")), (i64)DE_EP_ERR_NOT_FOUND);
    EXPECT_FALSE(mpack_node_map_contains_cstr(root, "result"));
    ASSERT_EQ(mpack_tree_destroy(&f->tree), mpack_ok);
}
