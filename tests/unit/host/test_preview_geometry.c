#include "de_test.h"
#include "render/loaders/preview_geometry.h"
#include "render/formats/de_cooked_mesh.h"
#include <dengine/de_math.h>
#include <math.h>
#include <string.h>

static void check_shape(de__render_preview_shape shape, u32 triangles, u32 max_vertices) {
    de_tracking_allocator track;
    const de_allocator allocator = de_tracking_allocator_init(&track, NULL, DE_MEM_RENDER);
    de__render_preview_blob blob;
    ASSERT_EQ(de__render_preview_geometry(&allocator, shape, &blob), DE_OK);
    EXPECT_LT(de_tracking_allocator_stats(&track).bytes_peak, 5u << 20);
    de_cooked_mesh_view mesh;
    const char *error = NULL;
    ASSERT_TRUE(de_cooked_mesh_parse(blob.data, blob.size, &mesh, &error));
    EXPECT_EQ(mesh.lod_count, 1u);
    EXPECT_EQ(mesh.submesh_count, 1u);
    EXPECT_EQ(mesh.index_count, triangles * 3);
    EXPECT_LE(mesh.vertex_count, max_vertices);
    EXPECT_GT(mesh.meshlet_count, 0u);
    EXPECT_LT(blob.size, 256u << 10);
    for (u32 i = 0; i < mesh.vertex_count; ++i) {
        f32 p[3], n[3], t[4], uv[2];
        de_cooked_mesh_position(&mesh, i, p);
        ASSERT_TRUE(de_cooked_mesh_normal(&mesh, i, n));
        ASSERT_TRUE(de_cooked_mesh_tangent(&mesh, i, t));
        ASSERT_TRUE(de_cooked_mesh_uv(&mesh, 0, i, uv));
        EXPECT_TRUE(fabsf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] - 1) < .001f);
        EXPECT_TRUE(fabsf(n[0] * t[0] + n[1] * t[1] + n[2] * t[2]) < .001f);
        EXPECT_TRUE(t[3] == (shape == DE__RENDER_PREVIEW_SPHERE ? -1.f : 1.f));
        EXPECT_TRUE(uv[0] >= 0 && uv[0] <= 1.00001f && uv[1] >= 0 && uv[1] <= 1.00001f);
        if (shape == DE__RENDER_PREVIEW_SPHERE) {
            EXPECT_TRUE(fabsf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2] - 1) < .001f);
            EXPECT_TRUE(p[0] * n[0] + p[1] * n[1] + p[2] * n[2] > .999f);
        }
    }
    for (u32 i = 0; i < mesh.index_count; i += 3) {
        de_vec3 p[3];
        f32 normal[3];
        for (u32 k = 0; k < 3; ++k) {
            u32 vertex = de_cooked_mesh_index(&mesh, i + k);
            ASSERT_LT(vertex, mesh.vertex_count);
            de_cooked_mesh_position(&mesh, vertex, (f32 *)&p[k]);
        }
        de_cooked_mesh_normal(&mesh, de_cooked_mesh_index(&mesh, i), normal);
        de_vec3 cross = de_v3_cross(de_v3_sub(p[1], p[0]), de_v3_sub(p[2], p[0]));
        EXPECT_TRUE(cross.x * normal[0] + cross.y * normal[1] + cross.z * normal[2] > 1e-6f);
    }
    de__render_preview_blob repeat;
    ASSERT_EQ(de__render_preview_geometry(&allocator, shape, &repeat), DE_OK);
    ASSERT_EQ(repeat.size, blob.size);
    EXPECT_EQ(memcmp(repeat.data, blob.data, (size_t)blob.size), 0);
    DE_FREE(&allocator, repeat.data, (size_t)repeat.size);
    DE_FREE(&allocator, blob.data, (size_t)blob.size);
    EXPECT_EQ(de_tracking_allocator_stats(&track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&track).blocks_live, 0u);
}
TEST(preview_geometry, sphere_is_bounded_real_cooked_mesh_with_consistent_tangent_frame) {
    check_shape(DE__RENDER_PREVIEW_SPHERE, 960, 561);
}
TEST(preview_geometry, box_preserves_hard_faces_and_uvs) {
    check_shape(DE__RENDER_PREVIEW_BOX, 12, 24);
}
TEST(preview_geometry, plane_faces_positive_z_with_uvs) {
    check_shape(DE__RENDER_PREVIEW_PLANE, 2, 4);
}
TEST(preview_geometry, rejects_unknown_shape_preserving_output) {
    de__render_preview_blob out = {.size = 123};
    EXPECT_EQ(de__render_preview_geometry(NULL, (de__render_preview_shape)-1, &out),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(out.size, 123u);
    EXPECT_EQ(de__render_preview_geometry(NULL, DE__RENDER_PREVIEW_SHAPES, &out),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__render_preview_geometry(NULL, DE__RENDER_PREVIEW_SPHERE, NULL),
              DE_ERR_INVALID_ARGUMENT);
}
