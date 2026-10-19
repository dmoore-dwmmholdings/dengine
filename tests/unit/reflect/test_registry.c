/* test_registry.c: the type registry (DENG-294): registration, validation, schema hash, hot
 * reload re-registration, enums, hooks and the de_ser_context declaration. */
#include "de_test.h"
#include "test_types.h"

#include "../core/test_counting_alloc.h"

#include <stdio.h>
#include <string.h>

/* The golden schema hash of TTransform (version 1). A cross-platform contract: binary files
 * store schema hashes. If this changes, the hash encoding changed: bump the "de.type.v1"
 * domain string in src/reflect/schema_hash.c and the binary format version instead of
 * updating the value. */
#define TTRANSFORM_SCHEMA_HASH_GOLDEN 0xc7444a2957d85c69ull

typedef struct reg_fx {
    de_type_registry *r;
} reg_fx;

static void reg_fx_setup(reg_fx *f) {
    ASSERT_EQ(de_type_registry_create(NULL, &f->r), DE_OK);
}

static void reg_fx_teardown(reg_fx *f) {
    de_type_registry_destroy(f->r);
}

/* Registration fails with DE_ERR_INVALID_ARGUMENT and a message naming `type` and `field`. */
static void expect_invalid(de_type_registry *r, const de_type_desc *d, const char *type,
                           const char *field) {
    de_error_clear();
    const de_type_id before = type ? de_type_find(r, type) : (de_type_id){0};
    de_type_id id;
    id.id = 123;
    EXPECT_EQ(de_type_register_ex(r, d, &id, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(id.id, 0u);
    const de_error *e = de_error_last();
    ASSERT_TRUE(e != NULL);
    EXPECT_EQ(e->result, DE_ERR_INVALID_ARGUMENT);
    char want_type[96], want_field[96];
    snprintf(want_type, sizeof want_type, "'%s'", type);
    snprintf(want_field, sizeof want_field, "'%s'", field);
    if (!strstr(e->message, want_type) || (field[0] && !strstr(e->message, want_field))) {
        EXPECT_STREQ(e->message, "(a message naming the type and the field)");
    }
    /* A failed registration never adds or changes the name's id. */
    EXPECT_EQ(de_type_find(r, type).id, before.id);
}

/* ---- The minimal_game.c registration ------------------------------------------------------- */

typedef struct DeSpin {
    f32 speed_rad;
} DeSpin;

TEST_F(reg, minimal_game_despin, reg_fx) {
    static const de_field_info spin_fields[] = {
        DE_FIELD(DeSpin, speed_rad, DE_FK_F32, DE_FIELD_DEFAULT | DE_FIELD_REPLICATE),
    };
    de_type_id t = de_type_register(f->r, &DE_TYPE_DESC(DeSpin, 1, spin_fields));
    ASSERT_TRUE(DE_HANDLE_VALID(t));

    de_type_id found = de_type_find(f->r, "DeSpin");
    EXPECT_EQ(found.id, t.id);
    EXPECT_EQ(de_type_find(f->r, "DeSpinX").id, 0u);

    const de_type_desc *d = de_type_get(f->r, t);
    ASSERT_TRUE(d != NULL);
    EXPECT_STREQ(d->name, "DeSpin");
    EXPECT_EQ(d->size, (u32)sizeof(DeSpin));
    EXPECT_EQ(d->align, (u32)DE_ALIGNOF(DeSpin));
    EXPECT_EQ(d->version, 1u);
    EXPECT_TRUE(d->is_pod);
    ASSERT_EQ(d->field_count, 1u);
    EXPECT_STREQ(d->fields[0].name, "speed_rad");
    EXPECT_EQ(d->fields[0].offset, 0u);
    EXPECT_EQ(d->fields[0].size, 4u);
    EXPECT_EQ(d->fields[0].kind, DE_FK_F32);
    EXPECT_EQ(d->fields[0].flags, (u32)(DE_FIELD_DEFAULT | DE_FIELD_REPLICATE));
    /* Deep copy: the registry's strings and array are not the caller's. */
    EXPECT_TRUE(d->fields != spin_fields);
    EXPECT_TRUE(d->name != spin_fields[0].name);

    u32 count = 99;
    const de_field_info *fields = de_type_fields(f->r, t, &count);
    EXPECT_EQ(count, 1u);
    EXPECT_TRUE(fields == d->fields);
    EXPECT_TRUE(de_type_get_previous(f->r, t) == NULL);
    EXPECT_NE(de_type_schema_hash(f->r, t), 0u);
}

TEST_F(reg, test_types_register, reg_fx) {
    test_type_ids ids;
    ASSERT_EQ(test_types_register(f->r, &ids), DE_OK);
    EXPECT_TRUE(DE_HANDLE_VALID(ids.all_kinds));
    const de_type_desc *d = de_type_get(f->r, ids.all_kinds);
    ASSERT_TRUE(d != NULL);
    EXPECT_EQ(d->field_count, TEST_COUNT_OF(k_tallkinds_fields));
    /* Every kind appears in TAllKinds. */
    u32 seen = 0;
    for (u32 i = 0; i < d->field_count; ++i)
        seen |= 1u << (u32)d->fields[i].kind;
    EXPECT_EQ(seen, (1u << DE_FIELD_KIND_COUNT) - 1u);
    /* Enums are found by name but are not struct types. */
    EXPECT_TRUE(de_type_get(f->r, ids.mode) == NULL);
    EXPECT_TRUE(de_enum_get(f->r, ids.vec) == NULL);
}

/* ---- Validation: one test per rule ---------------------------------------------------------- */

typedef struct TBad {
    u32 a;
    u32 b;
    f32 c;
    u32 d;
} TBad;

static de_type_desc bad_desc(const de_field_info *fields, u32 n) {
    de_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = "TBad";
    d.size = (u32)sizeof(TBad);
    d.align = 4;
    d.version = 1;
    d.fields = fields;
    d.field_count = n;
    d.is_pod = true;
    return d;
}

TEST_F(reg, validate_type_name, reg_fx) {
    de_field_info fl[] = {DE_FIELD(TBad, a, DE_FK_U32, 0)};
    de_type_desc d = bad_desc(fl, 1);
    d.name = "Bad Name";
    expect_invalid(f->r, &d, "Bad Name", "");
    d.name = "vec3"; /* built-in kind names are reserved */
    expect_invalid(f->r, &d, "vec3", "");
    d.name = NULL;
    expect_invalid(f->r, &d, "(null)", "");
    de_error_clear();
    EXPECT_EQ(de_type_register_ex(f->r, NULL, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_type_register(f->r, NULL).id, 0u);
}

TEST_F(reg, validate_size_align, reg_fx) {
    de_field_info fl[] = {DE_FIELD(TBad, a, DE_FK_U32, 0)};
    de_type_desc d = bad_desc(fl, 1);
    d.align = 3;
    expect_invalid(f->r, &d, "TBad", "");
    d.align = 32; /* does not divide 16 */
    expect_invalid(f->r, &d, "TBad", "");
    d.align = 4;
    d.size = 0;
    expect_invalid(f->r, &d, "TBad", "");
    d = bad_desc(NULL, 2);
    expect_invalid(f->r, &d, "TBad", "");
}

TEST_F(reg, validate_field_name, reg_fx) {
    de_field_info fl[] = {DE_FIELD(TBad, a, DE_FK_U32, 0), DE_FIELD(TBad, b, DE_FK_U32, 0)};
    fl[1].name = "b.x";
    de_type_desc d = bad_desc(fl, 2);
    expect_invalid(f->r, &d, "TBad", "b.x");
    fl[1].name = "9b";
    expect_invalid(f->r, &d, "TBad", "9b");
    fl[1].name = "";
    expect_invalid(f->r, &d, "TBad", "");
}

TEST_F(reg, validate_unique_names, reg_fx) {
    de_field_info fl[] = {DE_FIELD(TBad, a, DE_FK_U32, 0), DE_FIELD(TBad, b, DE_FK_U32, 0)};
    fl[1].name = "a";
    de_type_desc d = bad_desc(fl, 2);
    expect_invalid(f->r, &d, "TBad", "a");
}

TEST_F(reg, validate_offset_size, reg_fx) {
    de_field_info fl[] = {DE_FIELD(TBad, d, DE_FK_U32, 0)};
    fl[0].offset = 14; /* 14 + 4 > 16 */
    de_type_desc d = bad_desc(fl, 1);
    expect_invalid(f->r, &d, "TBad", "d");
    fl[0].offset = 0xfffffffeu; /* no u32 wrap-around */
    expect_invalid(f->r, &d, "TBad", "d");
}

TEST_F(reg, validate_overlap, reg_fx) {
    de_field_info fl[] = {DE_FIELD(TBad, a, DE_FK_U64, 0), DE_FIELD(TBad, b, DE_FK_U32, 0)};
    fl[0].size = 8; /* a covers [0, 8), b is at 4 */
    de_type_desc d = bad_desc(fl, 2);
    expect_invalid(f->r, &d, "TBad", "b");
}

TEST_F(reg, validate_kind_size, reg_fx) {
    /* Every fixed-size kind with a size one byte too large. */
    static const struct {
        de_field_kind kind;
        u32 size;
    } k_sizes[] = {
        {DE_FK_BOOL, 1},  {DE_FK_I8, 1},    {DE_FK_I16, 2},    {DE_FK_I32, 4},   {DE_FK_I64, 8},
        {DE_FK_U8, 1},    {DE_FK_U16, 2},   {DE_FK_U32, 4},    {DE_FK_U64, 8},   {DE_FK_F32, 4},
        {DE_FK_F64, 8},   {DE_FK_VEC2, 8},  {DE_FK_VEC3, 12},  {DE_FK_VEC4, 16}, {DE_FK_DVEC3, 24},
        {DE_FK_QUAT, 16}, {DE_FK_MAT4, 64}, {DE_FK_COLOR, 16}, {DE_FK_NAME, 8},  {DE_FK_ENTITY, 8},
        {DE_FK_ASSET, 4},
    };
    for (u32 i = 0; i < TEST_COUNT_OF(k_sizes); ++i) {
        de_field_info fl[1];
        memset(fl, 0, sizeof fl);
        fl[0].name = "v";
        fl[0].kind = k_sizes[i].kind;
        fl[0].size = k_sizes[i].size;
        de_type_desc d = bad_desc(fl, 1);
        d.size = 128;
        d.align = 0;
        /* The right size registers... */
        de_type_id id;
        char name[16];
        snprintf(name, sizeof name, "TOk%u", i);
        d.name = name;
        EXPECT_EQ(de_type_register_ex(f->r, &d, &id, NULL), DE_OK);
        /* ...one more byte does not. */
        fl[0].size = k_sizes[i].size + 1;
        d.name = "TBad";
        expect_invalid(f->r, &d, "TBad", "v");
    }
    /* An unknown kind. */
    de_field_info fl[] = {DE_FIELD(TBad, a, DE_FK_U32, 0)};
    fl[0].kind = (de_field_kind)99;
    de_type_desc d = bad_desc(fl, 1);
    expect_invalid(f->r, &d, "TBad", "a");
}

TEST_F(reg, validate_string, reg_fx) {
    de_field_info fl[] = {DE_FIELD(TBad, a, DE_FK_STRING, 0)};
    fl[0].size = 0;
    de_type_desc d = bad_desc(fl, 1);
    expect_invalid(f->r, &d, "TBad", "a");
    fl[0].size = 16; /* char[16] over the whole type is fine */
    EXPECT_TRUE(DE_HANDLE_VALID(de_type_register(f->r, &d)));
}

TEST_F(reg, validate_enum, reg_fx) {
    ASSERT_EQ(de_enum_register(f->r, "TMode", k_tmode_values, TEST_COUNT_OF(k_tmode_values), false),
              DE_OK);
    ASSERT_EQ(
        de_enum_register(f->r, "TLayers", k_tlayers_values, TEST_COUNT_OF(k_tlayers_values), true),
        DE_OK);
    de_field_info fl[] = {DE_FIELD_EX(TBad, a, DE_FK_ENUM, 0, {0, 0, 0, 0, "TMode", 0})};
    de_type_desc d = bad_desc(fl, 1);

    fl[0].size = 3; /* not 1, 2, 4 or 8 */
    expect_invalid(f->r, &d, "TBad", "a");
    fl[0].size = 4;
    fl[0].attrs.nested_type = NULL; /* no enum named */
    expect_invalid(f->r, &d, "TBad", "a");
    fl[0].attrs.nested_type = "TNope"; /* not registered */
    expect_invalid(f->r, &d, "TBad", "a");
    fl[0].attrs.nested_type = "TLayers"; /* ENUM naming a flags enum */
    expect_invalid(f->r, &d, "TBad", "a");
    fl[0].kind = DE_FK_FLAGS;
    fl[0].attrs.nested_type = "TMode"; /* FLAGS naming a plain enum */
    expect_invalid(f->r, &d, "TBad", "a");

    /* Values that do not fit the field size. */
    static const de_enum_value big[] = {{"Small", 1}, {"Big", 70000}};
    ASSERT_EQ(de_enum_register(f->r, "TBig", big, 2, false), DE_OK);
    fl[0].kind = DE_FK_ENUM;
    fl[0].attrs.nested_type = "TBig";
    fl[0].size = 2;
    expect_invalid(f->r, &d, "TBad", "a");
    fl[0].size = 4;
    EXPECT_TRUE(DE_HANDLE_VALID(de_type_register(f->r, &d)));
}

TEST_F(reg, validate_struct, reg_fx) {
    test_type_ids ids;
    ASSERT_EQ(test_types_register(f->r, &ids), DE_OK);
    de_field_info fl[] = {DE_FIELD_EX(TBad, a, DE_FK_STRUCT, 0, {0, 0, 0, 0, "TNope", 0})};
    fl[0].size = 12;
    de_type_desc d = bad_desc(fl, 1);
    expect_invalid(f->r, &d, "TBad", "a"); /* not registered */
    fl[0].attrs.nested_type = NULL;
    expect_invalid(f->r, &d, "TBad", "a"); /* none named */
    fl[0].attrs.nested_type = "TMode";
    expect_invalid(f->r, &d, "TBad", "a"); /* an enum, not a struct */
    fl[0].attrs.nested_type = "TVec";
    fl[0].size = 8;
    expect_invalid(f->r, &d, "TBad", "a"); /* TVec is 12 bytes */
    fl[0].size = 12;
    EXPECT_TRUE(DE_HANDLE_VALID(de_type_register(f->r, &d)));

    /* A cycle through re-registration: TVec cannot nest TBad, which nests TVec. */
    typedef struct TVec2 {
        TBad bad;
        f32 pad[2];
    } TVec2;
    de_field_info cyc[] = {DE_FIELD_EX(TVec2, bad, DE_FK_STRUCT, 0, {0, 0, 0, 0, "TBad", 0})};
    de_type_desc c;
    memset(&c, 0, sizeof c);
    c.name = "TVec";
    c.size = (u32)sizeof(TVec2);
    c.fields = cyc;
    c.field_count = 1;
    expect_invalid(f->r, &c, "TVec", "bad");
    /* The failed re-registration left TVec as it was. */
    EXPECT_EQ(de_type_get(f->r, ids.vec)->field_count, 3u);
}

TEST_F(reg, validate_fixed_array, reg_fx) {
    test_type_ids ids;
    ASSERT_EQ(test_types_register(f->r, &ids), DE_OK);
    typedef struct TArr {
        u8 bytes[6];
        u8 pad[2];
        TItem items[3];
    } TArr;
    de_field_info fl[] = {DE_FIELD_EX(TArr, items, DE_FK_FIXED_ARRAY, 0, {0, 0, 0, 0, "TItem", 0})};
    de_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = "TBad";
    d.size = (u32)sizeof(TArr);
    d.fields = fl;
    d.field_count = 1;

    expect_invalid(f->r, &d, "TBad", "items"); /* array_count 0 */
    fl[0].attrs.array_count = 5;
    expect_invalid(f->r, &d, "TBad", "items"); /* 24 bytes is not a multiple of 5 */
    fl[0].attrs.array_count = 4;
    expect_invalid(f->r, &d, "TBad", "items"); /* 6-byte elements, TItem is 8 */
    fl[0].attrs.array_count = 3;
    fl[0].attrs.nested_type = "TNope";
    expect_invalid(f->r, &d, "TBad", "items"); /* unknown element type */
    fl[0].attrs.nested_type = "vec3";
    expect_invalid(f->r, &d, "TBad", "items"); /* 8-byte elements, vec3 is 12 */
    fl[0].attrs.nested_type = "TLayers";
    fl[0].kind = DE_FK_FIXED_ARRAY;
    fl[0].attrs.array_count = 6; /* 4-byte flags elements are fine... */
    d.name = "TFlagsArr";
    EXPECT_TRUE(DE_HANDLE_VALID(de_type_register(f->r, &d)));
    d.name = "TBad";
    fl[0].attrs.array_count = 8; /* ...3-byte ones are not */
    expect_invalid(f->r, &d, "TBad", "items");

    /* No nested_type: elements are unsigned ints of 1, 2, 4 or 8 bytes. */
    de_field_info by[] = {DE_FIELD(TArr, bytes, DE_FK_FIXED_ARRAY, 0)};
    by[0].attrs.array_count = 2; /* 3-byte elements */
    d.fields = by;
    expect_invalid(f->r, &d, "TBad", "bytes");
    by[0].attrs.array_count = 6;
    d.name = "TArrOk";
    de_type_id ok = de_type_register(f->r, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(ok));
    const de_field_info *el = de_type_array_element(f->r, ok, 0);
    ASSERT_TRUE(el != NULL);
    EXPECT_EQ(el->kind, DE_FK_U8);
    EXPECT_EQ(el->size, 1u);
}

TEST_F(reg, validate_fixed_array_enum_elements, reg_fx) {
    test_type_ids ids;
    ASSERT_EQ(test_types_register(f->r, &ids), DE_OK);
    typedef struct TModes {
        u16 modes[4];
    } TModes;
    de_field_info fl[] = {
        DE_FIELD_EX(TModes, modes, DE_FK_FIXED_ARRAY, 0, {0, 0, 0, 0, "TMode", 4})};
    de_type_id id = de_type_register(f->r, &DE_TYPE_DESC(TModes, 1, fl));
    ASSERT_TRUE(DE_HANDLE_VALID(id));
    const de_field_info *el = de_type_array_element(f->r, id, 0);
    ASSERT_TRUE(el != NULL);
    EXPECT_EQ(el->kind, DE_FK_ENUM);
    EXPECT_EQ(el->size, 2u);
    EXPECT_STREQ(el->attrs.nested_type, "TMode");
    /* Three-byte enum elements are invalid. */
    typedef struct TModes3 {
        u8 modes[12];
    } TModes3;
    de_field_info bad[] = {
        DE_FIELD_EX(TModes3, modes, DE_FK_FIXED_ARRAY, 0, {0, 0, 0, 0, "TMode", 4})};
    de_type_desc d = DE_TYPE_DESC(TModes3, 1, bad);
    expect_invalid(f->r, &d, "TModes3", "modes");
}

TEST_F(reg, validate_enum_register, reg_fx) {
    static const de_enum_value dup_name[] = {{"A", 1}, {"A", 2}};
    static const de_enum_value dup_value[] = {{"A", 1}, {"B", 1}};
    static const de_enum_value bad_name[] = {{"A b", 1}};
    EXPECT_EQ(de_enum_register(f->r, "TE", dup_name, 2, false), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(de_error_last()->message, "'TE'") != NULL);
    EXPECT_EQ(de_enum_register(f->r, "TE", dup_value, 2, false), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_enum_register(f->r, "TE", bad_name, 1, false), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_enum_register(f->r, "TE", dup_name, 0, false), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_enum_register(f->r, "T E", k_tmode_values, 1, false), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_enum_register(f->r, "u32", k_tmode_values, 1, false), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_type_find(f->r, "TE").id, 0u);
}

TEST_F(reg, name_conflicts, reg_fx) {
    test_type_ids ids;
    ASSERT_EQ(test_types_register(f->r, &ids), DE_OK);
    /* A struct name used by an enum, and the other way round. */
    EXPECT_EQ(de_enum_register(f->r, "TVec", k_tmode_values, 1, false), DE_ERR_ALREADY_EXISTS);
    de_type_desc d = DE_TYPE_DESC(TVec, 1, k_tvec_fields);
    d.name = "TMode";
    de_type_id id;
    EXPECT_EQ(de_type_register_ex(f->r, &d, &id, NULL), DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(id.id, 0u);
    EXPECT_TRUE(de_enum_get(f->r, ids.mode) != NULL);
}

/* ---- Ids --------------------------------------------------------------------------------- */

TEST_F(reg, invalid_ids, reg_fx) {
    test_type_ids ids;
    ASSERT_EQ(test_types_register(f->r, &ids), DE_OK);
    de_type_id none = {0};
    de_type_id stale = ids.vec;
    stale.id += 1u << 20; /* another generation */
    de_type_id far = {0xfffffu | (1u << 20)};
    const de_type_id bad[] = {none, stale, far};
    for (u32 i = 0; i < 3; ++i) {
        EXPECT_TRUE(de_type_get(f->r, bad[i]) == NULL);
        EXPECT_TRUE(de_type_get_previous(f->r, bad[i]) == NULL);
        EXPECT_EQ(de_type_schema_hash(f->r, bad[i]), 0u);
        u32 n = 7;
        EXPECT_TRUE(de_type_fields(f->r, bad[i], &n) == NULL);
        EXPECT_EQ(n, 0u);
        EXPECT_TRUE(de_enum_get(f->r, bad[i]) == NULL);
        TVec v;
        EXPECT_EQ(de_type_construct(f->r, bad[i], &v), DE_ERR_INVALID_ARGUMENT);
        EXPECT_EQ(de_type_copy(f->r, bad[i], &v, &v), DE_ERR_INVALID_ARGUMENT);
        de_type_destruct(f->r, bad[i], &v);
    }
    EXPECT_TRUE(de_type_get(NULL, ids.vec) == NULL);
    EXPECT_EQ(de_type_find(NULL, "TVec").id, 0u);
    EXPECT_EQ(de_type_find(f->r, NULL).id, 0u);
    EXPECT_TRUE(de_type_array_element(f->r, ids.inventory, 0) == NULL); /* `count` is a u32 */
    EXPECT_TRUE(de_type_array_element(f->r, ids.inventory, 2) == NULL); /* no field 2 */
}

/* ---- Schema hash ----------------------------------------------------------------------------- */

static u64 hash_of(const de_type_desc *d, bool with_types) {
    de_type_registry *r = NULL;
    if (de_type_registry_create(NULL, &r) != DE_OK)
        return 0;
    test_type_ids ids;
    if (with_types && test_types_register(r, &ids) != DE_OK) {
        de_type_registry_destroy(r);
        return 0;
    }
    de_type_id id = de_type_register(r, d);
    u64 h = de_type_schema_hash(r, id);
    de_type_registry_destroy(r);
    return h;
}

TEST_F(reg, schema_hash_golden, reg_fx) {
    test_type_ids ids;
    ASSERT_EQ(test_types_register(f->r, &ids), DE_OK);
    u64 h = de_type_schema_hash(f->r, ids.transform);
    EXPECT_EQ(h, TTRANSFORM_SCHEMA_HASH_GOLDEN);

    /* The documented encoding (src/reflect/schema_hash.c), built by hand. It is shorter than
     * the 256-byte buffer, so the hash is one de_hash64 with seed 0. */
    u8 buf[256];
    u32 n = 0;
#define PUT_U32(v)                                                                                 \
    do {                                                                                           \
        u32 v_ = (u32)(v);                                                                         \
        for (u32 b_ = 0; b_ < 4; ++b_)                                                             \
            buf[n++] = (u8)(v_ >> (8 * b_));                                                       \
    } while (0)
#define PUT_STR(s)                                                                                 \
    do {                                                                                           \
        PUT_U32(strlen(s));                                                                        \
        memcpy(buf + n, (s), strlen(s));                                                           \
        n += (u32)strlen(s);                                                                       \
    } while (0)
    PUT_STR("de.type.v1");
    PUT_STR("TTransform");
    PUT_U32(1);  /* version */
    PUT_U32(64); /* size */
    PUT_U32(3);  /* fields */
    const u32 schema = DE_FIELD_SERIALIZE | DE_FIELD_SNAPSHOT;
    const struct {
        const char *name;
        de_field_kind kind;
        u32 size, offset, flags;
    } k_enc[] = {{"position", DE_FK_DVEC3, 24, 0, schema | DE_FIELD_REPLICATE},
                 {"rotation", DE_FK_QUAT, 16, 32, schema | DE_FIELD_REPLICATE},
                 {"scale", DE_FK_VEC3, 12, 48, schema}};
    for (u32 i = 0; i < 3; ++i) {
        PUT_STR(k_enc[i].name);
        PUT_U32(k_enc[i].kind);
        PUT_U32(k_enc[i].size);
        PUT_U32(k_enc[i].offset);
        PUT_U32(k_enc[i].flags);
        PUT_U32(0); /* nested hash, low and high half */
        PUT_U32(0);
        PUT_U32(0); /* array count */
    }
#undef PUT_STR
#undef PUT_U32
    EXPECT_EQ(de_hash64(buf, n, 0), h);

    /* The same description in another registry, registered alone or after other types. */
    EXPECT_EQ(hash_of(&DE_TYPE_DESC(TTransform, 1, k_ttransform_fields), false), h);
    EXPECT_EQ(hash_of(&DE_TYPE_DESC(TTransform, 1, k_ttransform_fields), true), h);
}

TEST_F(reg, schema_hash_changes, reg_fx) {
    (void)f;
    de_field_info fl[TEST_COUNT_OF(k_ttransform_fields)];
    memcpy(fl, k_ttransform_fields, sizeof fl);
    de_type_desc d = DE_TYPE_DESC(TTransform, 1, fl);
    const u64 base = hash_of(&d, false);
    ASSERT_NE(base, 0u);

    d.version = 2;
    EXPECT_NE(hash_of(&d, false), base);
    d.version = 1;
    d.name = "TTransform2";
    EXPECT_NE(hash_of(&d, false), base);
    d.name = "TTransform";

    fl[2].name = "scale2";
    EXPECT_NE(hash_of(&d, false), base);
    fl[2].name = "scale";
    fl[1].kind = DE_FK_VEC4; /* QUAT -> VEC4, same size */
    EXPECT_NE(hash_of(&d, false), base);
    fl[1].kind = DE_FK_QUAT;
    /* scale moves into the trailing padding (48 -> 52; 52 + 12 = sizeof(TTransform)). */
    ASSERT_EQ(sizeof(TTransform), 64u);
    fl[2].offset = 52;
    EXPECT_NE(hash_of(&d, false), base);
    fl[2].offset = (u32)offsetof(TTransform, scale);
    EXPECT_EQ(hash_of(&d, false), base);

    /* Each schema flag changes the hash. */
    const u32 schema_flags[] = {DE_FIELD_SERIALIZE, DE_FIELD_REPLICATE, DE_FIELD_SNAPSHOT};
    for (u32 i = 0; i < 3; ++i) {
        fl[0].flags ^= schema_flags[i];
        EXPECT_NE(hash_of(&d, false), base);
        fl[0].flags ^= schema_flags[i];
    }
    EXPECT_EQ(hash_of(&d, false), base);
}

TEST_F(reg, schema_hash_ignores_attrs, reg_fx) {
    (void)f;
    de_field_info fl[TEST_COUNT_OF(k_ttransform_fields)];
    memcpy(fl, k_ttransform_fields, sizeof fl);
    de_type_desc d = DE_TYPE_DESC(TTransform, 1, fl);
    const u64 base = hash_of(&d, false);

    fl[0].attrs.tooltip = "Another tooltip";
    fl[0].attrs.units = "km";
    fl[1].attrs.min = -1;
    fl[1].attrs.max = 1;
    fl[2].attrs.min = 0;
    fl[2].attrs.max = 0;
    /* Non-schema flags are editor metadata. */
    fl[0].flags |= DE_FIELD_EDITOR_HIDDEN | DE_FIELD_READONLY | DE_FIELD_ASSET_REF;
    /* Hooks, alignment and is_pod are not layout either. */
    d.is_pod = false;
    d.align = 0;
    EXPECT_EQ(hash_of(&d, false), base);
}

TEST_F(reg, schema_hash_nested, reg_fx) {
    test_type_ids ids;
    ASSERT_EQ(test_types_register(f->r, &ids), DE_OK);
    const u64 entity_hash = de_type_schema_hash(f->r, ids.entity_data);
    const u64 all_hash = de_type_schema_hash(f->r, ids.all_kinds);

    /* A field becomes non-replicated in the nested TTransform: the outer hash changes too. */
    de_field_info fl[TEST_COUNT_OF(k_ttransform_fields)];
    memcpy(fl, k_ttransform_fields, sizeof fl);
    fl[1].flags &= ~(u32)DE_FIELD_REPLICATE;
    bool changed = false;
    de_type_id id;
    ASSERT_EQ(de_type_register_ex(f->r, &DE_TYPE_DESC(TTransform, 1, fl), &id, &changed), DE_OK);
    EXPECT_TRUE(changed);
    EXPECT_NE(de_type_schema_hash(f->r, ids.entity_data), entity_hash);

    /* An enum's values are part of the hash of the types using it. */
    static const de_enum_value more[] = {
        {"Idle", 0}, {"Walk", 1}, {"Run", 2}, {"Negative", -1}, {"Swim", 3}};
    ASSERT_EQ(de_enum_register(f->r, "TMode", more, 5, false), DE_OK);
    EXPECT_NE(de_type_schema_hash(f->r, ids.all_kinds), all_hash);
    /* Re-registering TAllKinds now reports the layout change it got through TMode. */
    ASSERT_EQ(
        de_type_register_ex(f->r, &DE_TYPE_DESC(TAllKinds, 1, k_tallkinds_fields), &id, &changed),
        DE_OK);
    EXPECT_EQ(id.id, ids.all_kinds.id);
    EXPECT_TRUE(changed);
    ASSERT_EQ(
        de_type_register_ex(f->r, &DE_TYPE_DESC(TAllKinds, 1, k_tallkinds_fields), &id, &changed),
        DE_OK);
    EXPECT_FALSE(changed);
}

/* ---- Re-registration (hot reload) ---------------------------------------------------------- */

static void tvec_construct_a(void *obj) {
    TVec *v = (TVec *)obj;
    v->x = v->y = v->z = 1.0f;
}
static void tvec_construct_b(void *obj) {
    TVec *v = (TVec *)obj;
    v->x = v->y = v->z = 2.0f;
}

TEST_F(reg, reregister_same, reg_fx) {
    de_type_desc d = DE_TYPE_DESC(TVec, 1, k_tvec_fields);
    d.hooks.construct = tvec_construct_a;
    de_type_id a, b;
    bool changed = true;
    ASSERT_EQ(de_type_register_ex(f->r, &d, &a, &changed), DE_OK);
    EXPECT_FALSE(changed);
    const u64 h = de_type_schema_hash(f->r, a);

    /* The reloaded library brings new hook addresses and tooltips, same layout. */
    de_field_info fl[3];
    memcpy(fl, k_tvec_fields, sizeof fl);
    fl[0].attrs.tooltip = "X axis";
    de_type_desc d2 = DE_TYPE_DESC(TVec, 1, fl);
    d2.hooks.construct = tvec_construct_b;
    changed = true;
    ASSERT_EQ(de_type_register_ex(f->r, &d2, &b, &changed), DE_OK);
    EXPECT_EQ(b.id, a.id);
    EXPECT_FALSE(changed);
    EXPECT_EQ(de_type_schema_hash(f->r, a), h);
    EXPECT_TRUE(de_type_get_previous(f->r, a) == NULL);
    EXPECT_STREQ(de_type_get(f->r, a)->fields[0].attrs.tooltip, "X axis");
    TVec v;
    ASSERT_EQ(de_type_construct(f->r, a, &v), DE_OK);
    EXPECT_FLOAT_EQ(v.x, 2.0, 0.0); /* the new hook */

    /* de_type_register returns the same id too. */
    EXPECT_EQ(de_type_register(f->r, &d2).id, a.id);
}

typedef struct TVecV2 {
    f32 x, y, z;
    f32 w;
} TVecV2;

TEST_F(reg, reregister_changed, reg_fx) {
    test_type_ids ids;
    ASSERT_EQ(test_types_register(f->r, &ids), DE_OK);
    const de_type_desc *old = de_type_get(f->r, ids.vec);
    const u64 old_hash = de_type_schema_hash(f->r, ids.vec);

    static const de_field_info v2[] = {
        DE_FIELD(TVecV2, x, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(TVecV2, y, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(TVecV2, z, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(TVecV2, w, DE_FK_F32, DE_FIELD_DEFAULT),
    };
    de_type_desc d = DE_TYPE_DESC(TVecV2, 2, v2);
    d.name = "TVec";
    de_type_id id;
    bool changed = false;
    ASSERT_EQ(de_type_register_ex(f->r, &d, &id, &changed), DE_OK);
    EXPECT_EQ(id.id, ids.vec.id);
    EXPECT_TRUE(changed);
    EXPECT_NE(de_type_schema_hash(f->r, id), old_hash);

    const de_type_desc *cur = de_type_get(f->r, id);
    ASSERT_EQ(cur->field_count, 4u);
    EXPECT_EQ(cur->version, 2u);
    EXPECT_EQ(cur->size, 16u);
    const de_type_desc *prev = de_type_get_previous(f->r, id);
    ASSERT_TRUE(prev != NULL);
    EXPECT_TRUE(prev == old); /* the old pointer is still valid and is the previous layout */
    EXPECT_EQ(prev->field_count, 3u);
    EXPECT_EQ(prev->version, 1u);
    EXPECT_EQ(prev->size, 12u);
    EXPECT_STREQ(prev->fields[2].name, "z");

    /* The same new layout again: unchanged, previous kept. */
    ASSERT_EQ(de_type_register_ex(f->r, &d, &id, &changed), DE_OK);
    EXPECT_FALSE(changed);
    EXPECT_TRUE(de_type_get_previous(f->r, id) == prev);

    /* TAllKinds nests TVec by name and still describes 12 bytes: re-registering it now fails
     * until its own layout is updated, and leaves it as it was. */
    de_type_desc all = DE_TYPE_DESC(TAllKinds, 1, k_tallkinds_fields);
    expect_invalid(f->r, &all, "TAllKinds", "vec");
    EXPECT_TRUE(de_type_get(f->r, ids.all_kinds) != NULL);
}

/* ---- Enums ----------------------------------------------------------------------------------- */

TEST_F(reg, enum_lookup, reg_fx) {
    test_type_ids ids;
    ASSERT_EQ(test_types_register(f->r, &ids), DE_OK);
    const de_enum_info *e = de_enum_get(f->r, ids.mode);
    ASSERT_TRUE(e != NULL);
    EXPECT_STREQ(e->name, "TMode");
    EXPECT_EQ(e->count, 4u);
    EXPECT_FALSE(e->is_flags);
    EXPECT_TRUE(e->values != k_tmode_values); /* copied */

    i64 v = 42;
    EXPECT_TRUE(de_enum_value_of(f->r, ids.mode, "Run", &v));
    EXPECT_EQ(v, 2);
    EXPECT_TRUE(de_enum_value_of(f->r, ids.mode, "Negative", &v));
    EXPECT_EQ(v, -1);
    v = 42;
    EXPECT_FALSE(de_enum_value_of(f->r, ids.mode, "Fly", &v));
    EXPECT_EQ(v, 42);
    EXPECT_FALSE(de_enum_value_of(f->r, ids.vec, "x", &v));
    EXPECT_STREQ(de_enum_name_of(f->r, ids.mode, 1), "Walk");
    EXPECT_STREQ(de_enum_name_of(f->r, ids.mode, -1), "Negative");
    EXPECT_TRUE(de_enum_name_of(f->r, ids.mode, 7) == NULL);

    const de_enum_info *fl = de_enum_get(f->r, ids.layers);
    ASSERT_TRUE(fl != NULL);
    EXPECT_TRUE(fl->is_flags);
    EXPECT_STREQ(de_enum_name_of(f->r, ids.layers, TLAYER_AIR), "Air");
    EXPECT_TRUE(de_enum_name_of(f->r, ids.layers, TLAYER_AIR | TLAYER_WATER) == NULL);

    /* Re-registering keeps the id and replaces the values. */
    static const de_enum_value v2[] = {{"Idle", 0}, {"Sprint", 5}};
    ASSERT_EQ(de_enum_register(f->r, "TMode", v2, 2, false), DE_OK);
    EXPECT_EQ(de_type_find(f->r, "TMode").id, ids.mode.id);
    EXPECT_TRUE(de_enum_value_of(f->r, ids.mode, "Sprint", &v));
    EXPECT_EQ(v, 5);
    EXPECT_FALSE(de_enum_value_of(f->r, ids.mode, "Run", &v));
    EXPECT_STREQ(e->values[2].name, "Run"); /* the old values stay readable */
}

/* ---- Hooks ----------------------------------------------------------------------------------- */

static int g_copy_calls, g_destruct_calls;
static void titem_copy(void *dst, const void *src) {
    memcpy(dst, src, sizeof(TItem));
    ((TItem *)dst)->count += 1000;
    g_copy_calls++;
}
static void titem_destruct(void *obj) {
    ((TItem *)obj)->id = 0xdead;
    g_destruct_calls++;
}

TEST_F(reg, hooks, reg_fx) {
    test_type_ids ids;
    ASSERT_EQ(test_types_register(f->r, &ids), DE_OK);

    /* Defaults: zero-fill, memcpy for POD, no destruct. */
    TTransform t;
    memset(&t, 0xab, sizeof t);
    ASSERT_EQ(de_type_construct(f->r, ids.transform, &t), DE_OK);
    TTransform zero;
    memset(&zero, 0, sizeof zero);
    EXPECT_MEMEQ(&t, &zero, sizeof t);
    t.position.x = 5.0;
    TTransform t2;
    memset(&t2, 0, sizeof t2);
    ASSERT_EQ(de_type_copy(f->r, ids.transform, &t2, &t), DE_OK);
    EXPECT_MEMEQ(&t2, &t, sizeof t);
    de_type_destruct(f->r, ids.transform, &t2);
    EXPECT_FLOAT_EQ(t2.position.x, 5.0, 0.0);
    EXPECT_EQ(de_type_construct(f->r, ids.transform, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_type_construct(f->r, ids.mode, &t), DE_ERR_INVALID_ARGUMENT); /* an enum */

    /* Hooks are called. */
    de_type_desc d = DE_TYPE_DESC(TItem, 1, k_titem_fields);
    d.hooks.copy = titem_copy;
    d.hooks.destruct = titem_destruct;
    ASSERT_EQ(de_type_register(f->r, &d).id, ids.item.id);
    TItem a = {7, 3}, b = {0, 0};
    g_copy_calls = g_destruct_calls = 0;
    ASSERT_EQ(de_type_copy(f->r, ids.item, &b, &a), DE_OK);
    EXPECT_EQ(g_copy_calls, 1);
    EXPECT_EQ(b.count, 1003);
    de_type_destruct(f->r, ids.item, &b);
    EXPECT_EQ(g_destruct_calls, 1);
    EXPECT_EQ(b.id, 0xdeadu);

    /* Not POD and no copy hook: unsupported. */
    d.hooks.copy = NULL;
    d.is_pod = false;
    ASSERT_EQ(de_type_register(f->r, &d).id, ids.item.id);
    EXPECT_EQ(de_type_copy(f->r, ids.item, &b, &a), DE_ERR_UNSUPPORTED);
}

/* ---- Fields and array elements --------------------------------------------------------------- */

TEST_F(reg, fields_and_elements, reg_fx) {
    test_type_ids ids;
    ASSERT_EQ(test_types_register(f->r, &ids), DE_OK);
    u32 n = 0;
    const de_field_info *fl = de_type_fields(f->r, ids.inventory, &n);
    ASSERT_EQ(n, 2u);
    EXPECT_STREQ(fl[1].name, "items");
    EXPECT_EQ(fl[1].kind, DE_FK_FIXED_ARRAY);
    EXPECT_EQ(fl[1].size, (u32)sizeof(TItem) * 8u);
    const de_field_info *el = de_type_array_element(f->r, ids.inventory, 1);
    ASSERT_TRUE(el != NULL);
    EXPECT_STREQ(el->name, "items");
    EXPECT_EQ(el->kind, DE_FK_STRUCT);
    EXPECT_EQ(el->size, (u32)sizeof(TItem));
    EXPECT_EQ(el->offset, 0u);
    EXPECT_EQ(el->flags, (u32)DE_FIELD_DEFAULT);
    EXPECT_STREQ(el->attrs.nested_type, "TItem");
    EXPECT_EQ(el->attrs.array_count, 0u);

    /* f32 samples[4]: a built-in element kind. */
    const de_type_desc *all = de_type_get(f->r, ids.all_kinds);
    u32 samples = 0;
    while (samples < all->field_count && strcmp(all->fields[samples].name, "samples") != 0)
        ++samples;
    el = de_type_array_element(f->r, ids.all_kinds, samples);
    ASSERT_TRUE(el != NULL);
    EXPECT_EQ(el->kind, DE_FK_F32);
    EXPECT_EQ(el->size, 4u);
    EXPECT_TRUE(el->attrs.nested_type == NULL);
}

/* ---- Out of memory --------------------------------------------------------------------------- */

TEST(reg, oom) {
    de_type_registry *r = (de_type_registry *)1;
    EXPECT_EQ(de_type_registry_create(de_allocator_null(), &r), DE_ERR_OUT_OF_MEMORY);
    EXPECT_TRUE(r == NULL);
    EXPECT_EQ(de_type_registry_create(NULL, NULL), DE_ERR_INVALID_ARGUMENT);

    /* Fail the n-th allocation for growing n until registration of everything succeeds; every
     * failure is clean (no leak, registry still usable, nothing half-registered). */
    for (i64 n = 1;; ++n) {
        test_counting_alloc c;
        test_counting_init(&c);
        de_type_registry *reg = NULL;
        ASSERT_EQ(de_type_registry_create(&c.iface, &reg), DE_OK);
        c.fail_after = n;
        test_type_ids ids;
        de_result res = test_types_register(reg, &ids);
        if (res != DE_OK) {
            EXPECT_EQ(res, DE_ERR_OUT_OF_MEMORY);
            /* Whatever is registered is complete. */
            de_type_id t = de_type_find(reg, "TAllKinds");
            EXPECT_FALSE(DE_HANDLE_VALID(t));
            de_type_id v = de_type_find(reg, "TVec");
            if (DE_HANDLE_VALID(v))
                EXPECT_EQ(de_type_get(reg, v)->field_count, 3u);
        }
        de_type_registry_destroy(reg);
        EXPECT_EQ(c.live_blocks, 0u);
        EXPECT_EQ(c.size_errors, 0u);
        EXPECT_EQ(c.align_errors, 0u);
        if (res == DE_OK) {
            EXPECT_GT(n, 8); /* the loop really failed allocations along the way */
            break;
        }
        ASSERT_LT(n, 1000);
    }
}

/* ---- Serialization context declaration ------------------------------------------------------- */

static u64 ctx_e2s(u64 entity_bits, void *user) {
    return entity_bits + (u64)(uintptr_t)user;
}
static u64 ctx_s2e(u64 scene_id, void *user) {
    return scene_id - (u64)(uintptr_t)user;
}
static u64 ctx_a2i(u32 handle, const char *asset_type, void *user) {
    (void)asset_type;
    return (u64)handle * 2u + (u64)(uintptr_t)user;
}
static u32 ctx_i2a(u64 id, const char *asset_type, void *user) {
    (void)asset_type;
    return (u32)((id - (u64)(uintptr_t)user) / 2u);
}

TEST(reg, ser_context_decl) {
    de_ser_context ctx = {ctx_e2s, ctx_s2e, ctx_a2i, ctx_i2a, (void *)(uintptr_t)10};
    de_ser_options opt;
    memset(&opt, 0, sizeof opt);
    opt.format = DE_SER_TEXT;
    opt.required_flags = DE_FIELD_SERIALIZE;
    opt.ctx = &ctx;
    EXPECT_EQ(opt.ctx->entity_to_scene_id(5, opt.ctx->user), 15u);
    EXPECT_EQ(opt.ctx->scene_id_to_entity(15, opt.ctx->user), 5u);
    EXPECT_EQ(opt.ctx->asset_to_id(7, "mesh", opt.ctx->user), 24u);
    EXPECT_EQ(opt.ctx->id_to_asset(24, "mesh", opt.ctx->user), 7u);
    /* (de_ser_options){0} means raw values. */
    de_ser_options zero = {DE_SER_TEXT, 0, NULL};
    EXPECT_TRUE(zero.ctx == NULL);
}

/* ---- C++ registration macros ----------------------------------------------------------------- */

/* test_cxx_macros.cpp: registers a type with DE_FIELD / DE_FIELD_EX / DE_TYPE_DESC in C++17. */
de_type_id de__test_reflect_cxx_register(de_type_registry *r);

TEST_F(reg, cxx_macros, reg_fx) {
    de_type_id id = de__test_reflect_cxx_register(f->r);
    ASSERT_TRUE(DE_HANDLE_VALID(id));
    const de_type_desc *d = de_type_get(f->r, id);
    ASSERT_TRUE(d != NULL);
    EXPECT_STREQ(d->name, "TCxxHealth");
    EXPECT_EQ(d->field_count, 2u);
    EXPECT_STREQ(d->fields[1].attrs.units, "hp");
}

/* DENG-321 prerequisite: valid and errored path caches observe every successful publication. */
TEST_F(reg, revision_tracks_publications_without_changing_schema_identity, reg_fx) {
    EXPECT_EQ(de_type_registry_revision(NULL), 0u);
    EXPECT_EQ(de_type_registry_revision(f->r), 0u);
    de_field_info field = {"current", 0, sizeof(f32), DE_FK_F32, DE_FIELD_DEFAULT, {0}};
    de_type_desc type = {.name = "BoundHealth",
                         .size = 8,
                         .align = 4,
                         .version = 1,
                         .fields = &field,
                         .field_count = 1,
                         .is_pod = true};
    de_type_id id;
    bool layout = false;
    ASSERT_EQ(de_type_register_ex(f->r, &type, &id, &layout), DE_OK);
    EXPECT_EQ(de_type_registry_revision(f->r), 1u);
    u64 hash = de_type_schema_hash(f->r, id);
    EXPECT_EQ(de_type_find(f->r, "BoundHealth").id, id.id);
    EXPECT_EQ(de_type_registry_revision(f->r), 1u); /* Lookups are read-only. */
    field.attrs.tooltip = "changed metadata";
    ASSERT_EQ(de_type_register_ex(f->r, &type, &id, &layout), DE_OK);
    EXPECT_FALSE(layout);
    EXPECT_EQ(de_type_schema_hash(f->r, id), hash);
    EXPECT_EQ(de_type_registry_revision(f->r), 2u);
    const de_field_info *resolved;
    u32 offset = 99;
    ASSERT_EQ(de_reflect_resolve_path(f->r, id, "current", &resolved, &offset), DE_OK);
    EXPECT_EQ(offset, 0u);
    field.offset = 4;
    type.version = 2;
    ASSERT_EQ(de_type_register_ex(f->r, &type, &id, &layout), DE_OK);
    EXPECT_TRUE(layout);
    EXPECT_EQ(de_type_registry_revision(f->r), 3u);
    ASSERT_EQ(de_reflect_resolve_path(f->r, id, "current", &resolved, &offset), DE_OK);
    EXPECT_EQ(offset, 4u); /* A binding can re-resolve its moved field. */
    type.size = 1;
    EXPECT_EQ(de_type_register_ex(f->r, &type, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_type_registry_revision(f->r), 3u);
    const de_enum_value values[] = {{"A", 1}, {"B", 2}};
    ASSERT_EQ(de_enum_register(f->r, "BoundMode", values, 2, false), DE_OK);
    EXPECT_EQ(de_type_registry_revision(f->r), 4u);
    ASSERT_EQ(de_enum_register(f->r, "BoundMode", values, 2, false), DE_OK);
    EXPECT_EQ(de_type_registry_revision(f->r), 5u);
    EXPECT_EQ(de_enum_register(f->r, "BoundMode", values, 0, false), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_enum_register(f->r, "BoundHealth", values, 2, false), DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(de_type_registry_revision(f->r), 5u);
}
TEST(reg, revision_does_not_publish_failed_allocations) {
    test_counting_alloc c;
    test_counting_init(&c);
    de_type_registry *r = NULL;
    ASSERT_EQ(de_type_registry_create(&c.iface, &r), DE_OK);
    de_field_info field = {"value", 0, sizeof(u32), DE_FK_U32, DE_FIELD_DEFAULT, {0}};
    de_type_desc type = {.name = "BoundOOM",
                         .size = 4,
                         .align = 4,
                         .version = 1,
                         .fields = &field,
                         .field_count = 1,
                         .is_pod = true};
    c.fail_after = 0;
    EXPECT_EQ(de_type_register_ex(r, &type, NULL, NULL), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_type_registry_revision(r), 0u);
    const de_enum_value values[] = {{"Only", 1}};
    EXPECT_EQ(de_enum_register(r, "BoundOOMEnum", values, 1, false), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_type_registry_revision(r), 0u);
    c.fail_after = -1;
    de_type_id id;
    ASSERT_EQ(de_type_register_ex(r, &type, &id, NULL), DE_OK);
    const de_type_desc *previous = de_type_get(r, id);
    u64 hash = de_type_schema_hash(r, id);
    char large_tooltip[20000];
    memset(large_tooltip, 'x', sizeof large_tooltip - 1);
    large_tooltip[sizeof large_tooltip - 1] = 0;
    field.attrs.tooltip = large_tooltip;
    c.fail_after = 0;
    EXPECT_EQ(de_type_register_ex(r, &type, NULL, NULL), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_type_registry_revision(r), 1u);
    EXPECT_TRUE(de_type_get(r, id) == previous);
    EXPECT_EQ(de_type_schema_hash(r, id), hash);
    de_type_registry_destroy(r);
    EXPECT_EQ(c.live_blocks, 0u);
    EXPECT_EQ(c.size_errors, 0u);
}
