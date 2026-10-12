/* test_path.c: de_reflect_resolve_path (DENG-294). */
#include "de_test.h"
#include "test_types.h"

#include <stdio.h>
#include <string.h>

typedef struct path_fx {
    de_type_registry *r;
    test_type_ids ids;
} path_fx;

static void path_fx_setup(path_fx *f) {
    ASSERT_EQ(de_type_registry_create(NULL, &f->r), DE_OK);
    ASSERT_EQ(test_types_register(f->r, &f->ids), DE_OK);
}

static void path_fx_teardown(path_fx *f) {
    de_type_registry_destroy(f->r);
}

/* Resolves `path` and checks the offset against `want_offset`; returns the field. */
static const de_field_info *resolve_ok(de_type_registry *r, de_type_id t, const char *path,
                                       size_t want_offset) {
    const de_field_info *field = NULL;
    u32 offset = 0xffffffffu;
    de_result res = de_reflect_resolve_path(r, t, path, &field, &offset);
    if (res != DE_OK) {
        EXPECT_STREQ(de_error_last()->message, path);
        return NULL;
    }
    EXPECT_EQ(offset, (u32)want_offset);
    return field;
}

/* Fails with `want` and a message that quotes `segment`. Outputs are left alone. */
static void resolve_fails(de_type_registry *r, de_type_id t, const char *path, de_result want,
                          const char *segment) {
    const de_field_info *field = (const de_field_info *)&field;
    u32 offset = 77;
    de_error_clear();
    EXPECT_EQ(de_reflect_resolve_path(r, t, path, &field, &offset), want);
    EXPECT_TRUE(field == (const de_field_info *)&field);
    EXPECT_EQ(offset, 77u);
    const de_error *e = de_error_last();
    ASSERT_TRUE(e != NULL);
    char quoted[128];
    snprintf(quoted, sizeof quoted, "'%s'", segment);
    if (!strstr(e->message, quoted))
        EXPECT_STREQ(e->message, quoted);
}

TEST_F(path, nested, path_fx) {
    const de_field_info *fi =
        resolve_ok(f->r, f->ids.entity_data, "transform", offsetof(TEntityData, transform));
    ASSERT_TRUE(fi != NULL);
    EXPECT_EQ(fi->kind, DE_FK_STRUCT);

    fi = resolve_ok(f->r, f->ids.entity_data, "transform.position",
                    offsetof(TEntityData, transform) + offsetof(TTransform, position));
    ASSERT_TRUE(fi != NULL);
    EXPECT_EQ(fi->kind, DE_FK_DVEC3);
    EXPECT_STREQ(fi->name, "position");
    EXPECT_STREQ(fi->attrs.units, "m");

    fi = resolve_ok(f->r, f->ids.entity_data, "inventory.count",
                    offsetof(TEntityData, inventory) + offsetof(TInventory, count));
    ASSERT_TRUE(fi != NULL);
    EXPECT_EQ(fi->kind, DE_FK_U32);

    fi = resolve_ok(f->r, f->ids.entity_data, "mode", offsetof(TEntityData, mode));
    ASSERT_TRUE(fi != NULL);
    EXPECT_EQ(fi->kind, DE_FK_ENUM);

    /* The offset addresses the right bytes of a real object. */
    TEntityData obj;
    memset(&obj, 0, sizeof obj);
    obj.transform.scale.y = 3.5f;
    u32 off = 0;
    ASSERT_EQ(de_reflect_resolve_path(f->r, f->ids.entity_data, "transform.scale.y", &fi, &off),
              DE_OK);
    f32 y;
    memcpy(&y, (const u8 *)&obj + off, sizeof y);
    EXPECT_FLOAT_EQ(y, 3.5, 0.0);
}

TEST_F(path, components, path_fx) {
    const size_t pos = offsetof(TEntityData, transform) + offsetof(TTransform, position);
    const de_field_info *fi = resolve_ok(f->r, f->ids.entity_data, "transform.position.x", pos);
    ASSERT_TRUE(fi != NULL);
    EXPECT_STREQ(fi->name, "x");
    EXPECT_EQ(fi->kind, DE_FK_F64);
    EXPECT_EQ(fi->size, 8u);
    EXPECT_EQ(fi->offset, 0u);
    /* Inherited from the vector field. */
    EXPECT_EQ(fi->flags, (u32)(DE_FIELD_DEFAULT | DE_FIELD_REPLICATE));
    EXPECT_STREQ(fi->attrs.units, "m");

    fi = resolve_ok(f->r, f->ids.entity_data, "transform.position.z", pos + 16);
    ASSERT_TRUE(fi != NULL);
    EXPECT_EQ(fi->offset, 16u);

    const size_t rot = offsetof(TEntityData, transform) + offsetof(TTransform, rotation);
    fi = resolve_ok(f->r, f->ids.entity_data, "transform.rotation.w", rot + 12);
    ASSERT_TRUE(fi != NULL);
    EXPECT_EQ(fi->kind, DE_FK_F32);
    EXPECT_EQ(fi->size, 4u);

    /* Every vector kind in TAllKinds. */
    resolve_ok(f->r, f->ids.all_kinds, "v2.y", offsetof(TAllKinds, v2) + 4);
    resolve_ok(f->r, f->ids.all_kinds, "v3.z", offsetof(TAllKinds, v3) + 8);
    resolve_ok(f->r, f->ids.all_kinds, "v4.w", offsetof(TAllKinds, v4) + 12);
    resolve_ok(f->r, f->ids.all_kinds, "dv3.y", offsetof(TAllKinds, dv3) + 8);
    resolve_ok(f->r, f->ids.all_kinds, "q.x", offsetof(TAllKinds, q));
    fi = resolve_ok(f->r, f->ids.all_kinds, "color.b", offsetof(TAllKinds, color) + 8);
    ASSERT_TRUE(fi != NULL);
    EXPECT_STREQ(fi->name, "b");
    resolve_ok(f->r, f->ids.all_kinds, "color.a", offsetof(TAllKinds, color) + 12);
    resolve_ok(f->r, f->ids.all_kinds, "color.z", offsetof(TAllKinds, color) + 8); /* alias */
    resolve_ok(f->r, f->ids.all_kinds, "vec.y", offsetof(TAllKinds, vec) + 4);     /* a struct */
}

TEST_F(path, array_index, path_fx) {
    const de_field_info *fi =
        resolve_ok(f->r, f->ids.inventory, "items[3].count",
                   offsetof(TInventory, items) + 3 * sizeof(TItem) + offsetof(TItem, count));
    ASSERT_TRUE(fi != NULL);
    EXPECT_STREQ(fi->name, "count");
    EXPECT_EQ(fi->kind, DE_FK_I32);

    fi = resolve_ok(f->r, f->ids.inventory, "items[7]",
                    offsetof(TInventory, items) + 7 * sizeof(TItem));
    ASSERT_TRUE(fi != NULL);
    EXPECT_EQ(fi->kind, DE_FK_STRUCT);
    EXPECT_EQ(fi->size, (u32)sizeof(TItem));
    EXPECT_STREQ(fi->attrs.nested_type, "TItem");

    fi = resolve_ok(f->r, f->ids.inventory, "items", offsetof(TInventory, items));
    ASSERT_TRUE(fi != NULL);
    EXPECT_EQ(fi->kind, DE_FK_FIXED_ARRAY);

    resolve_ok(f->r, f->ids.entity_data, "inventory.items[0].id",
               offsetof(TEntityData, inventory) + offsetof(TInventory, items));
    resolve_ok(f->r, f->ids.all_kinds, "samples[2]", offsetof(TAllKinds, samples) + 8);
    resolve_ok(f->r, f->ids.all_kinds, "items[1].count",
               offsetof(TAllKinds, items) + sizeof(TItem) + offsetof(TItem, count));

    /* The offset addresses the element in a real object. */
    TInventory inv;
    memset(&inv, 0, sizeof inv);
    inv.items[5].count = -42;
    u32 off = 0;
    ASSERT_EQ(de_reflect_resolve_path(f->r, f->ids.inventory, "items[5].count", &fi, &off), DE_OK);
    i32 c;
    memcpy(&c, (const u8 *)&inv + off, sizeof c);
    EXPECT_EQ(c, -42);
}

TEST_F(path, errors, path_fx) {
    de_type_registry *r = f->r;
    const de_type_id ed = f->ids.entity_data, inv = f->ids.inventory;

    /* Unknown names. */
    resolve_fails(r, ed, "transfrom.position.x", DE_ERR_NOT_FOUND, "transfrom");
    resolve_fails(r, ed, "transform.positon.x", DE_ERR_NOT_FOUND, "positon");
    resolve_fails(r, ed, "transform.position.q", DE_ERR_NOT_FOUND, "q");
    resolve_fails(r, ed, "transform.position.xy", DE_ERR_NOT_FOUND, "xy");
    resolve_fails(r, ed, "transform.position.x.y", DE_ERR_NOT_FOUND, "y"); /* f64 has none */
    resolve_fails(r, ed, "mode.x", DE_ERR_NOT_FOUND, "x");
    resolve_fails(r, inv, "items.count", DE_ERR_NOT_FOUND, "count"); /* no index */
    resolve_fails(r, inv, "items[1].nope", DE_ERR_NOT_FOUND, "nope");
    resolve_fails(r, f->ids.all_kinds, "m4.x", DE_ERR_NOT_FOUND, "x");
    resolve_fails(r, f->ids.all_kinds, "samples.x", DE_ERR_NOT_FOUND, "x");

    /* Indexes. */
    resolve_fails(r, inv, "items[8].count", DE_ERR_NOT_FOUND, "items[8]");
    resolve_fails(r, inv, "items[4294967296]", DE_ERR_NOT_FOUND, "items[4294967296]");
    resolve_fails(r, inv, "items[99999999999999999999999]", DE_ERR_NOT_FOUND,
                  "items[99999999999999999999999]");
    resolve_fails(r, inv, "count[0]", DE_ERR_NOT_FOUND, "count[0]");
    resolve_fails(r, inv, "items[1][2]", DE_ERR_INVALID_ARGUMENT, "items[1][2]");

    /* Malformed paths. */
    resolve_fails(r, inv, "", DE_ERR_INVALID_ARGUMENT, "");
    resolve_fails(r, inv, "items[]", DE_ERR_INVALID_ARGUMENT, "items[]");
    resolve_fails(r, inv, "items[x]", DE_ERR_INVALID_ARGUMENT, "items[x]");
    resolve_fails(r, inv, "items[1", DE_ERR_INVALID_ARGUMENT, "items[1");
    resolve_fails(r, ed, "transform..position", DE_ERR_INVALID_ARGUMENT, "");
    resolve_fails(r, ed, "transform.", DE_ERR_INVALID_ARGUMENT, "");
    resolve_fails(r, ed, ".transform", DE_ERR_INVALID_ARGUMENT, "");
    resolve_fails(r, ed, "1transform", DE_ERR_INVALID_ARGUMENT, "1transform");
    resolve_fails(r, ed, "transform-x", DE_ERR_INVALID_ARGUMENT, "transform-x");

    /* Bad ids and arguments. */
    const de_field_info *fi;
    u32 off;
    de_type_id none = {0};
    EXPECT_EQ(de_reflect_resolve_path(r, none, "x", &fi, &off), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_reflect_resolve_path(r, f->ids.mode, "x", &fi, &off), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_reflect_resolve_path(r, inv, NULL, &fi, &off), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_reflect_resolve_path(r, inv, "count", NULL, &off), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_reflect_resolve_path(r, inv, "count", &fi, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_reflect_resolve_path(NULL, inv, "count", &fi, &off), DE_ERR_INVALID_ARGUMENT);
}
