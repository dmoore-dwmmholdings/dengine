/* Smoke test for de_tp_tomlc17: parse a manifest-like document and read typed values back. */
#include <string.h>

#include <tomlc17.h>

#include "de_test.h"

TEST(de_tp_smoke_tomlc17, parse_typed_values) {
    static const char doc[] = "# cvar-style document\n"
                              "title = \"DEngine\"\n"
                              "[render]\n"
                              "vsync = true\n"
                              "scale = 0.75\n"
                              "[[asset]]\n"
                              "path = \"textures/a.png\"\n"
                              "size = 1024\n"
                              "[[asset]]\n"
                              "path = \"textures/b.png\"\n"
                              "size = 2048\n";

    toml_result_t r = toml_parse(doc, (int)strlen(doc));
    ASSERT_TRUE(r.ok);
    toml_datum_t title = toml_get(r.toptab, "title");
    EXPECT_EQ(title.type, TOML_STRING);
    EXPECT_STREQ(title.u.s, "DEngine");
    toml_datum_t vsync = toml_seek(r.toptab, "render.vsync");
    EXPECT_EQ(vsync.type, TOML_BOOLEAN);
    EXPECT_TRUE(vsync.u.boolean);
    toml_datum_t scale = toml_seek(r.toptab, "render.scale");
    EXPECT_EQ(scale.type, TOML_FP64);
    EXPECT_FLOAT_EQ(scale.u.fp64, 0.75, 0.0);
    toml_datum_t assets = toml_get(r.toptab, "asset");
    EXPECT_EQ(assets.type, TOML_ARRAY);
    EXPECT_EQ(assets.u.arr.size, 2);
    if (assets.type == TOML_ARRAY && assets.u.arr.size == 2) {
        toml_datum_t size = toml_get(assets.u.arr.elem[1], "size");
        EXPECT_EQ(size.type, TOML_INT64);
        EXPECT_EQ(size.u.int64, 2048);
    }
    toml_free(r);
}

TEST(de_tp_smoke_tomlc17, invalid_document_is_rejected) {
    static const char bad[] = "key = = 1\n";
    toml_result_t r = toml_parse(bad, (int)strlen(bad));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.errmsg[0], '\0');
    toml_free(r);
}
