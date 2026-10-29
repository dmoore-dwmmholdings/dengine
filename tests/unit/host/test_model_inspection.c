#include "de_test.h"
#include "host/editor/editor_model_inspection.h"
#include "render/formats/de_cooked_model.h"
#include <string.h>

static const char *handle_name(void *user, u32 handle) {
    (void)user;
    return handle == 1 ? "model#Body" : handle == 2 ? "model#Lid" : NULL;
}
static const char *asset_name(void *user, de_asset_id id) {
    (void)user;
    return id.hash == 77 ? "model#material:Wood" : NULL;
}
static const de__ed_model_names names = {handle_name, asset_name, NULL};
static de_mesh handles[] = {{1}, {2}};
static de_render_model_node nodes[] = {{.name = "Root", .parent = DE_COOKED_NONE},
                                       {.name = "Body", .parent = 0, .mesh = {1}},
                                       {.name = "Lid", .parent = 0, .mesh = {2}}};
static const de_render_model model = {
    .node_count = 3, .mesh_count = 2, .nodes = nodes, .meshes = handles};
static de_render_submesh submeshes[] = {
    {.material = {77}, .bounds_min = {-2, -1, 0}, .bounds_max = {2, 1, 3}},
    {.bounds_min = {-1, -1, 0}, .bounds_max = {1, 1, 1}}};
static de_cooked_mesh_range ranges[] = {{0, 6, 0, 1}, {6, 6, 1, 1}, {12, 3, 2, 1}, {15, 3, 3, 0}};
static const de_render_mesh mesh = {
    .vertex_count = 6,
    .submesh_count = 2,
    .lod_count = 2,
    .bounds_min = {-2, -1, 0},
    .bounds_max = {2, 1, 3},
    .position_offset = {-2, -1, 0},
    .position_scale = {4, 2, 3},
    .max_position_error = .0025f,
    .lod_error = .25f,
    .stream_format = {[DE_COOKED_STREAM_POSITION] = DE_COOKED_VF_UNORM16X4,
                      [DE_COOKED_STREAM_NORMAL] = DE_COOKED_VF_OCT16,
                      [DE_COOKED_STREAM_UV1] = DE_COOKED_VF_UNORM16X2},
    .lods = {{0, 12, 0, 2, 4, 6, 0}, {12, 6, 2, 1, 2, 4, .0375f}},
    .submeshes = submeshes,
    .lod_submeshes = ranges};

TEST(model_inspection, actual_selected_mesh_lods_formats_sparse_uv_and_assignments) {
    u64 storage[2048];
    de_ep_arena arena = {(u8 *)storage, sizeof storage, 0};
    de_ep_model_inspection info;
    ASSERT_EQ(de__ed_model_inspect(&model, &mesh, 1, &names, &arena, &info), DE_OK);
    EXPECT_TRUE(info.has_mesh);
    EXPECT_EQ(info.mesh, 1u);
    EXPECT_EQ(info.meshes.count, 2u);
    EXPECT_TRUE(de_ep_str_eq(info.meshes.items[1], "model#Lid"));
    EXPECT_TRUE(de_ep_str_eq(info.nodes.items[1].mesh, "model#Body"));
    EXPECT_EQ(info.nodes.items[1].parent, 0u);
    EXPECT_EQ(info.lods.count, 2u);
    EXPECT_EQ(info.lods.items[0].triangles, 4u);
    EXPECT_EQ(info.lods.items[1].triangles, 2u);
    EXPECT_EQ(info.lods.items[1].meshlets, 1u);
    EXPECT_EQ(info.lods.items[1].vertices, 4u);
    EXPECT_FLOAT_EQ(info.lods.items[0].error_metres, 0, .00001);
    EXPECT_FLOAT_EQ(info.lods.items[1].error_metres, .0375, .00001);
    EXPECT_FLOAT_EQ(info.max_position_error, .0025, .00001);
    EXPECT_EQ(info.position_scale.count, 3u);
    EXPECT_FLOAT_EQ(info.position_scale.items[0], 4, .00001);
    EXPECT_EQ(info.uv_channels.count, 1u);
    EXPECT_EQ(info.uv_channels.items[0], 1u);
    EXPECT_TRUE(de_ep_str_eq(info.attributes.items[0].format, "unorm16x4"));
    EXPECT_TRUE(de_ep_str_eq(info.attributes.items[1].format, "oct16"));
    EXPECT_TRUE(de_ep_str_eq(info.attributes.items[2].name, "uv1"));
    EXPECT_TRUE(de_ep_str_eq(info.submeshes.items[0].material, "model#material:Wood"));
    EXPECT_TRUE(de_ep_str_eq(info.submeshes.items[1].material, ""));
    EXPECT_EQ(info.submeshes.items[1].lods.items[1].triangles, 1u);
    EXPECT_EQ(info.submeshes.items[1].lods.items[1].index_first, 15u);
    EXPECT_EQ(info.submeshes.items[1].lods.items[1].index_count, 3u);
    EXPECT_EQ(info.submeshes.items[1].lods.items[1].meshlet_first, 3u);
    EXPECT_EQ(info.submeshes.items[1].lods.items[1].meshlets, 0u);
}
TEST(model_inspection, invalid_selection_limits_and_short_arena_leave_output_untouched) {
    u64 storage[64];
    memset(storage, 0xc5, sizeof storage);
    de_ep_arena arena = {(u8 *)storage, 16, 0};
    de_ep_model_inspection sentinel;
    memset(&sentinel, 0xa3, sizeof sentinel);
    de_ep_model_inspection out = sentinel;
    EXPECT_EQ(de__ed_model_inspect(&model, &mesh, 2, &names, &arena, &out),
              DE_ERR_INVALID_ARGUMENT);
    ASSERT_MEMEQ(&sentinel, &out, sizeof out);
    EXPECT_EQ(de__ed_model_inspect(&model, &mesh, 0, &names, &arena, &out), DE_ERR_OUT_OF_MEMORY);
    ASSERT_MEMEQ(&sentinel, &out, sizeof out);
    for (size_t i = 16; i < sizeof storage; ++i)
        EXPECT_EQ(((u8 *)storage)[i], 0xc5u);
    de_render_model too_big = model;
    too_big.node_count = 8193;
    EXPECT_EQ(de__ed_model_inspect(&too_big, &mesh, 0, &names, &arena, &out), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(de__ed_model_inspect(&model, NULL, 0, &names, &arena, &out), DE_ERR_NOT_FOUND);
    ASSERT_MEMEQ(&sentinel, &out, sizeof out);
}
TEST(model_inspection, rejects_inconsistent_cooked_ranges_and_formats) {
    u64 storage[2048];
    de_ep_arena arena = {(u8 *)storage, sizeof storage, 0};
    de_ep_model_inspection sentinel;
    memset(&sentinel, 0xa3, sizeof sentinel);
    de_ep_model_inspection out = sentinel;
    de_render_mesh bad = mesh;
    bad.lods[0].index_count = 11;
    EXPECT_EQ(de__ed_model_inspect(&model, &bad, 0, &names, &arena, &out), DE_ERR_PARSE);
    ASSERT_MEMEQ(&sentinel, &out, sizeof out);
    arena.used = 0;
    bad = mesh;
    bad.stream_format[DE_COOKED_STREAM_NORMAL] = DE_COOKED_VF_F32X4;
    EXPECT_EQ(de__ed_model_inspect(&model, &bad, 0, &names, &arena, &out), DE_ERR_PARSE);
    ASSERT_MEMEQ(&sentinel, &out, sizeof out);
    arena.used = 0;
    de_cooked_mesh_range invalid[4];
    memcpy(invalid, ranges, sizeof invalid);
    invalid[0].index_count = 18;
    bad = mesh;
    bad.lod_submeshes = invalid;
    EXPECT_EQ(de__ed_model_inspect(&model, &bad, 0, &names, &arena, &out), DE_ERR_PARSE);
    ASSERT_MEMEQ(&sentinel, &out, sizeof out);
}
TEST(model_inspection, node_only_models_have_no_selected_mesh) {
    de_render_model empty = {0};
    de_ep_arena arena = {0};
    de_ep_model_inspection out;
    ASSERT_EQ(de__ed_model_inspect(&empty, NULL, 0, &names, &arena, &out), DE_OK);
    EXPECT_FALSE(out.has_mesh);
    EXPECT_EQ(out.lods.count, 0u);
    EXPECT_EQ(de__ed_model_inspect(&empty, NULL, 1, &names, &arena, &out), DE_ERR_INVALID_ARGUMENT);
}
