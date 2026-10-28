#include "de_test.h"
#include <dengine/de_reflect.h>
#include "../core/test_counting_alloc.h"
#include <string.h>

#if defined(DE_SHIPPING)
TEST(debugger, shipping_excludes_generation) {
    EXPECT_EQ(de_debug_visualizers_write(NULL, NULL, NULL), DE_ERR_UNSUPPORTED);
}
#else
static void emit(de_type_registry *r, de_mem_writer *xml, de_mem_writer *py) {
    de_mem_writer_init(xml, NULL);
    de_mem_writer_init(py, NULL);
    de_writer x = de_writer_from_mem(xml), p = de_writer_from_mem(py);
    ASSERT_EQ(de_debug_visualizers_write(r, &x, &p), DE_OK);
    ASSERT_EQ(x.write(x.user, "", 1), DE_OK);
    ASSERT_EQ(p.write(p.user, "", 1), DE_OK);
}

TEST(debugger, deterministic_current_registry) {
    de_type_registry *r = NULL;
    ASSERT_EQ(de_type_registry_create(NULL, &r), DE_OK);
    typedef struct Zebra {
        f32 speed;
    } Zebra;
    const de_field_info fields[] = {DE_FIELD(Zebra, speed, DE_FK_F32, DE_FIELD_DEFAULT)};
    const de_type_desc z = DE_TYPE_DESC(Zebra, 1, fields);
    ASSERT_EQ(de_type_register_ex(r, &z, NULL, NULL), DE_OK);
    de_type_desc a = z;
    a.name = "Alpha";
    ASSERT_EQ(de_type_register_ex(r, &a, NULL, NULL), DE_OK);
    de_mem_writer xml, py;
    const u64 revision = de_type_registry_revision(r);
    emit(r, &xml, &py);
    EXPECT_TRUE(strstr((char *)xml.data, "Name=\"Alpha\"") <
                strstr((char *)xml.data, "Name=\"Zebra\""));
    EXPECT_TRUE(strstr((char *)py.data, "'Alpha': (4, ['speed'])") != NULL);
    EXPECT_EQ(de_type_registry_revision(r), revision);
    de_mem_writer xml2, py2;
    emit(r, &xml2, &py2);
    EXPECT_EQ(xml.size, xml2.size);
    EXPECT_EQ(py.size, py2.size);
    EXPECT_TRUE(memcmp(xml.data, xml2.data, xml.size) == 0);
    EXPECT_TRUE(memcmp(py.data, py2.data, py.size) == 0);
    /* A fresh output reflects hot-reloaded names, not stale descriptions. */
    de_field_info changed = fields[0];
    changed.name = "velocity";
    a.fields = &changed;
    ASSERT_EQ(de_type_register_ex(r, &a, NULL, NULL), DE_OK);
    de_mem_writer_free(&xml2);
    de_mem_writer_free(&py2);
    emit(r, &xml2, &py2);
    EXPECT_TRUE(strstr((char *)py2.data, "'Alpha': (4, ['velocity'])") != NULL);
    de_mem_writer_free(&xml);
    de_mem_writer_free(&py);
    de_mem_writer_free(&xml2);
    de_mem_writer_free(&py2);
    de_type_registry_destroy(r);
}

TEST(debugger, allocation_and_writer_failures) {
    test_counting_alloc c;
    test_counting_init(&c);
    de_type_registry *r = NULL;
    ASSERT_EQ(de_type_registry_create(&c.iface, &r), DE_OK);
    de_type_desc d = {.name = "Tag", .size = 1, .align = 1};
    ASSERT_EQ(de_type_register_ex(r, &d, NULL, NULL), DE_OK);
    u8 tiny[4];
    de_mem_writer mw;
    de_mem_writer_init_fixed(&mw, tiny, sizeof tiny);
    de_writer w = de_writer_from_mem(&mw);
    c.fail_after = 0;
    EXPECT_EQ(de_debug_visualizers_write(r, &w, NULL), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(mw.size, 0u);
    c.fail_after = -1;
    EXPECT_EQ(de_debug_visualizers_write(r, NULL, &w), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_debug_visualizers_write(r, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_debug_visualizers_write(NULL, &w, NULL), DE_ERR_INVALID_ARGUMENT);
    de_type_registry_destroy(r);
    EXPECT_EQ(c.live_blocks, 0u);
    EXPECT_EQ(c.size_errors, 0u);
}
#endif
