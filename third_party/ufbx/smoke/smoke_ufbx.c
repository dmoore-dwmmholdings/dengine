/* Smoke test for de_tp_ufbx: loads a one-triangle ASCII FBX from memory, triangulates its face,
 * and rejects garbage. The importer tests are in tests/unit/import/test_fbx_import.c. */
#include <string.h>

#include <ufbx.h>

#include "de_test.h"

static const char k_fbx[] = "; FBX 7.4.0 project file\n"
                            "FBXHeaderExtension:  {\n"
                            "\tFBXHeaderVersion: 1003\n"
                            "\tFBXVersion: 7400\n"
                            "}\n"
                            "Objects:  {\n"
                            "\tGeometry: 1, \"Geometry::tri\", \"Mesh\" {\n"
                            "\t\tVertices: *9 {\n"
                            "\t\t\ta: 0,0,0,1,0,0,0,1,0\n"
                            "\t\t}\n"
                            "\t\tPolygonVertexIndex: *3 {\n"
                            "\t\t\ta: 0,1,-3\n"
                            "\t\t}\n"
                            "\t}\n"
                            "\tModel: 2, \"Model::tri\", \"Mesh\" {\n"
                            "\t}\n"
                            "}\n"
                            "Connections:  {\n"
                            "\tC: \"OO\",1,2\n"
                            "\tC: \"OO\",2,0\n"
                            "}\n";

TEST(de_tp_smoke_ufbx, load_triangle) {
    ufbx_load_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.file_format = UFBX_FILE_FORMAT_FBX;
    ufbx_error err;
    ufbx_scene *scene = ufbx_load_memory(k_fbx, sizeof k_fbx - 1u, &opts, &err);
    ASSERT_TRUE(scene != NULL);
    ASSERT_EQ(scene->meshes.count, (size_t)1);
    const ufbx_mesh *mesh = scene->meshes.data[0];
    EXPECT_EQ(mesh->num_faces, (size_t)1);
    EXPECT_EQ(mesh->num_vertices, (size_t)3);
    uint32_t tri[3];
    EXPECT_EQ(ufbx_triangulate_face(tri, 3, mesh, mesh->faces.data[0]), (uint32_t)1);
    ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, tri[1]);
    EXPECT_FLOAT_EQ((float)p.x, 1.0f, 0.0f);
    ASSERT_TRUE(ufbx_find_node(scene, "tri") != NULL);
    ufbx_free_scene(scene);
}

TEST(de_tp_smoke_ufbx, rejects_garbage) {
    ufbx_load_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.file_format = UFBX_FILE_FORMAT_FBX;
    static const char bad[] = "Kaydara FBX Binary  \0\x1a\0\xe8\x1c\0\0\xff\xff\xff\x7f";
    ufbx_error err;
    ufbx_scene *scene = ufbx_load_memory(bad, sizeof bad - 1u, &opts, &err);
    EXPECT_TRUE(scene == NULL);
    EXPECT_NE((int)err.type, (int)UFBX_ERROR_NONE);
}
