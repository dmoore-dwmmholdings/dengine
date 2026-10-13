/* Smoke test for de_tp_cgltf: parses a one-triangle glTF from memory (buffer as a data URI),
 * loads the buffer, validates, and unpacks the positions. The importer tests are in
 * tests/unit/import/. */
#include <string.h>

#include <cgltf.h>

#include "de_test.h"

/* Three float3 positions: (0,0,0) (1,0,0) (0,1,0), base64. */
static const char k_gltf[] =
    "{\"asset\":{\"version\":\"2.0\"},"
    "\"buffers\":[{\"byteLength\":36,\"uri\":\"data:application/octet-stream;base64,"
    "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA\"}],"
    "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
    "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
    "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
    "\"nodes\":[{\"mesh\":0,\"name\":\"tri\"}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";

TEST(de_tp_smoke_cgltf, parse_triangle) {
    cgltf_options opt;
    memset(&opt, 0, sizeof opt);
    cgltf_data *data = NULL;
    ASSERT_EQ((int)cgltf_parse(&opt, k_gltf, sizeof k_gltf - 1u, &data), (int)cgltf_result_success);
    ASSERT_EQ((int)cgltf_load_buffers(&opt, data, NULL), (int)cgltf_result_success);
    EXPECT_EQ((int)cgltf_validate(data), (int)cgltf_result_success);
    ASSERT_EQ(data->meshes_count, (cgltf_size)1);
    EXPECT_STREQ(data->nodes[0].name, "tri");
    const cgltf_accessor *acc = data->meshes[0].primitives[0].attributes[0].data;
    float pos[9];
    EXPECT_EQ(cgltf_accessor_unpack_floats(acc, pos, 9), (cgltf_size)9);
    EXPECT_FLOAT_EQ(pos[3], 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(pos[7], 1.0f, 0.0f);
    cgltf_free(data);
}

TEST(de_tp_smoke_cgltf, rejects_garbage) {
    cgltf_options opt;
    memset(&opt, 0, sizeof opt);
    cgltf_data *data = NULL;
    static const char bad[] = "{\"asset\":";
    EXPECT_NE((int)cgltf_parse(&opt, bad, sizeof bad - 1u, &data), (int)cgltf_result_success);
    EXPECT_TRUE(data == NULL);
}
