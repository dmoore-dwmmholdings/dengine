/* test_ser_text.c: the TOML text serializer (DENG-295): round trips of every field kind, exact
 * floats, golden output, unknown fields, versions and migrate, the entity/asset context, the
 * required_flags filter, multi-object documents, errors and scene IDs.
 *
 * Golden files live in data/ (DE_REFLECT_TEST_DATA_DIR). check_toml.py parses the same files
 * with Python's tomllib, so the byte-exact comparisons here also prove that the writer's output
 * is TOML another parser reads. On a mismatch the actual text is written to the current
 * directory as text_actual_<name>.toml. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* fopen, strcpy */
#endif
#include "de_test.h"
#include "test_types.h"

#include "../core/test_counting_alloc.h"

#include <float.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_REFLECT_TEST_DATA_DIR
#error "DE_REFLECT_TEST_DATA_DIR must be defined by tests/unit/reflect/CMakeLists.txt"
#endif

/* Exact float comparison (EXPECT_EQ takes integers and pointers only). */
#define EXPECT_FEQ(a, b) EXPECT_TRUE((a) == (b))

/* ---- Memory writer / reader ---------------------------------------------------------------- */

#define TEXT_CAP (256u * 1024u)

typedef struct membuf {
    char *buf;
    size_t len;
} membuf;

static de_result mem_write(void *user, const void *data, size_t bytes) {
    membuf *m = (membuf *)user;
    if (m->len + bytes + 1 > TEXT_CAP)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "test buffer full");
    memcpy(m->buf + m->len, data, bytes);
    m->len += bytes;
    m->buf[m->len] = '\0';
    return DE_OK;
}

typedef struct memsrc {
    const char *p;
    size_t len, pos;
    size_t chunk; /* most bytes per read call (exercises the reader's loop) */
} memsrc;

static de_result mem_read(void *user, void *data, size_t bytes, size_t *read) {
    memsrc *s = (memsrc *)user;
    size_t n = s->len - s->pos;
    if (n > bytes)
        n = bytes;
    if (s->chunk != 0 && n > s->chunk)
        n = s->chunk;
    memcpy(data, s->p + s->pos, n);
    s->pos += n;
    *read = n;
    return DE_OK;
}

/* Serializes into a fresh heap buffer (free with free()). */
static char *to_text(const de_type_registry *r, de_type_id t, const void *obj,
                     const de_ser_options *opt) {
    membuf m;
    m.buf = (char *)calloc(TEXT_CAP, 1);
    m.len = 0;
    de_writer w = {mem_write, &m};
    de_result res = de_serialize_object(r, t, obj, opt, &w);
    if (res != DE_OK) {
        const de_error *e = de_error_last();
        printf("serialize failed: %s\n", e ? e->message : "?");
        free(m.buf);
        return NULL;
    }
    return m.buf;
}

static de_result from_text(const de_type_registry *r, de_type_id t, void *obj,
                           const de_ser_options *opt, const char *text) {
    memsrc s = {text, strlen(text), 0, 7};
    de_reader rd = {mem_read, &s};
    return de_deserialize_object(r, t, obj, opt, &rd);
}

static char *read_file(const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", DE_REFLECT_TEST_DATA_DIR, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    char *buf = (char *)calloc(TEXT_CAP, 1);
    size_t n = fread(buf, 1, TEXT_CAP - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Compares `actual` with data/<name>; on a mismatch writes text_actual_<name>. */
static void expect_golden(const char *actual, const char *name) {
    char *want = read_file(name);
    ASSERT_TRUE(want != NULL);
    if (strcmp(want, actual) != 0) {
        char path[256];
        snprintf(path, sizeof path, "text_actual_%s", name);
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            fwrite(actual, 1, strlen(actual), f);
            fclose(f);
        }
        printf("golden mismatch for %s: actual text written to %s\n", name, path);
        EXPECT_STREQ(actual, want);
    }
    free(want);
}

/* ---- Log capture --------------------------------------------------------------------------- */

#define CAP_MAX 64

typedef struct capture {
    u32 count; /* warnings in category "reflect" */
    u32 other;
    char msgs[CAP_MAX][200];
} capture;

static capture g_cap;
static de_log_category g_reflect_cat;

static void capture_sink(const de_log_record *rec, void *user) {
    (void)user;
    if (rec->level != DE_LOG_WARN || rec->category.id != g_reflect_cat.id) {
        g_cap.other++;
        return;
    }
    if (g_cap.count < CAP_MAX) {
        size_t n = rec->message.len < 199 ? rec->message.len : 199;
        memcpy(g_cap.msgs[g_cap.count], rec->message.ptr, n);
        g_cap.msgs[g_cap.count][n] = '\0';
    }
    g_cap.count++;
}

/* Warnings so far (flushes the log thread first). */
static u32 warnings(void) {
    de_log_flush();
    return g_cap.count;
}

static bool warned_about(const char *needle) {
    de_log_flush();
    for (u32 i = 0; i < g_cap.count && i < CAP_MAX; ++i)
        if (strstr(g_cap.msgs[i], needle) != NULL)
            return true;
    return false;
}

/* ---- Fixture ------------------------------------------------------------------------------- */

typedef struct text_fx {
    de_type_registry *r;
    test_type_ids ids;
} text_fx;

static void text_fx_setup(text_fx *f) {
    memset(&g_cap, 0, sizeof g_cap);
    g_reflect_cat = de_log_register_category("reflect", DE_LOG_INFO);
    de_log_desc d;
    memset(&d, 0, sizeof d);
    d.no_console_sink = true;
    ASSERT_EQ(de_log_init(&d), DE_OK);
    de_log_add_sink(capture_sink, NULL);
    ASSERT_EQ(de_type_registry_create(NULL, &f->r), DE_OK);
    ASSERT_EQ(test_types_register(f->r, &f->ids), DE_OK);
}

static void text_fx_teardown(text_fx *f) {
    de_type_registry_destroy(f->r);
    de_log_shutdown();
}

/* ---- Values -------------------------------------------------------------------------------- */

static void fill_all_kinds(TAllKinds *a) {
    memset(a, 0, sizeof *a);
    a->b = true;
    a->i8v = -128;
    a->i16v = -12345;
    a->i32v = -2000000000;
    a->i64v = INT64_MIN;
    a->u8v = 255;
    a->u16v = 65535;
    a->u32v = 4000000000u;
    a->u64v = UINT64_MAX;
    a->f32v = 0.1f;
    a->f64v = 1.0 / 3.0;
    a->v2.x = 1.0f;
    a->v2.y = -2.0f;
    a->v3.x = 0.5f;
    a->v3.y = 0.25f;
    a->v3.z = -0.0f;
    a->v4.x = 1e-45f; /* the smallest denormal */
    a->v4.y = FLT_MAX;
    a->v4.z = -FLT_MAX;
    a->v4.w = FLT_MIN;
    a->dv3.x = 1e300;
    a->dv3.y = -4.9406564584124654e-324; /* the smallest f64 denormal */
    a->dv3.z = 123456789.125;
    a->q.z = 0.70710677f;
    a->q.w = 0.70710677f;
    for (int i = 0; i < 16; ++i)
        a->m4.m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    a->m4.m[12] = 10.0f;
    a->m4.m[13] = 20.5f;
    a->m4.m[14] = -30.0f;
    a->color.x = 1.0f;
    a->color.y = 0.5f;
    a->color.z = 0.25f;
    a->color.w = 1.0f;
    a->name = de_name_from("player");
    strcpy(a->str, "say \"hi\"\\\n\t\xC3\xBC\x01");
    a->entity = 0x0000000100000002ull;
    a->mesh.id = 7;
    a->mode = (u8)TMODE_NEG;
    a->layers = TLAYER_GROUND | TLAYER_AIR | 0x100u;
    a->vec.x = 1.0f;
    a->vec.y = 2.0f;
    a->vec.z = 3.0f;
    a->samples[0] = INFINITY;
    a->samples[1] = -INFINITY;
    a->samples[2] = 0.0f;
    a->samples[3] = -1.5f;
    a->items[0].id = 1;
    a->items[0].count = -1;
    a->items[1].id = 42;
    a->items[1].count = 7;
}

/* ---- text.roundtrip_all_kinds -------------------------------------------------------------- */

TEST_F(text, roundtrip_all_kinds, text_fx) {
    TAllKinds a, b;
    fill_all_kinds(&a);
    char *t1 = to_text(f->r, f->ids.all_kinds, &a, NULL);
    ASSERT_TRUE(t1 != NULL);

    memset(&b, 0, sizeof b);
    ASSERT_EQ(from_text(f->r, f->ids.all_kinds, &b, NULL, t1), DE_OK);
    EXPECT_MEMEQ(&a, &b, sizeof a);
    EXPECT_EQ(warnings(), 0u);

    char *t2 = to_text(f->r, f->ids.all_kinds, &b, NULL);
    ASSERT_TRUE(t2 != NULL);
    EXPECT_STREQ(t2, t1);
#if DE_NAME_LOOKUP
    /* The NAME field is written as "player" only where the build keeps names. */
    expect_golden(t1, "text_all_kinds.toml");
#endif
    free(t1);
    free(t2);
}

/* Every kind also round-trips from the checked-in file (the file tomllib checks). */
TEST_F(text, golden_all_kinds_reads_back, text_fx) {
    char *want = read_file("text_all_kinds.toml");
    ASSERT_TRUE(want != NULL);
    TAllKinds a, b;
    fill_all_kinds(&a);
    memset(&b, 0, sizeof b);
    ASSERT_EQ(from_text(f->r, f->ids.all_kinds, &b, NULL, want), DE_OK);
    EXPECT_MEMEQ(&a, &b, sizeof a);
    free(want);
}

/* ---- text.float_exact ---------------------------------------------------------------------- */

typedef struct TFloats {
    f32 f[64];
    f64 d[64];
} TFloats;

static const de_field_info k_tfloats_fields[] = {
    DE_FIELD_EX(TFloats, f, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, "f32", 64}),
    DE_FIELD_EX(TFloats, d, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, "f64", 64}),
};

static bool same_f32(f32 a, f32 b) {
    if (isnan(a) || isnan(b))
        return isnan(a) && isnan(b);
    u32 x, y;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    return x == y;
}

static bool same_f64(f64 a, f64 b) {
    if (isnan(a) || isnan(b))
        return isnan(a) && isnan(b);
    u64 x, y;
    memcpy(&x, &a, 8);
    memcpy(&y, &b, 8);
    return x == y;
}

/* Writes `in`, reads it back into a zeroed object and checks every value bit for bit (NaN
 * payload aside); returns the text. */
static char *check_floats(text_fx *f, de_type_id t, const TFloats *in) {
    char *text = to_text(f->r, t, in, NULL);
    if (text == NULL)
        FAIL("serialize failed"); /* _Noreturn: a return after it is C4702 on MSVC */
    TFloats out;
    memset(&out, 0, sizeof out);
    EXPECT_EQ(from_text(f->r, t, &out, NULL, text), DE_OK);
    for (int i = 0; i < 64; ++i) {
        if (!same_f32(in->f[i], out.f[i])) {
            u32 bits;
            memcpy(&bits, &in->f[i], 4); /* not a pointer cast: GCC -Wstrict-aliasing */
            printf("f32 %d: %.9g (0x%08x) came back as %.9g\n", i, (double)in->f[i], (unsigned)bits,
                   (double)out.f[i]);
            FAIL("f32 did not round-trip");
        }
        if (!same_f64(in->d[i], out.d[i])) {
            printf("f64 %d: %.17g came back as %.17g\n", i, in->d[i], out.d[i]);
            FAIL("f64 did not round-trip");
        }
    }
    return text;
}

static f32 f32_bits(u32 b) {
    f32 v;
    memcpy(&v, &b, 4);
    return v;
}

static f64 f64_bits(u64 b) {
    f64 v;
    memcpy(&v, &b, 8);
    return v;
}

TEST_F(text, float_exact, text_fx) {
    de_type_id t = de_type_register(f->r, &DE_TYPE_DESC(TFloats, 1, k_tfloats_fields));
    ASSERT_TRUE(t.id != 0);

    static const f32 k_f32[] = {
        0.0f,        -0.0f,       1e-45f,      -1e-45f,  1.17549421e-38f /* max denormal */,
        FLT_MIN,     FLT_MAX,     -FLT_MAX,    INFINITY, -INFINITY,
        NAN,         0.1f,        1.0f,        -2.0f,    1.0f / 3.0f,
        16777216.0f, 16777217.0f, 3.14159274f, 1e10f,    1e-10f};
    static const f64 k_f64[] = {0.0,
                                -0.0,
                                4.9406564584124654e-324,
                                -4.9406564584124654e-324,
                                2.2250738585072009e-308 /* max denormal */,
                                DBL_MIN,
                                DBL_MAX,
                                -DBL_MAX,
                                INFINITY,
                                -INFINITY,
                                NAN,
                                0.1,
                                1.0,
                                -2.0,
                                1.0 / 3.0,
                                9007199254740993.0,
                                3.141592653589793,
                                1e22,
                                1e23,
                                5e-324};
    TFloats in;
    memset(&in, 0, sizeof in);
    for (u32 i = 0; i < TEST_COUNT_OF(k_f32); ++i)
        in.f[i] = k_f32[i];
    for (u32 i = 0; i < TEST_COUNT_OF(k_f64); ++i)
        in.d[i] = k_f64[i];
    char *text = check_floats(f, t, &in);
    ASSERT_TRUE(text != NULL);
    /* The shortest forms, in TOML syntax. */
    EXPECT_TRUE(
        strstr(text,
               "f = [0.0, -0.0, 1e-45, -1e-45, 1.1754942e-38, 1.1754944e-38, "
               "3.4028235e38, -3.4028235e38, inf, -inf, nan, 0.1, 1.0, -2.0, "
               "0.33333334, 16777216.0, 16777216.0, 3.1415927, 10000000000.0, 1e-10, 0.0") != NULL);
    EXPECT_TRUE(strstr(text, "d = [0.0, -0.0, 5e-324, -5e-324, 2.225073858507201e-308, "
                             "2.2250738585072014e-308, 1.7976931348623157e308, "
                             "-1.7976931348623157e308, inf, -inf, nan, 0.1, 1.0, -2.0, "
                             "0.3333333333333333, 9007199254740992.0, 3.141592653589793, 1e22, "
                             "1e23, 5e-324, 0.0") != NULL);

    /* The text does not depend on the C locale (a decimal-comma locale, where one exists). */
    if (setlocale(LC_NUMERIC, "de_DE.UTF-8") != NULL || setlocale(LC_NUMERIC, "de_DE") != NULL ||
        setlocale(LC_NUMERIC, "fr_FR.UTF-8") != NULL) {
        char *text2 = to_text(f->r, t, &in, NULL);
        setlocale(LC_NUMERIC, "C");
        ASSERT_TRUE(text2 != NULL);
        EXPECT_STREQ(text2, text);
        free(text2);
    }
    free(text);

    /* Random bit patterns: every class of value, many exponents. */
    de_rng rng = de_rng_seed(295, 1);
    for (int round = 0; round < 300; ++round) {
        for (int i = 0; i < 64; ++i) {
            in.f[i] = f32_bits(de_rng_u32(&rng));
            u64 hi = de_rng_u32(&rng), lo = de_rng_u32(&rng);
            in.d[i] = f64_bits((hi << 32) | lo);
        }
        char *tx = check_floats(f, t, &in);
        free(tx);
    }
    EXPECT_EQ(warnings(), 0u);
}

/* ---- text.golden_output -------------------------------------------------------------------- */

static void fill_transform(TTransform *t, f64 x) {
    memset(t, 0, sizeof *t);
    t->position.x = x;
    t->position.y = -2.0;
    t->position.z = 1000000.25;
    t->rotation.x = 0.0f;
    t->rotation.y = 0.0f;
    t->rotation.z = 0.38268343f;
    t->rotation.w = 0.92387953f;
    t->scale.x = 1.0f;
    t->scale.y = 2.5f;
    t->scale.z = 0.001f;
}

TEST_F(text, golden_output, text_fx) {
    TTransform t;
    fill_transform(&t, 1.5);
    char *text = to_text(f->r, f->ids.transform, &t, NULL);
    ASSERT_TRUE(text != NULL);
    expect_golden(text, "text_golden_transform.toml");

    TTransform back;
    memset(&back, 0, sizeof back);
    ASSERT_EQ(from_text(f->r, f->ids.transform, &back, NULL, text), DE_OK);
    EXPECT_MEMEQ(&back, &t, sizeof t);
    free(text);
}

/* Nested structs as dotted keys, arrays of structs one per line, enums by name. */
TEST_F(text, golden_nested, text_fx) {
    TEntityData e;
    memset(&e, 0, sizeof e);
    fill_transform(&e.transform, 4.0);
    e.inventory.count = 3;
    for (u32 i = 0; i < 8; ++i) {
        e.inventory.items[i].id = i < 3 ? 100 + i : 0;
        e.inventory.items[i].count = i < 3 ? (i32)(i * 10) : 0;
    }
    e.mode = TMODE_RUN;
    char *text = to_text(f->r, f->ids.entity_data, &e, NULL);
    ASSERT_TRUE(text != NULL);
    expect_golden(text, "text_golden_entity.toml");
    TEntityData back;
    memset(&back, 0, sizeof back);
    ASSERT_EQ(from_text(f->r, f->ids.entity_data, &back, NULL, text), DE_OK);
    EXPECT_MEMEQ(&back, &e, sizeof e);
    free(text);
}

/* ---- text.unknown_field_warns -------------------------------------------------------------- */

TEST_F(text, unknown_field_warns, text_fx) {
    const char *text = "__type = \"TTransform\"\n"
                       "__version = 1\n"
                       "position = [1.0, 2, 3.0]\n" /* an integer is fine for a float */
                       "bogus = { a = 1, b = [2, 3] }\n"
                       "rotation = [0.0, 0.0, 0.0, 1.0]\n"
                       "scale = [2.0, 2.0, 2.0]\n";
    TTransform t;
    memset(&t, 0, sizeof t);
    ASSERT_EQ(from_text(f->r, f->ids.transform, &t, NULL, text), DE_OK);
    EXPECT_EQ(warnings(), 1u);
    EXPECT_TRUE(warned_about("bogus"));
    EXPECT_TRUE(warned_about("line 4"));
    EXPECT_FEQ(t.position.x, 1.0);
    EXPECT_FEQ(t.position.y, 2.0);
    EXPECT_FEQ(t.position.z, 3.0);
    EXPECT_FEQ(t.rotation.w, 1.0f);
    EXPECT_FEQ(t.scale.z, 2.0f);
}

/* Fields the text does not name keep their constructed values. */
TEST_F(text, missing_fields_keep_defaults, text_fx) {
    TTransform t;
    fill_transform(&t, 9.0);
    ASSERT_EQ(from_text(f->r, f->ids.transform, &t, NULL, "scale = [3.0, 3.0, 3.0]\n"), DE_OK);
    EXPECT_FEQ(t.position.x, 9.0);
    EXPECT_FEQ(t.rotation.w, 0.92387953f);
    EXPECT_FEQ(t.scale.x, 3.0f);
    EXPECT_EQ(warnings(), 0u);
}

/* ---- Versions ------------------------------------------------------------------------------ */

typedef struct TPlayer {
    i32 health;
    f32 speed;
    char tag[16];
} TPlayer;

static const de_field_info k_tplayer_fields[] = {
    DE_FIELD(TPlayer, health, DE_FK_I32, DE_FIELD_DEFAULT),
    DE_FIELD(TPlayer, speed, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(TPlayer, tag, DE_FK_STRING, DE_FIELD_DEFAULT),
};

static const char k_player_v1[] = "__type = \"TPlayer\"\n"
                                  "__version = 1\n"
                                  "hp = 75\n"
                                  "speed = 2.5\n"
                                  "tag = \"hero\"\n"
                                  "pos = { x = 1.5, y = -2 }\n"
                                  "ids = [\"0x0000000000000010\", \"ab\"]\n"
                                  "owner = \"0x00000000000000ff\"\n";

typedef struct migrate_log {
    u32 calls;
    u32 from_version;
    bool saw_hp, saw_speed, saw_tag, saw_pos_x, saw_ids, saw_owner;
    bool drop;
} migrate_log;

static migrate_log g_mig;

static const de_field_info *old_field(const de_field_info *fields, u32 n, const char *name) {
    for (u32 i = 0; i < n; ++i)
        if (strcmp(fields[i].name, name) == 0)
            return &fields[i];
    return NULL;
}

static bool player_migrate(void *obj, u32 from_version, const void *old_data,
                           const de_field_info *old_fields, u32 old_field_count) {
    TPlayer *p = (TPlayer *)obj;
    const u8 *data = (const u8 *)old_data;
    g_mig.calls++;
    g_mig.from_version = from_version;
    const de_field_info *hp = old_field(old_fields, old_field_count, "hp");
    if (hp != NULL && hp->kind == DE_FK_I64 && hp->size == 8) {
        i64 v;
        memcpy(&v, data + hp->offset, 8);
        p->health = (i32)v; /* renamed: hp -> health */
        g_mig.saw_hp = true;
    }
    const de_field_info *sp = old_field(old_fields, old_field_count, "speed");
    if (sp != NULL && sp->kind == DE_FK_F64) {
        f64 v;
        memcpy(&v, data + sp->offset, 8);
        g_mig.saw_speed = v == 2.5;
    }
    const de_field_info *tag = old_field(old_fields, old_field_count, "tag");
    g_mig.saw_tag = tag != NULL && tag->kind == DE_FK_STRING && tag->size == 5 &&
                    strcmp((const char *)data + tag->offset, "hero") == 0;
    const de_field_info *px = old_field(old_fields, old_field_count, "pos.x");
    if (px != NULL && px->kind == DE_FK_F64) {
        f64 v;
        memcpy(&v, data + px->offset, 8);
        g_mig.saw_pos_x = v == 1.5 && old_field(old_fields, old_field_count, "pos.y") != NULL;
    }
    const de_field_info *ids = old_field(old_fields, old_field_count, "ids");
    g_mig.saw_ids =
        ids != NULL && ids->kind == DE_FK_FIXED_ARRAY && ids->attrs.nested_type != NULL &&
        strcmp(ids->attrs.nested_type, "string") == 0 && ids->attrs.array_count == 2 &&
        ids->size == 2u * 19u && strcmp((const char *)data + ids->offset + 19, "ab") == 0;
    const de_field_info *owner = old_field(old_fields, old_field_count, "owner");
    if (owner != NULL && owner->kind == DE_FK_U64) {
        u64 v;
        memcpy(&v, data + owner->offset, 8);
        g_mig.saw_owner = v == 0xff;
    }
    for (u32 i = 0; i < old_field_count; ++i) /* offsets are 8-byte aligned */
        if (old_fields[i].offset % 8u != 0)
            return false;
    return !g_mig.drop;
}

static de_type_id register_player(de_type_registry *r, bool with_hook) {
    de_type_desc d = DE_TYPE_DESC(TPlayer, 2, k_tplayer_fields);
    if (with_hook)
        d.hooks.migrate = player_migrate;
    return de_type_register(r, &d);
}

TEST_F(text, migrate_rename, text_fx) {
    de_type_id t = register_player(f->r, true);
    ASSERT_TRUE(t.id != 0);
    memset(&g_mig, 0, sizeof g_mig);
    TPlayer p;
    memset(&p, 0, sizeof p);
    p.health = -1;
    ASSERT_EQ(from_text(f->r, t, &p, NULL, k_player_v1), DE_OK);
    EXPECT_EQ(g_mig.calls, 1u);
    EXPECT_EQ(g_mig.from_version, 1u);
    EXPECT_EQ(p.health, 75);     /* by the hook */
    EXPECT_FEQ(p.speed, 2.5f);   /* matched by name before the hook */
    EXPECT_STREQ(p.tag, "hero"); /* matched by name */
    EXPECT_TRUE(g_mig.saw_hp);
    EXPECT_TRUE(g_mig.saw_speed);
    EXPECT_TRUE(g_mig.saw_tag);
    EXPECT_TRUE(g_mig.saw_pos_x);
    EXPECT_TRUE(g_mig.saw_ids);
    EXPECT_TRUE(g_mig.saw_owner);
    /* The old fields go to the hook: no unknown-field warnings. */
    EXPECT_EQ(warnings(), 0u);

    /* Writing it again gives the current version. */
    char *text = to_text(f->r, t, &p, NULL);
    ASSERT_TRUE(text != NULL);
    EXPECT_STREQ(text, "__type = \"TPlayer\"\n__version = 2\nhealth = 75\nspeed = 2.5\n"
                       "tag = \"hero\"\n");
    free(text);

    /* A hook that returns false drops the object. */
    g_mig.drop = true;
    memset(&p, 0, sizeof p);
    EXPECT_EQ(from_text(f->r, t, &p, NULL, k_player_v1), DE_ERR_VERSION_MISMATCH);
    EXPECT_EQ(g_mig.calls, 2u);
}

TEST_F(text, no_migrate_best_effort, text_fx) {
    de_type_id t = register_player(f->r, false);
    ASSERT_TRUE(t.id != 0);
    static const char k_text[] = "__type = \"TPlayer\"\n"
                                 "__version = 1\n"
                                 "hp = 75\n"
                                 "speed = 2.5\n"
                                 "tag = 12\n"; /* the type changed: skipped */
    TPlayer p;
    memset(&p, 0, sizeof p);
    p.health = -1;
    strcpy(p.tag, "none");
    ASSERT_EQ(from_text(f->r, t, &p, NULL, k_text), DE_OK);
    EXPECT_EQ(p.health, -1); /* nothing maps hp */
    EXPECT_FEQ(p.speed, 2.5f);
    EXPECT_STREQ(p.tag, "none");
    EXPECT_EQ(warnings(), 3u); /* the version note, unknown 'hp', the skipped 'tag' */
    EXPECT_TRUE(warned_about("no migrate hook"));
    EXPECT_TRUE(warned_about("field hp: unknown field"));
    EXPECT_TRUE(warned_about("field tag: expected a string"));
}

TEST_F(text, future_version_rejected, text_fx) {
    de_type_id t = register_player(f->r, true);
    ASSERT_TRUE(t.id != 0);
    memset(&g_mig, 0, sizeof g_mig);
    TPlayer p;
    memset(&p, 0, sizeof p);
    de_error_clear();
    EXPECT_EQ(from_text(f->r, t, &p, NULL, "__type = \"TPlayer\"\n__version = 3\nhealth = 1\n"),
              DE_ERR_VERSION_MISMATCH);
    const de_error *e = de_error_last();
    ASSERT_TRUE(e != NULL);
    EXPECT_TRUE(strstr(e->message, "newer") != NULL);
    EXPECT_EQ(p.health, 0);
    EXPECT_EQ(g_mig.calls, 0u);

    /* A different __type is a parse error. */
    EXPECT_EQ(from_text(f->r, t, &p, NULL, "__type = \"TTransform\"\n__version = 2\n"),
              DE_ERR_PARSE);
    /* A missing __version means the current version. */
    EXPECT_EQ(from_text(f->r, t, &p, NULL, "health = 4\n"), DE_OK);
    EXPECT_EQ(p.health, 4);
    EXPECT_EQ(g_mig.calls, 0u);
}

/* ---- text.entity_asset_callbacks ----------------------------------------------------------- */

#define SCENE_XOR 0x5a5a000000000000ull

typedef struct ctx_state {
    u32 e2s, s2e, a2i, i2a;
    u32 bad_user;
    u32 bad_type;
    u64 mesh_id;
} ctx_state;

static ctx_state g_ctx_state;

static void check_user(void *user) {
    if (user != &g_ctx_state)
        g_ctx_state.bad_user++;
}

static u64 cb_e2s(u64 bits, void *user) {
    check_user(user);
    g_ctx_state.e2s++;
    return bits ^ SCENE_XOR;
}

static u64 cb_s2e(u64 id, void *user) {
    check_user(user);
    g_ctx_state.s2e++;
    return id ^ SCENE_XOR;
}

static u64 cb_a2i(u32 handle, const char *type, void *user) {
    check_user(user);
    g_ctx_state.a2i++;
    if (type == NULL || strcmp(type, "mesh") != 0)
        g_ctx_state.bad_type++;
    return handle == 7 ? g_ctx_state.mesh_id : 0;
}

static u32 cb_i2a(u64 id, const char *type, void *user) {
    check_user(user);
    g_ctx_state.i2a++;
    if (type == NULL || strcmp(type, "mesh") != 0)
        g_ctx_state.bad_type++;
    return id == g_ctx_state.mesh_id ? 7u : 0u;
}

TEST_F(text, entity_asset_callbacks, text_fx) {
    memset(&g_ctx_state, 0, sizeof g_ctx_state);
    g_ctx_state.mesh_id = de_name_from("player/mesh").hash; /* interned where the build can */
    de_ser_context ctx = {cb_e2s, cb_s2e, cb_a2i, cb_i2a, &g_ctx_state};
    de_ser_options opt = {DE_SER_TEXT, 0, &ctx};

    TAllKinds a, b;
    fill_all_kinds(&a);
    char *text = to_text(f->r, f->ids.all_kinds, &a, &opt);
    ASSERT_TRUE(text != NULL);
    EXPECT_EQ(g_ctx_state.e2s, 1u);
    EXPECT_EQ(g_ctx_state.a2i, 1u);
    EXPECT_TRUE(strstr(text, "\nentity = \"0x5a5a000100000002\"\n") != NULL);
    char want[128];
    snprintf(want, sizeof want, "\nmesh = { id = \"0x%016llx\"",
             (unsigned long long)g_ctx_state.mesh_id);
    EXPECT_TRUE(strstr(text, want) != NULL);
#if DE_NAME_LOOKUP
    EXPECT_TRUE(strstr(text, ", path = \"player/mesh\" }\n") != NULL);
#endif

    memset(&b, 0, sizeof b);
    ASSERT_EQ(from_text(f->r, f->ids.all_kinds, &b, &opt, text), DE_OK);
    EXPECT_EQ(g_ctx_state.s2e, 1u);
    EXPECT_EQ(g_ctx_state.i2a, 1u);
    EXPECT_EQ(b.entity, a.entity);
    EXPECT_EQ(b.mesh.id, 7u);
    EXPECT_MEMEQ(&a, &b, sizeof a);
    free(text);

    /* An asset given by path only: its ID is the hash of the path. */
    memset(&b, 0, sizeof b);
    ASSERT_EQ(from_text(f->r, f->ids.all_kinds, &b, &opt, "mesh = \"player/mesh\"\nentity = 5\n"),
              DE_OK);
    EXPECT_EQ(b.mesh.id, 7u);
    EXPECT_EQ(b.entity, 5ull ^ SCENE_XOR);
    ASSERT_EQ(from_text(f->r, f->ids.all_kinds, &b, &opt, "mesh = { path = \"player/mesh\" }\n"),
              DE_OK);
    EXPECT_EQ(b.mesh.id, 7u);
    EXPECT_EQ(g_ctx_state.bad_user, 0u);
    EXPECT_EQ(g_ctx_state.bad_type, 0u);

    /* No context, or a context without callbacks: raw values. */
    de_ser_context empty;
    memset(&empty, 0, sizeof empty);
    de_ser_options raw = {DE_SER_TEXT, 0, &empty};
    for (int pass = 0; pass < 2; ++pass) {
        char *t2 = to_text(f->r, f->ids.all_kinds, &a, pass == 0 ? NULL : &raw);
        ASSERT_TRUE(t2 != NULL);
        EXPECT_TRUE(strstr(t2, "\nentity = \"0x0000000100000002\"\n") != NULL);
        EXPECT_TRUE(strstr(t2, "\nmesh = { id = \"0x0000000000000007\" }\n") != NULL);
        memset(&b, 0, sizeof b);
        ASSERT_EQ(from_text(f->r, f->ids.all_kinds, &b, pass == 0 ? NULL : &raw, t2), DE_OK);
        EXPECT_MEMEQ(&a, &b, sizeof a);
        free(t2);
    }
    /* A raw read of an ID that does not fit a handle fails. */
    EXPECT_EQ(from_text(f->r, f->ids.all_kinds, &b, NULL, "mesh = { id = \"0x0000000100000000\" }"),
              DE_ERR_PARSE);
}

/* ---- text.required_flags_filter ------------------------------------------------------------ */

typedef struct TFlagged {
    i32 saved;
    i32 replicated;
    i32 editor_only;
    TVec v;
} TFlagged;

static const de_field_info k_tflagged_fields[] = {
    DE_FIELD(TFlagged, saved, DE_FK_I32, DE_FIELD_SERIALIZE),
    DE_FIELD(TFlagged, replicated, DE_FK_I32, DE_FIELD_SERIALIZE | DE_FIELD_REPLICATE),
    DE_FIELD(TFlagged, editor_only, DE_FK_I32, DE_FIELD_EDITOR_HIDDEN),
    DE_FIELD_EX(TFlagged, v, DE_FK_STRUCT, DE_FIELD_SERIALIZE, {0, 0, 0, 0, "TVec", 0}),
};

TEST_F(text, required_flags_filter, text_fx) {
    de_type_id t = de_type_register(f->r, &DE_TYPE_DESC(TFlagged, 1, k_tflagged_fields));
    ASSERT_TRUE(t.id != 0);
    TFlagged x = {1, 2, 3, {4.0f, 5.0f, 6.0f}};

    de_ser_options all = {DE_SER_TEXT, 0, NULL};
    char *text = to_text(f->r, t, &x, &all);
    ASSERT_TRUE(text != NULL);
    EXPECT_STREQ(text, "__type = \"TFlagged\"\n__version = 1\nsaved = 1\nreplicated = 2\n"
                       "editor_only = 3\nv.x = 4.0\nv.y = 5.0\nv.z = 6.0\n");
    free(text);

    de_ser_options ser = {DE_SER_TEXT, DE_FIELD_SERIALIZE, NULL};
    text = to_text(f->r, t, &x, &ser);
    ASSERT_TRUE(text != NULL);
    EXPECT_STREQ(text, "__type = \"TFlagged\"\n__version = 1\nsaved = 1\nreplicated = 2\n"
                       "v.x = 4.0\nv.y = 5.0\nv.z = 6.0\n");
    free(text);

    /* Every required bit must be set; TVec's fields lack REPLICATE, so v is empty. */
    de_ser_options rep = {DE_SER_TEXT, DE_FIELD_SERIALIZE | DE_FIELD_REPLICATE, NULL};
    text = to_text(f->r, t, &x, &rep);
    ASSERT_TRUE(text != NULL);
    EXPECT_STREQ(text, "__type = \"TFlagged\"\n__version = 1\nreplicated = 2\n");
    free(text);

    /* Reading applies the same filter: a known field that is filtered out is left alone, and is
     * not "unknown". */
    TFlagged y = {0, 0, 0, {0, 0, 0}};
    ASSERT_EQ(
        from_text(f->r, t, &y, &ser, "saved = 10\nreplicated = 20\neditor_only = 30\nv.y = 7.0\n"),
        DE_OK);
    EXPECT_EQ(y.saved, 10);
    EXPECT_EQ(y.replicated, 20);
    EXPECT_EQ(y.editor_only, 0);
    EXPECT_FEQ(y.v.y, 7.0f);
    EXPECT_EQ(warnings(), 0u);
}

/* ---- text.doc_multi_objects ---------------------------------------------------------------- */

TEST_F(text, doc_multi_objects, text_fx) {
    membuf m;
    m.buf = (char *)calloc(TEXT_CAP, 1);
    m.len = 0;
    de_writer w = {mem_write, &m};
    de_text_doc_writer doc;
    ASSERT_EQ(de_text_doc_begin(&doc, &w), DE_OK);
    TTransform t0, t1;
    fill_transform(&t0, 1.0);
    fill_transform(&t1, -1.0);
    t1.scale.x = 4.0f;
    TItem item = {9, -9};
    ASSERT_EQ(de_text_doc_write_object(&doc, "entity", f->r, f->ids.transform, &t0, NULL), DE_OK);
    ASSERT_EQ(de_text_doc_write_object(&doc, "item", f->r, f->ids.item, &item, NULL), DE_OK);
    ASSERT_EQ(de_text_doc_write_object(&doc, "entity", f->r, f->ids.transform, &t1, NULL), DE_OK);
    EXPECT_EQ(de_text_doc_write_object(&doc, "bad key", f->r, f->ids.item, &item, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(doc.object_count, 3u);
    ASSERT_EQ(de_text_doc_end(&doc), DE_OK);
    expect_golden(m.buf, "text_doc_multi.toml");

    de_text_doc *d = NULL;
    ASSERT_EQ(de_text_doc_parse(NULL, m.buf, m.len, "multi.toml", &d), DE_OK);
    EXPECT_EQ(de_text_doc_count(d, "entity"), 2u);
    EXPECT_EQ(de_text_doc_count(d, "item"), 1u);
    EXPECT_EQ(de_text_doc_count(d, "missing"), 0u);
    EXPECT_EQ(de_text_doc_count(d, NULL), 1u);
    EXPECT_STREQ(de_text_doc_type(d, "entity", 1), "TTransform");
    EXPECT_STREQ(de_text_doc_type(d, "item", 0), "TItem");
    EXPECT_TRUE(de_text_doc_type(d, "entity", 2) == NULL);
    TTransform b0, b1;
    TItem bi;
    memset(&b0, 0, sizeof b0);
    memset(&b1, 0, sizeof b1);
    memset(&bi, 0, sizeof bi);
    ASSERT_EQ(de_text_doc_read_object(d, "entity", 0, f->r, f->ids.transform, &b0, NULL), DE_OK);
    ASSERT_EQ(de_text_doc_read_object(d, "entity", 1, f->r, f->ids.transform, &b1, NULL), DE_OK);
    ASSERT_EQ(de_text_doc_read_object(d, "item", 0, f->r, f->ids.item, &bi, NULL), DE_OK);
    EXPECT_MEMEQ(&b0, &t0, sizeof t0);
    EXPECT_MEMEQ(&b1, &t1, sizeof t1);
    EXPECT_MEMEQ(&bi, &item, sizeof item);
    EXPECT_EQ(de_text_doc_read_object(d, "entity", 2, f->r, f->ids.transform, &b0, NULL),
              DE_ERR_INVALID_ARGUMENT);
    /* The wrong type for an entry: its __type does not match. */
    EXPECT_EQ(de_text_doc_read_object(d, "item", 0, f->r, f->ids.transform, &b0, NULL),
              DE_ERR_PARSE);
    de_text_doc_destroy(d);

    /* A single-object document read through the document API (NULL key). */
    char *single = to_text(f->r, f->ids.item, &item, NULL);
    ASSERT_TRUE(single != NULL);
    ASSERT_EQ(de_text_doc_parse(NULL, single, strlen(single), NULL, &d), DE_OK);
    EXPECT_STREQ(de_text_doc_type(d, NULL, 0), "TItem");
    memset(&bi, 0, sizeof bi);
    ASSERT_EQ(de_text_doc_read_object(d, NULL, 0, f->r, f->ids.item, &bi, NULL), DE_OK);
    EXPECT_MEMEQ(&bi, &item, sizeof item);
    de_text_doc_destroy(d);
    free(single);
    free(m.buf);
}

/* ---- Errors -------------------------------------------------------------------------------- */

static const char *last_message(void) {
    const de_error *e = de_error_last();
    return e ? e->message : "";
}

TEST_F(text, type_mismatch_path_and_line, text_fx) {
    const char *text = "count = 3\n"
                       "items = [\n"
                       "    { id = 1, count = 1 },\n"
                       "    { id = 2, count = 2 },\n"
                       "    { id = 3, count = \"three\" },\n"
                       "]\n";
    TInventory inv;
    memset(&inv, 0, sizeof inv);
    de_error_clear();
    EXPECT_EQ(from_text(f->r, f->ids.inventory, &inv, NULL, text), DE_ERR_PARSE);
    const char *msg = last_message();
    EXPECT_TRUE(strstr(msg, "items[2].count") != NULL);
    EXPECT_TRUE(strstr(msg, "line 5") != NULL);
    EXPECT_TRUE(strstr(msg, "expected an integer, got a string") != NULL);
    if (strstr(msg, "items[2].count") == NULL)
        printf("message: %s\n", msg);

    /* A nested struct path. */
    EXPECT_EQ(from_text(f->r, f->ids.entity_data, &(TEntityData){0}, NULL,
                        "transform.position = [1.0, 2.0, true]\n"),
              DE_ERR_PARSE);
    EXPECT_TRUE(strstr(last_message(), "transform.position[2]") != NULL);
    /* Too many elements, a vector of the wrong length, a float for an integer. */
    EXPECT_EQ(from_text(f->r, f->ids.inventory, &inv, NULL,
                        "items = [{}, {}, {}, {}, {}, {}, {}, {}, {}]\n"),
              DE_ERR_PARSE);
    EXPECT_EQ(from_text(f->r, f->ids.transform, &(TTransform){0}, NULL, "scale = [1.0, 2.0]\n"),
              DE_ERR_PARSE);
    EXPECT_EQ(from_text(f->r, f->ids.inventory, &inv, NULL, "count = 1.5\n"), DE_ERR_PARSE);
    /* Invalid TOML. */
    EXPECT_EQ(from_text(f->r, f->ids.inventory, &inv, NULL, "count = = 1\n"), DE_ERR_PARSE);
}

TEST_F(text, out_of_range, text_fx) {
    TAllKinds a;
    memset(&a, 0, sizeof a);
    static const char *const k_bad[] = {
        "i8v = 128\n",
        "i8v = -129\n",
        "u8v = 256\n",
        "u8v = -1\n",
        "i16v = 40000\n",
        "u16v = 65536\n",
        "i32v = 2147483648\n",
        "u32v = 4294967296\n",
        "u64v = -1\n",
        "u64v = \"0x1g\"\n",
        "f32v = 1e39\n",
        "f32v = -3.5e38\n",
        "mode = 300\n",
        "mode = \"Fly\"\n",
        "layers = [\"Fire\"]\n",
        "str = \"" /* below */,
        "entity = -5\n",
        "v2 = [1.0, \"x\"]\n",
        "b = 1\n",
        "name = 5\n",
    };
    char long_str[80];
    snprintf(long_str, sizeof long_str, "str = \"%s\"\n",
             "0123456789012345678901234567890123456789"); /* 40 bytes: str holds 31 */
    for (u32 i = 0; i < TEST_COUNT_OF(k_bad); ++i) {
        const char *t = strcmp(k_bad[i], "str = \"") == 0 ? long_str : k_bad[i];
        de_error_clear();
        de_result res = from_text(f->r, f->ids.all_kinds, &a, NULL, t);
        if (res != DE_ERR_PARSE)
            printf("input %s gave %s\n", t, de_result_str(res));
        EXPECT_EQ(res, DE_ERR_PARSE);
    }
    /* The edges are fine, and a float just above FLT_MAX that rounds to it too. */
    EXPECT_EQ(from_text(f->r, f->ids.all_kinds, &a, NULL,
                        "i8v = -128\nu8v = 255\nu64v = \"0xffffffffffffffff\"\nf32v = "
                        "3.4028235e38\nmode = -1\nlayers = 7\n"),
              DE_OK);
    EXPECT_EQ(a.i8v, -128);
    EXPECT_EQ(a.u64v, UINT64_MAX);
    EXPECT_FEQ(a.f32v, FLT_MAX);
    EXPECT_EQ(a.mode, 0xFFu);
    EXPECT_EQ(a.layers, 7u);
}

TEST_F(text, write_errors, text_fx) {
    TAllKinds a;
    fill_all_kinds(&a);
    strcpy(a.str, "bad \xC3 utf8"); /* a lone lead byte */
    membuf m;
    m.buf = (char *)calloc(TEXT_CAP, 1);
    m.len = 0;
    de_writer w = {mem_write, &m};
    de_error_clear();
    EXPECT_EQ(de_serialize_object(f->r, f->ids.all_kinds, &a, NULL, &w), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(last_message(), "str") != NULL);
    memset(a.str, 'x', sizeof a.str); /* no NUL */
    EXPECT_EQ(de_serialize_object(f->r, f->ids.all_kinds, &a, NULL, &w), DE_ERR_INVALID_ARGUMENT);
    /* Bad arguments. */
    EXPECT_EQ(de_serialize_object(f->r, (de_type_id){0}, &a, NULL, &w), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_serialize_object(f->r, f->ids.mode, &a, NULL, &w), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_serialize_object(f->r, f->ids.all_kinds, NULL, NULL, &w), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_serialize_object(NULL, f->ids.all_kinds, &a, NULL, &w), DE_ERR_INVALID_ARGUMENT);
    free(m.buf);
}

/* A writer that fails on its n-th call: the failure comes back, nothing crashes. */
static u32 g_fail_after;
static de_result failing_write(void *user, const void *data, size_t bytes) {
    (void)user;
    (void)data;
    (void)bytes;
    if (g_fail_after == 0)
        return DE_FAIL(DE_ERR_IO, "disk full");
    g_fail_after--;
    return DE_OK;
}

TEST_F(text, writer_failure, text_fx) {
    TFloats big;
    memset(&big, 0, sizeof big);
    for (int i = 0; i < 64; ++i) {
        big.f[i] = 1.0f / (f32)(i + 3);
        big.d[i] = 1.0 / (f64)(i + 3);
    }
    de_type_id t = de_type_register(f->r, &DE_TYPE_DESC(TFloats, 1, k_tfloats_fields));
    de_writer w = {failing_write, NULL};
    g_fail_after = 0;
    EXPECT_EQ(de_serialize_object(f->r, t, &big, NULL, &w), DE_ERR_IO);
    /* Output larger than the staging buffer: the second write fails. */
    char *text = to_text(f->r, t, &big, NULL);
    ASSERT_TRUE(text != NULL);
    if (strlen(text) > 4096) {
        g_fail_after = 1;
        EXPECT_EQ(de_serialize_object(f->r, t, &big, NULL, &w), DE_ERR_IO);
    }
    free(text);
}

/* Every allocation of a document parse and of a migrating read fails in turn: each failure is
 * clean (an error, no leak), and the allocator chosen for the document is the only one used. */
TEST_F(text, oom, text_fx) {
    de_type_id t = register_player(f->r, true);
    ASSERT_TRUE(t.id != 0);
    memset(&g_mig, 0, sizeof g_mig);
    for (i64 n = 0;; ++n) {
        test_counting_alloc c;
        test_counting_init(&c);
        c.fail_after = n;
        de_text_doc *d = NULL;
        de_result res = de_text_doc_parse(&c.iface, k_player_v1, strlen(k_player_v1), "oom", &d);
        if (res == DE_OK) {
            TPlayer p;
            memset(&p, 0, sizeof p);
            res = de_text_doc_read_object(d, NULL, 0, f->r, t, &p, NULL);
            if (res == DE_OK)
                EXPECT_EQ(p.health, 75);
            de_text_doc_destroy(d);
        } else {
            EXPECT_TRUE(d == NULL);
            /* tomlc17 reports its own out-of-memory as a parse failure. */
            EXPECT_TRUE(res == DE_ERR_OUT_OF_MEMORY || res == DE_ERR_PARSE);
        }
        EXPECT_EQ(c.live_blocks, 0u);
        EXPECT_EQ(c.size_errors, 0u);
        EXPECT_EQ(c.align_errors, 0u);
        if (res == DE_OK) {
            EXPECT_GT(n, 2);
            break;
        }
        EXPECT_TRUE(res == DE_ERR_OUT_OF_MEMORY || res == DE_ERR_PARSE);
        ASSERT_LT(n, 10000);
    }
}

/* ---- Scene IDs ----------------------------------------------------------------------------- */

static int cmp_u64(const void *a, const void *b) {
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

TEST(text, scene_id_new) {
    enum { N = 4096 };
    u64 *ids = (u64 *)malloc(N * sizeof(u64));
    ASSERT_TRUE(ids != NULL);
    u32 high_bits = 0;
    for (u32 i = 0; i < N; ++i) {
        ids[i] = de_scene_id_new();
        EXPECT_NE(ids[i], 0ull);
        if (ids[i] >> 63)
            high_bits++;
    }
    qsort(ids, N, sizeof(u64), cmp_u64);
    for (u32 i = 1; i < N; ++i)
        EXPECT_NE(ids[i], ids[i - 1]);
    /* 64 random bits: the top bit is set about half the time. */
    EXPECT_GT(high_bits, (u32)N / 4u);
    EXPECT_LT(high_bits, (u32)N * 3u / 4u);
    free(ids);
}

/* ---- Document API arguments ---------------------------------------------------------------- */

TEST(text, doc_arguments) {
    de_text_doc *d = NULL;
    EXPECT_EQ(de_text_doc_parse(NULL, NULL, 3, NULL, &d), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_text_doc_parse(NULL, "a = 1", 5, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    EXPECT_EQ(de_text_doc_parse(NULL, "a = [", 5, "broken.toml", &d), DE_ERR_PARSE);
    EXPECT_TRUE(d == NULL);
    EXPECT_TRUE(strstr(last_message(), "broken.toml") != NULL);
    ASSERT_EQ(de_text_doc_parse(NULL, "", 0, NULL, &d), DE_OK);
    EXPECT_EQ(de_text_doc_count(d, "x"), 0u);
    de_text_doc_destroy(d);
    de_text_doc_destroy(NULL);
    EXPECT_EQ(de_text_doc_count(NULL, "x"), 0u);
    EXPECT_EQ(de_text_doc_begin(NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_text_doc_end(NULL), DE_ERR_INVALID_ARGUMENT);
}
