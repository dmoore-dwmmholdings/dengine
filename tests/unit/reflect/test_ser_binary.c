/* test_ser_binary.c: the binary serializer (DENG-296): round trips of every field kind, the
 * golden byte layout, the memcpy fast path (and its speed), migration through the stored schema,
 * schema mismatches, truncated and corrupt input, the required_flags filter, the entity/asset
 * callbacks and the memory reader/writer. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* strcpy, getenv, fopen */
#endif
#include "de_test.h"
#include "test_types.h"

#include <dengine/de_platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_BIN_TEST_DATA_DIR
#error "DE_BIN_TEST_DATA_DIR must name tests/unit/reflect/data"
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer)
#define BIN_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define BIN_TEST_SANITIZED 1
#endif

/* ---- Fixture and helpers ------------------------------------------------------------------- */

typedef struct bin_fx {
    de_type_registry *r;
    test_type_ids ids;
    de_mem_writer mw;
} bin_fx;

static void bin_fx_setup(bin_fx *f) {
    ASSERT_EQ(de_type_registry_create(NULL, &f->r), DE_OK);
    ASSERT_EQ(test_types_register(f->r, &f->ids), DE_OK);
    de_mem_writer_init(&f->mw, NULL);
}

static void bin_fx_teardown(bin_fx *f) {
    de_mem_writer_free(&f->mw);
    de_type_registry_destroy(f->r);
}

static const de_ser_binary_options k_self = {DE_SER_BINARY_SELF_DESCRIBING, NULL, false};
static const de_ser_binary_options k_compact = {DE_SER_BINARY_COMPACT, NULL, false};

/* Appends one object to `mw`. */
static de_result ser(const de_type_registry *r, de_type_id t, const void *obj,
                     const de_ser_options *opt, const de_ser_binary_options *bopt,
                     de_mem_writer *mw) {
    de_writer w = de_writer_from_mem(mw);
    return de_serialize_binary(r, t, obj, opt, bopt, &w);
}

/* Reads one object from `size` bytes at `data`; `*consumed` (optional) gets the bytes read. */
static de_result deser(const de_type_registry *r, de_type_id t, void *obj,
                       const de_ser_options *opt, const de_ser_binary_options *bopt,
                       const void *data, size_t size, size_t *consumed) {
    de_mem_reader mr;
    de_mem_reader_init(&mr, data, size);
    de_reader rd = de_reader_from_mem(&mr);
    de_result res = de_deserialize_binary(r, t, obj, opt, bopt, &rd);
    if (consumed)
        *consumed = mr.pos;
    return res;
}

/* A reader that hands out at most `max` bytes per call (exercises the read loops). */
typedef struct chunk_reader {
    const u8 *data;
    size_t size, pos, max;
    u32 calls;
} chunk_reader;

static de_result chunk_read(void *user, void *dst, size_t bytes, size_t *read) {
    chunk_reader *c = (chunk_reader *)user;
    size_t n = c->size - c->pos;
    if (n > bytes)
        n = bytes;
    if (n > c->max)
        n = c->max;
    memcpy(dst, c->data + c->pos, n);
    c->pos += n;
    *read = n;
    c->calls++;
    return DE_OK;
}

static void set_f32_bits(void *dst, u32 bits) {
    memcpy(dst, &bits, 4);
}

static void set_f64_bits(void *dst, u64 bits) {
    memcpy(dst, &bits, 8);
}

/* Every kind with a distinct, awkward value (negative numbers, -0, denormals, NaN payloads). */
static void fill_all_kinds(TAllKinds *a) {
    memset(a, 0, sizeof *a);
    a->b = true;
    a->i8v = -5;
    a->i16v = -1234;
    a->i32v = -123456789;
    a->i64v = -1234567890123456789LL;
    a->u8v = 250;
    a->u16v = 65000;
    a->u32v = 4000000000u;
    a->u64v = 0xFEDCBA9876543210ull;
    set_f32_bits(&a->f32v, 0x00000001u);           /* smallest denormal */
    set_f64_bits(&a->f64v, 0x7FF0000000000ABCull); /* NaN with a payload */
    set_f32_bits(&a->v2.x, 0x80000000u);           /* -0 */
    a->v2.y = 3.5f;
    a->v3 = (de_vec3){1.0f, -2.0f, 3.25f};
    a->v4.x = 4.0f;
    a->v4.y = 5.0f;
    a->v4.z = 6.0f;
    set_f32_bits(&a->v4.w, 0x7F800000u); /* +inf */
    a->dv3 = (de_dvec3){1e300, -1e-300, 0.1};
    a->q.x = 0.0f;
    a->q.y = 0.0f;
    a->q.z = 0.70710677f;
    a->q.w = 0.70710677f;
    for (u32 i = 0; i < 16; ++i)
        a->m4.m[i] = (f32)i * 0.5f - 3.0f;
    a->color.x = 0.25f;
    a->color.y = 0.5f;
    a->color.z = 0.75f;
    a->color.w = 1.0f;
    a->name.hash = 0x0123456789ABCDEFull;
    strcpy(a->str, "hello, binary");
    a->entity = 0x0000000100000007ull;
    a->mesh.id = 42;
    a->mode = (u8)TMODE_NEG;
    a->layers = TLAYER_GROUND | TLAYER_AIR;
    a->vec = (TVec){7.0f, 8.0f, 9.0f};
    a->samples[0] = 1.0f;
    a->samples[1] = -1.0f;
    a->samples[2] = 1e-20f;
    a->samples[3] = 1e20f;
    a->items[0] = (TItem){11, -12};
    a->items[1] = (TItem){13, -14};
}

static void fill_entity_data(TEntityData *d) {
    memset(d, 0, sizeof *d);
    d->transform.position = (de_dvec3){1.5, -2.25, 1e10};
    d->transform.rotation.w = 1.0f;
    d->transform.scale = (de_vec3){1.0f, 2.0f, 3.0f};
    d->inventory.count = 3;
    for (u32 i = 0; i < 8; ++i)
        d->inventory.items[i] = (TItem){100 + i, -(i32)i};
    d->mode = (u8)TMODE_RUN;
}

static void fill_transform(TTransform *t) {
    memset(t, 0, sizeof *t);
    t->position = (de_dvec3){1.5, -2.25, 1e10};
    t->rotation.x = 0.0f;
    t->rotation.y = 0.0f;
    t->rotation.z = 0.70710677f;
    t->rotation.w = 0.70710677f;
    t->scale = (de_vec3){1.0f, 2.0f, 3.0f};
}

/* ---- bin.roundtrip_all_kinds --------------------------------------------------------------- */

TEST_F(bin, roundtrip_all_kinds, bin_fx) {
    TAllKinds a;
    fill_all_kinds(&a);
    const de_ser_binary_options *modes[2] = {&k_self, &k_compact};
    for (u32 m = 0; m < 2; ++m) {
        de_mem_writer_reset(&f->mw);
        de_ser_binary_stats st;
        memset(&st, 0, sizeof st);
        de_ser_binary_options bo = *modes[m];
        bo.stats = &st;
        ASSERT_EQ(ser(f->r, f->ids.all_kinds, &a, NULL, &bo, &f->mw), DE_OK);
        EXPECT_EQ(st.objects_written, 1u);
        EXPECT_EQ(st.field_wise, 1u); /* BOOL, STRING and ASSET rule out the memcpy path */

        TAllKinds b;
        memset(&b, 0, sizeof b);
        size_t used = 0;
        ASSERT_EQ(deser(f->r, f->ids.all_kinds, &b, NULL, &bo, f->mw.data, f->mw.size, &used),
                  DE_OK);
        EXPECT_EQ(used, f->mw.size);
        EXPECT_MEMEQ(&a, &b, sizeof a); /* bit-exact, NaN payload, -0 and padding included */
        EXPECT_EQ(st.objects_read, 1u);

        /* Write -> read -> write gives the same bytes. */
        de_mem_writer again;
        de_mem_writer_init(&again, NULL);
        ASSERT_EQ(ser(f->r, f->ids.all_kinds, &b, NULL, &bo, &again), DE_OK);
        ASSERT_EQ(again.size, f->mw.size);
        EXPECT_MEMEQ(again.data, f->mw.data, f->mw.size);
        de_mem_writer_free(&again);

        /* The same bytes through a reader that returns 1 or 3 bytes per call. */
        for (size_t max = 1; max <= 3; max += 2) {
            chunk_reader c = {f->mw.data, f->mw.size, 0, max, 0};
            de_reader rd = {chunk_read, &c};
            memset(&b, 0, sizeof b);
            ASSERT_EQ(de_deserialize_binary(f->r, f->ids.all_kinds, &b, NULL, &bo, &rd), DE_OK);
            EXPECT_EQ(c.pos, f->mw.size);
            EXPECT_MEMEQ(&a, &b, sizeof a);
        }
    }

    /* Nested structs, arrays of structs and enums, and two objects in one stream. */
    TEntityData d, e;
    fill_entity_data(&d);
    de_mem_writer_reset(&f->mw);
    ASSERT_EQ(ser(f->r, f->ids.entity_data, &d, NULL, &k_self, &f->mw), DE_OK);
    size_t first = f->mw.size;
    d.inventory.count = 5;
    ASSERT_EQ(ser(f->r, f->ids.entity_data, &d, NULL, &k_compact, &f->mw), DE_OK);
    de_mem_reader mr;
    de_mem_reader_init(&mr, f->mw.data, f->mw.size);
    de_reader rd = de_reader_from_mem(&mr);
    memset(&e, 0, sizeof e);
    ASSERT_EQ(de_deserialize_binary(f->r, f->ids.entity_data, &e, NULL, NULL, &rd), DE_OK);
    EXPECT_EQ(mr.pos, first);
    EXPECT_EQ(e.inventory.count, 3u);
    ASSERT_EQ(de_deserialize_binary(f->r, f->ids.entity_data, &e, NULL, NULL, &rd), DE_OK);
    EXPECT_EQ(mr.pos, f->mw.size);
    EXPECT_MEMEQ(&d, &e, sizeof d);
    /* At the end of the stream: truncated header. */
    EXPECT_EQ(de_deserialize_binary(f->r, f->ids.entity_data, &e, NULL, NULL, &rd), DE_ERR_PARSE);

    /* Invalid arguments. */
    EXPECT_EQ(ser(f->r, (de_type_id){0}, &a, NULL, NULL, &f->mw), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ser(f->r, f->ids.mode, &a, NULL, NULL, &f->mw), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ser(f->r, f->ids.all_kinds, NULL, NULL, NULL, &f->mw), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_serialize_binary(f->r, f->ids.all_kinds, &a, NULL, NULL, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_deserialize_binary(f->r, f->ids.all_kinds, &a, NULL, NULL, NULL),
              DE_ERR_INVALID_ARGUMENT);
}

/* ---- bin.golden_bytes ---------------------------------------------------------------------- */

static void put_le(u8 *p, u64 v, u32 n) {
    for (u32 i = 0; i < n; ++i)
        p[i] = (u8)(v >> (8 * i));
}

TEST_F(bin, golden_bytes, bin_fx) {
    TTransform t;
    fill_transform(&t);

    /* Compact: build the expected bytes by hand from the documented layout. */
    ASSERT_EQ(ser(f->r, f->ids.transform, &t, NULL, &k_compact, &f->mw), DE_OK);
    u8 want[32 + 52];
    memcpy(want, "DEBO", 4);
    put_le(want + 4, 1, 2); /* format version */
    put_le(want + 6, 0, 2); /* flags: compact, required_flags 0 */
    put_le(want + 8, de_hash64_str("TTransform"), 8);
    put_le(want + 16, 1, 4);                     /* type version */
    put_le(want + 20, 0xc7444a2957d85c69ull, 8); /* golden schema hash (DENG-294) */
    put_le(want + 28, 52, 4);                    /* 3 f64 + 4 f32 + 3 f32, no padding */
    u8 *p = want + 32;
    const f64 pos[3] = {1.5, -2.25, 1e10};
    const f32 rest[7] = {0.0f, 0.0f, 0.70710677f, 0.70710677f, 1.0f, 2.0f, 3.0f};
    for (u32 i = 0; i < 3; ++i, p += 8) {
        u64 bits;
        memcpy(&bits, &pos[i], 8);
        put_le(p, bits, 8);
    }
    for (u32 i = 0; i < 7; ++i, p += 4) {
        u32 bits;
        memcpy(&bits, &rest[i], 4);
        put_le(p, bits, 4);
    }
    ASSERT_EQ(f->mw.size, sizeof want);
    EXPECT_MEMEQ(f->mw.data, want, sizeof want);

    /* Self-describing: equal to the checked-in golden file. DE_BIN_UPDATE_GOLDEN=1 rewrites it
     * (only when the format version changes on purpose). */
    de_mem_writer_reset(&f->mw);
    ASSERT_EQ(ser(f->r, f->ids.transform, &t, NULL, &k_self, &f->mw), DE_OK);
    const char *path = DE_BIN_TEST_DATA_DIR "/bin_golden_transform.bin";
    const char *update = getenv("DE_BIN_UPDATE_GOLDEN");
    if (update && update[0] == '1') {
        FILE *fp = fopen(path, "wb");
        ASSERT_TRUE(fp != NULL);
        ASSERT_EQ(fwrite(f->mw.data, 1, f->mw.size, fp), f->mw.size);
        fclose(fp);
    }
    FILE *fp = fopen(path, "rb");
    ASSERT_TRUE(fp != NULL);
    u8 golden[1024];
    size_t n = fread(golden, 1, sizeof golden, fp);
    fclose(fp);
    ASSERT_EQ(n, f->mw.size);
    EXPECT_MEMEQ(f->mw.data, golden, n);
    /* Same header as the compact object, apart from the flags and the payload size. */
    EXPECT_MEMEQ(golden, want, 6);
    EXPECT_EQ(golden[6], 1u); /* DE__BIN_FLAG_SELF_DESCRIBING */
    EXPECT_EQ(golden[7], 0u);
    EXPECT_MEMEQ(golden + 8, want + 8, 20);

    /* And it loads. */
    TTransform back;
    memset(&back, 0, sizeof back);
    ASSERT_EQ(deser(f->r, f->ids.transform, &back, NULL, NULL, golden, n, NULL), DE_OK);
    EXPECT_MEMEQ(&back, &t, sizeof t);
}

/* ---- bin.fast_path_pod --------------------------------------------------------------------- */

#define TB_ROW(X, r) X(r##0) X(r##1) X(r##2) X(r##3) X(r##4) X(r##5) X(r##6) X(r##7)
#define TB_ALL(X)                                                                                  \
    TB_ROW(X, 0)                                                                                   \
    TB_ROW(X, 1) TB_ROW(X, 2) TB_ROW(X, 3) TB_ROW(X, 4) TB_ROW(X, 5) TB_ROW(X, 6) TB_ROW(X, 7)
#define TB_MEMBER(n) f32 f##n;
#define TB_FIELD(n)  DE_FIELD(TBig, f##n, DE_FK_F32, DE_FIELD_DEFAULT),

/* A 64-field POD struct. */
typedef struct TBig {
    TB_ALL(TB_MEMBER)
} TBig;

static const de_field_info k_tbig_fields[] = {TB_ALL(TB_FIELD)};
DE_STATIC_ASSERT(sizeof(k_tbig_fields) / sizeof(k_tbig_fields[0]) == 64, "64 fields");

/* Nanoseconds per read, the best of `trials` runs of `iters` reads. */
static f64 time_reads(const de_type_registry *r, de_type_id t, const de_ser_binary_options *bo,
                      const u8 *data, size_t size, u32 trials, u32 iters) {
    TBig obj;
    f64 best = 1e30;
    for (u32 k = 0; k < trials; ++k) {
        u64 t0 = de_time_now_ns();
        for (u32 i = 0; i < iters; ++i) {
            de_mem_reader mr;
            de_mem_reader_init(&mr, data, size);
            de_reader rd = de_reader_from_mem(&mr);
            if (de_deserialize_binary(r, t, &obj, NULL, bo, &rd) != DE_OK)
                return -1.0;
        }
        f64 ns = (f64)(de_time_now_ns() - t0) / iters;
        if (ns < best)
            best = ns;
    }
    return best;
}

TEST_F(bin, fast_path_pod, bin_fx) {
    de_type_id big = de_type_register(f->r, &DE_TYPE_DESC(TBig, 1, k_tbig_fields));
    ASSERT_TRUE(DE_HANDLE_VALID(big));
    TBig a, b;
    f32 *av = (f32 *)&a;
    for (u32 i = 0; i < 64; ++i)
        av[i] = (f32)i * 1.25f - 7.0f;

    de_ser_binary_stats st;
    memset(&st, 0, sizeof st);
    de_ser_binary_options bo = {DE_SER_BINARY_COMPACT, &st, false};
    ASSERT_EQ(ser(f->r, big, &a, NULL, &bo, &f->mw), DE_OK);
    EXPECT_EQ(st.fast_path, 1u);
    EXPECT_EQ(f->mw.size, 32u + sizeof(TBig));
    memset(&b, 0, sizeof b);
    ASSERT_EQ(deser(f->r, big, &b, NULL, &bo, f->mw.data, f->mw.size, NULL), DE_OK);
    EXPECT_EQ(st.fast_path, 2u);
    EXPECT_EQ(st.field_wise, 0u);
    EXPECT_MEMEQ(&a, &b, sizeof a);

    /* The field-wise path gives the same bytes and the same object. */
    de_ser_binary_options slow = {DE_SER_BINARY_COMPACT, &st, true};
    de_mem_writer mw2;
    de_mem_writer_init(&mw2, NULL);
    ASSERT_EQ(ser(f->r, big, &a, NULL, &slow, &mw2), DE_OK);
    ASSERT_EQ(mw2.size, f->mw.size);
    EXPECT_MEMEQ(mw2.data, f->mw.data, f->mw.size);
    de_mem_writer_free(&mw2);
    memset(&b, 0, sizeof b);
    ASSERT_EQ(deser(f->r, big, &b, NULL, &slow, f->mw.data, f->mw.size, NULL), DE_OK);
    EXPECT_EQ(st.field_wise, 2u);
    EXPECT_MEMEQ(&a, &b, sizeof a);

    /* A self-describing object of the same schema takes the fast path too (block skipped). */
    de_mem_writer_reset(&f->mw);
    de_ser_binary_options self = {DE_SER_BINARY_SELF_DESCRIBING, &st, false};
    ASSERT_EQ(ser(f->r, big, &a, NULL, &self, &f->mw), DE_OK);
    u64 fast_before = st.fast_path;
    ASSERT_EQ(deser(f->r, big, &b, NULL, &self, f->mw.data, f->mw.size, NULL), DE_OK);
    EXPECT_EQ(st.fast_path, fast_before + 1);

    /* Not every field selected, padding, or a mapped entity: no memcpy. */
    de_ser_options only_rep = {DE_SER_BINARY, DE_FIELD_REPLICATE, NULL};
    u64 fw = st.field_wise;
    de_mem_writer_reset(&f->mw);
    ASSERT_EQ(ser(f->r, big, &a, &only_rep, &bo, &f->mw), DE_OK);
    EXPECT_EQ(st.field_wise, fw + 1);
    EXPECT_EQ(f->mw.size, 32u); /* no TBig field has REPLICATE */
    TTransform t;
    fill_transform(&t);
    ASSERT_EQ(ser(f->r, f->ids.transform, &t, NULL, &bo, &f->mw), DE_OK);
    EXPECT_EQ(st.field_wise, fw + 2); /* TTransform has padding between position and rotation */
    TVec v = {1, 2, 3};
    u64 fp = st.fast_path;
    ASSERT_EQ(ser(f->r, f->ids.vec, &v, NULL, &bo, &f->mw), DE_OK);
    EXPECT_EQ(st.fast_path, fp + 1);

    /* Microbenchmark (acceptance: memcpy path >= 5x faster than field-wise for 64 fields). */
    de_mem_writer_reset(&f->mw);
    ASSERT_EQ(ser(f->r, big, &a, NULL, &bo, &f->mw), DE_OK);
    de_ser_binary_options fast_o = {DE_SER_BINARY_COMPACT, NULL, false};
    de_ser_binary_options slow_o = {DE_SER_BINARY_COMPACT, NULL, true};
    f64 t_fast = 1e30, t_slow = 1e30;
    for (u32 round = 0; round < 5; ++round) { /* interleaved, best of all rounds */
        f64 tf = time_reads(f->r, big, &fast_o, f->mw.data, f->mw.size, 5, 4000);
        f64 ts = time_reads(f->r, big, &slow_o, f->mw.data, f->mw.size, 5, 4000);
        ASSERT_TRUE(tf > 0 && ts > 0);
        t_fast = tf < t_fast ? tf : t_fast;
        t_slow = ts < t_slow ? ts : t_slow;
    }
    f64 ratio = t_slow / t_fast;
    printf("bin.fast_path_pod: 64-field POD read: memcpy %.1f ns, field-wise %.1f ns, %.1fx\n",
           t_fast, t_slow, ratio);
#if defined(BIN_TEST_SANITIZED) || defined(DE_DEBUG)
    EXPECT_TRUE(ratio > 1.5); /* instrumented or unoptimised: only the direction */
#else
    const char *ci = getenv("DE_CI") ? getenv("DE_CI") : getenv("CI");
    bool on_ci = ci && ci[0] && strcmp(ci, "0") != 0 && strcmp(ci, "false") != 0;
    EXPECT_TRUE(ratio >= (on_ci ? 3.0 : 5.0));
#endif
}

/* ---- Migration types: "TMig" v1 and v2, nested in "TMigOuter" ------------------------------- */

typedef struct TMigV1 {
    f32 hp;
    i32 gold;
    u32 removed;
    char tag[8];
} TMigV1;

static const de_field_info k_mig_v1_fields[] = {
    DE_FIELD(TMigV1, hp, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(TMigV1, gold, DE_FK_I32, DE_FIELD_DEFAULT),
    DE_FIELD(TMigV1, removed, DE_FK_U32, DE_FIELD_DEFAULT),
    DE_FIELD(TMigV1, tag, DE_FK_STRING, DE_FIELD_DEFAULT),
};

/* v2: hp renamed to health, removed dropped, added added, tag grew, gold moved. */
typedef struct TMigV2 {
    u64 added;
    f32 health;
    i32 gold;
    char tag[16];
} TMigV2;

static const de_field_info k_mig_v2_fields[] = {
    DE_FIELD(TMigV2, added, DE_FK_U64, DE_FIELD_DEFAULT),
    DE_FIELD(TMigV2, health, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(TMigV2, gold, DE_FK_I32, DE_FIELD_DEFAULT),
    DE_FIELD(TMigV2, tag, DE_FK_STRING, DE_FIELD_DEFAULT),
};

typedef struct TMigOuterV1 {
    TMigV1 m;
    TMigV1 arr[2];
    u32 x;
} TMigOuterV1;

static const de_field_info k_outer_v1_fields[] = {
    DE_FIELD_EX(TMigOuterV1, m, DE_FK_STRUCT, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TMig", 0}),
    DE_FIELD_EX(TMigOuterV1, arr, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TMig", 2}),
    DE_FIELD(TMigOuterV1, x, DE_FK_U32, DE_FIELD_DEFAULT),
};

typedef struct TMigOuterV2 {
    TMigV2 m;
    TMigV2 arr[2];
    u32 x;
} TMigOuterV2;

static const de_field_info k_outer_v2_fields[] = {
    DE_FIELD_EX(TMigOuterV2, m, DE_FK_STRUCT, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TMig", 0}),
    DE_FIELD_EX(TMigOuterV2, arr, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TMig", 2}),
    DE_FIELD(TMigOuterV2, x, DE_FK_U32, DE_FIELD_DEFAULT),
};

/* What the migrate hook saw. */
static struct {
    u32 calls;
    u32 from_version;
    u32 field_count;
    bool names_ok, image_ok, drop;
} g_mig;

static bool mig_hook(void *obj, u32 from_version, const void *old_data,
                     const de_field_info *old_fields, u32 n) {
    TMigV2 *o = (TMigV2 *)obj;
    g_mig.calls++;
    g_mig.from_version = from_version;
    g_mig.field_count = n;
    static const char *const want[4] = {"hp", "gold", "removed", "tag"};
    static const u32 want_off[4] = {offsetof(TMigV1, hp), offsetof(TMigV1, gold),
                                    offsetof(TMigV1, removed), offsetof(TMigV1, tag)};
    static const u32 want_size[4] = {4, 4, 4, 8};
    g_mig.names_ok = n == 4;
    for (u32 i = 0; i < n && i < 4; ++i)
        g_mig.names_ok = g_mig.names_ok && strcmp(old_fields[i].name, want[i]) == 0 &&
                         old_fields[i].offset == want_off[i] && old_fields[i].size == want_size[i];
    /* The old image has the v1 memory layout. The data is untrusted: only after old_fields
     * proved the layout (every field inside the stored type, so the image holds at least
     * sizeof(TMigV1) bytes) is it read as the old struct, and through memcpy because a nested
     * image sits at a stored offset whose alignment nothing guarantees. */
    if (g_mig.names_ok) {
        TMigV1 old;
        memcpy(&old, old_data, sizeof old);
        g_mig.image_ok = old.gold == o->gold; /* gold was already copied by name */
    }
    for (u32 i = 0; i < n; ++i) {
        if (strcmp(old_fields[i].name, "hp") == 0 && old_fields[i].kind == DE_FK_F32)
            memcpy(&o->health, (const u8 *)old_data + old_fields[i].offset, 4);
    }
    return !g_mig.drop;
}

static de_type_desc mig_desc(const char *name, u32 size, u32 align, u32 version,
                             const de_field_info *fields, u32 n, bool hook) {
    de_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.size = size;
    d.align = align;
    d.version = version;
    d.fields = fields;
    d.field_count = n;
    d.hooks.migrate = hook ? mig_hook : NULL;
    d.is_pod = true;
    return d;
}

typedef struct mig_fx {
    de_type_registry *r1, *r2;
    de_type_id t1, t2, o1, o2;
    de_mem_writer mw;
} mig_fx;

static void mig_fx_register(mig_fx *f, bool hook) {
    de_type_desc d1 = mig_desc("TMig", sizeof(TMigV1), DE_ALIGNOF(TMigV1), 1, k_mig_v1_fields,
                               TEST_COUNT_OF(k_mig_v1_fields), false);
    de_type_desc d2 = mig_desc("TMig", sizeof(TMigV2), DE_ALIGNOF(TMigV2), 2, k_mig_v2_fields,
                               TEST_COUNT_OF(k_mig_v2_fields), hook);
    ASSERT_EQ(de_type_register_ex(f->r1, &d1, &f->t1, NULL), DE_OK);
    ASSERT_EQ(de_type_register_ex(f->r2, &d2, &f->t2, NULL), DE_OK);
    /* The outer type keeps version 1: only its nested type changed. */
    de_type_desc o1 = mig_desc("TMigOuter", sizeof(TMigOuterV1), DE_ALIGNOF(TMigOuterV1), 1,
                               k_outer_v1_fields, TEST_COUNT_OF(k_outer_v1_fields), false);
    de_type_desc o2 = mig_desc("TMigOuter", sizeof(TMigOuterV2), DE_ALIGNOF(TMigOuterV2), 1,
                               k_outer_v2_fields, TEST_COUNT_OF(k_outer_v2_fields), false);
    ASSERT_EQ(de_type_register_ex(f->r1, &o1, &f->o1, NULL), DE_OK);
    ASSERT_EQ(de_type_register_ex(f->r2, &o2, &f->o2, NULL), DE_OK);
}

static void mig_fx_setup(mig_fx *f) {
    ASSERT_EQ(de_type_registry_create(NULL, &f->r1), DE_OK);
    ASSERT_EQ(de_type_registry_create(NULL, &f->r2), DE_OK);
    de_mem_writer_init(&f->mw, NULL);
    memset(&g_mig, 0, sizeof g_mig);
}

static void mig_fx_teardown(mig_fx *f) {
    de_mem_writer_free(&f->mw);
    de_type_registry_destroy(f->r1);
    de_type_registry_destroy(f->r2);
}

static TMigV1 mig_v1_value(f32 hp) {
    TMigV1 v;
    memset(&v, 0, sizeof v);
    v.hp = hp;
    v.gold = -77;
    v.removed = 5;
    strcpy(v.tag, "hero");
    return v;
}

/* ---- bin.migrate_self_describing ----------------------------------------------------------- */

TEST_F(bin, migrate_self_describing, mig_fx) {
    mig_fx_register(f, true);
    TMigV1 v1 = mig_v1_value(42.5f);
    ASSERT_EQ(ser(f->r1, f->t1, &v1, NULL, &k_self, &f->mw), DE_OK);

    de_ser_binary_stats st;
    memset(&st, 0, sizeof st);
    de_ser_binary_options bo = {DE_SER_BINARY_SCHEMA_DEFAULT, &st, false};
    TMigV2 v2;
    memset(&v2, 0, sizeof v2);
    v2.added = 999; /* a field the old data does not have keeps its value */
    size_t used = 0;
    ASSERT_EQ(deser(f->r2, f->t2, &v2, NULL, &bo, f->mw.data, f->mw.size, &used), DE_OK);
    EXPECT_EQ(used, f->mw.size);
    EXPECT_EQ(v2.added, 999u);
    EXPECT_FLOAT_EQ(v2.health, 42.5, 0.0); /* renamed: moved by the migrate hook */
    EXPECT_EQ(v2.gold, -77);               /* matched by name, at a new offset */
    EXPECT_STREQ(v2.tag, "hero");          /* STRING buffer grew */
    EXPECT_EQ(g_mig.calls, 1u);
    EXPECT_EQ(g_mig.from_version, 1u);
    EXPECT_EQ(g_mig.field_count, 4u);
    EXPECT_TRUE(g_mig.names_ok);
    EXPECT_TRUE(g_mig.image_ok);
    EXPECT_EQ(st.schema_matched, 1u);
    EXPECT_EQ(st.migrations, 1u);
    EXPECT_EQ(st.skipped_fields, 2u); /* hp and removed: unknown to v2 */
    EXPECT_EQ(st.warnings, 1u);       /* once for the object */

    /* Nested: the outer type is unchanged (version 1) but nests TMig; every TMig instance (one
     * field and two array elements) migrates, and the object still warns once. */
    TMigOuterV1 o1;
    memset(&o1, 0, sizeof o1);
    o1.m = mig_v1_value(1.0f);
    o1.arr[0] = mig_v1_value(2.0f);
    o1.arr[1] = mig_v1_value(3.0f);
    strcpy(o1.arr[1].tag, "1234567"); /* the longest string a char[8] holds */
    o1.x = 0xABCDu;
    de_mem_writer_reset(&f->mw);
    ASSERT_EQ(ser(f->r1, f->o1, &o1, NULL, &k_self, &f->mw), DE_OK);
    memset(&st, 0, sizeof st);
    memset(&g_mig, 0, sizeof g_mig);
    TMigOuterV2 o2;
    memset(&o2, 0, sizeof o2);
    ASSERT_EQ(deser(f->r2, f->o2, &o2, NULL, &bo, f->mw.data, f->mw.size, NULL), DE_OK);
    EXPECT_EQ(g_mig.calls, 3u);
    EXPECT_FLOAT_EQ(o2.m.health, 1.0, 0.0);
    EXPECT_FLOAT_EQ(o2.arr[0].health, 2.0, 0.0);
    EXPECT_FLOAT_EQ(o2.arr[1].health, 3.0, 0.0);
    EXPECT_STREQ(o2.arr[1].tag, "1234567");
    EXPECT_EQ(o2.arr[1].gold, -77);
    EXPECT_EQ(o2.x, 0xABCDu);
    EXPECT_EQ(st.migrations, 3u);
    EXPECT_EQ(st.skipped_fields, 2u);
    EXPECT_EQ(st.warnings, 1u);

    /* A hook that returns false drops the object; the reader still stands after it. */
    g_mig.drop = true;
    memset(&v2, 0, sizeof v2);
    de_mem_writer_reset(&f->mw);
    ASSERT_EQ(ser(f->r1, f->t1, &v1, NULL, &k_self, &f->mw), DE_OK);
    ASSERT_EQ(deser(f->r2, f->t2, &v2, NULL, &bo, f->mw.data, f->mw.size, &used),
              DE_ERR_VERSION_MISMATCH);
    EXPECT_EQ(used, f->mw.size);
}

TEST_F(bin, migrate_without_hook, mig_fx) {
    mig_fx_register(f, false);
    TMigV1 v1 = mig_v1_value(42.5f);
    ASSERT_EQ(ser(f->r1, f->t1, &v1, NULL, &k_self, &f->mw), DE_OK);
    de_ser_binary_stats st;
    memset(&st, 0, sizeof st);
    de_ser_binary_options bo = {DE_SER_BINARY_SCHEMA_DEFAULT, &st, false};
    TMigV2 v2;
    memset(&v2, 0, sizeof v2);
    ASSERT_EQ(deser(f->r2, f->t2, &v2, NULL, &bo, f->mw.data, f->mw.size, NULL), DE_OK);
    EXPECT_EQ(v2.gold, -77);
    EXPECT_STREQ(v2.tag, "hero");
    EXPECT_FLOAT_EQ(v2.health, 0.0, 0.0);
    EXPECT_EQ(st.migrations, 0u);
    EXPECT_EQ(st.warnings, 1u);

    /* Data from a newer version: rejected, and the next object in the stream still loads. */
    TMigV2 newer;
    memset(&newer, 0, sizeof newer);
    de_mem_writer_reset(&f->mw);
    ASSERT_EQ(ser(f->r2, f->t2, &newer, NULL, &k_self, &f->mw), DE_OK);
    ASSERT_EQ(ser(f->r1, f->t1, &v1, NULL, &k_self, &f->mw), DE_OK);
    de_mem_reader mr;
    de_mem_reader_init(&mr, f->mw.data, f->mw.size);
    de_reader rd = de_reader_from_mem(&mr);
    TMigV1 back;
    memset(&back, 0, sizeof back);
    EXPECT_EQ(de_deserialize_binary(f->r1, f->t1, &back, NULL, NULL, &rd), DE_ERR_VERSION_MISMATCH);
    ASSERT_EQ(de_deserialize_binary(f->r1, f->t1, &back, NULL, NULL, &rd), DE_OK);
    EXPECT_EQ(mr.pos, f->mw.size);
    EXPECT_MEMEQ(&back, &v1, sizeof v1);
}

/* ---- bin.mismatch_rejected ----------------------------------------------------------------- */

TEST_F(bin, mismatch_rejected, mig_fx) {
    mig_fx_register(f, true);
    TMigV1 v1 = mig_v1_value(1.0f);
    ASSERT_EQ(ser(f->r1, f->t1, &v1, NULL, &k_compact, &f->mw), DE_OK);
    ASSERT_EQ(ser(f->r1, f->t1, &v1, NULL, &k_compact, &f->mw), DE_OK);
    size_t one = f->mw.size / 2;

    /* Compact data with another schema hash: rejected, the reader skips to the next object. */
    TMigV2 v2;
    memset(&v2, 0, sizeof v2);
    size_t used = 0;
    de_error_clear();
    EXPECT_EQ(deser(f->r2, f->t2, &v2, NULL, NULL, f->mw.data, f->mw.size, &used),
              DE_ERR_VERSION_MISMATCH);
    EXPECT_EQ(used, one);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_VERSION_MISMATCH);
    EXPECT_EQ(g_mig.calls, 0u);

    /* Same version, different layout (no version bump) and compact: rejected too. */
    static const de_field_info changed[] = {
        DE_FIELD(TMigV1, hp, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(TMigV1, gold, DE_FK_I32, DE_FIELD_DEFAULT),
    };
    de_type_registry *r3;
    ASSERT_EQ(de_type_registry_create(NULL, &r3), DE_OK);
    de_type_desc d3 = mig_desc("TMig", sizeof(TMigV1), DE_ALIGNOF(TMigV1), 1, changed, 2, false);
    de_type_id t3;
    ASSERT_EQ(de_type_register_ex(r3, &d3, &t3, NULL), DE_OK);
    TMigV1 v3;
    EXPECT_EQ(deser(r3, t3, &v3, NULL, NULL, f->mw.data, f->mw.size, NULL),
              DE_ERR_VERSION_MISMATCH);

    /* Another type's data, bad magic, a future format, unknown flags: not ours. */
    TVec tv;
    de_type_id tvec = de_type_register(r3, &DE_TYPE_DESC(TVec, 1, k_tvec_fields));
    EXPECT_EQ(deser(r3, tvec, &tv, NULL, NULL, f->mw.data, f->mw.size, NULL), DE_ERR_PARSE);
    u8 bad[64];
    memcpy(bad, f->mw.data, one < sizeof bad ? one : sizeof bad);
    bad[0] = 'X';
    EXPECT_EQ(deser(f->r1, f->t1, &v1, NULL, NULL, bad, one, NULL), DE_ERR_PARSE);
    memcpy(bad, f->mw.data, 8);
    bad[4] = 2; /* format version 2 */
    EXPECT_EQ(deser(f->r1, f->t1, &v1, NULL, NULL, bad, one, NULL), DE_ERR_VERSION_MISMATCH);
    memcpy(bad, f->mw.data, 8);
    bad[6] = 0x02; /* unknown flag bit */
    EXPECT_EQ(deser(f->r1, f->t1, &v1, NULL, NULL, bad, one, NULL), DE_ERR_PARSE);
    de_type_registry_destroy(r3);
}

/* ---- bin.truncation_sweep ------------------------------------------------------------------ */

/* Every proper prefix of a loadable object is DE_ERR_PARSE, and every single-byte corruption
 * either loads or fails cleanly (ASan / UBSan builds check that nothing reads out of bounds). */
static void sweep(const de_type_registry *r, de_type_id t, size_t obj_size, const u8 *data,
                  size_t size) {
    DE_ALIGN(16) u8 obj[1024]; /* migrate hooks cast it to their struct */
    ASSERT_TRUE(obj_size <= sizeof obj);
    memset(obj, 0, sizeof obj);
    ASSERT_EQ(deser(r, t, obj, NULL, NULL, data, size, NULL), DE_OK);
    u8 *copy = (u8 *)malloc(size);
    ASSERT_TRUE(copy != NULL);
    for (size_t n = 0; n < size; ++n) {
        memcpy(copy, data, n); /* an exact-size heap block: ASan sees reads past n */
        memset(obj, 0, sizeof obj);
        de_result res = deser(r, t, obj, NULL, NULL, copy, n, NULL);
        if (res != DE_ERR_PARSE) {
            char msg[96];
            snprintf(msg, sizeof msg, "prefix of %zu/%zu bytes gave %s", n, size,
                     de_result_str(res));
            FAIL(msg);
        }
    }
    for (size_t i = 0; i < size; ++i) {
        for (u32 k = 0; k < 3; ++k) {
            static const u8 masks[3] = {0xFF, 0x01, 0x80};
            memcpy(copy, data, size);
            copy[i] ^= masks[k];
            memset(obj, 0, sizeof obj);
            (void)deser(r, t, obj, NULL, NULL, copy, size, NULL);
        }
    }
    free(copy);
}

TEST_F(bin, truncation_sweep, bin_fx) {
    /* Corrupt inputs that still parse log a warning each: keep the output readable. */
    de_log_category cat = de_log_register_category("reflect", DE_LOG_INFO);
    de_log_set_level(cat, DE_LOG_ERROR);
    TAllKinds a;
    fill_all_kinds(&a);
    TEntityData d;
    fill_entity_data(&d);
    const de_ser_binary_options *modes[2] = {&k_self, &k_compact};
    for (u32 m = 0; m < 2; ++m) {
        de_mem_writer_reset(&f->mw);
        ASSERT_EQ(ser(f->r, f->ids.all_kinds, &a, NULL, modes[m], &f->mw), DE_OK);
        sweep(f->r, f->ids.all_kinds, sizeof a, f->mw.data, f->mw.size);
        de_mem_writer_reset(&f->mw);
        ASSERT_EQ(ser(f->r, f->ids.entity_data, &d, NULL, modes[m], &f->mw), DE_OK);
        sweep(f->r, f->ids.entity_data, sizeof d, f->mw.data, f->mw.size);
    }

    /* The schema path: v1 data read into v2 (old image, matching, migrate). */
    mig_fx mf;
    memset(&mf, 0, sizeof mf);
    mig_fx_setup(&mf);
    mig_fx_register(&mf, true);
    TMigOuterV1 o1;
    memset(&o1, 0, sizeof o1);
    o1.m = mig_v1_value(1.0f);
    o1.arr[1] = mig_v1_value(3.0f);
    ASSERT_EQ(ser(mf.r1, mf.o1, &o1, NULL, &k_self, &mf.mw), DE_OK);
    sweep(mf.r2, mf.o2, sizeof(TMigOuterV2), mf.mw.data, mf.mw.size);
    mig_fx_teardown(&mf);

    /* The fast path: a POD with a matching hash. */
    TVec v = {1, 2, 3};
    de_mem_writer_reset(&f->mw);
    ASSERT_EQ(ser(f->r, f->ids.vec, &v, NULL, &k_compact, &f->mw), DE_OK);
    sweep(f->r, f->ids.vec, sizeof v, f->mw.data, f->mw.size);
    de_log_set_level(cat, DE_LOG_INFO);
}

/* ---- bin.required_flags_filter ------------------------------------------------------------- */

TEST_F(bin, required_flags_filter, bin_fx) {
    TTransform t;
    fill_transform(&t);
    de_ser_options rep = {DE_SER_BINARY, DE_FIELD_REPLICATE, NULL};

    /* Writing with REPLICATE: position and rotation only. */
    ASSERT_EQ(ser(f->r, f->ids.transform, &t, &rep, &k_compact, &f->mw), DE_OK);
    ASSERT_EQ(f->mw.size, 32u + 24u + 16u);
    EXPECT_EQ(f->mw.data[7], (u8)DE_FIELD_REPLICATE); /* the filter is in the header */
    TTransform b;
    memset(&b, 0, sizeof b);
    b.scale = (de_vec3){-1.0f, -1.0f, -1.0f};
    ASSERT_EQ(deser(f->r, f->ids.transform, &b, NULL, NULL, f->mw.data, f->mw.size, NULL), DE_OK);
    EXPECT_MEMEQ(&b.position, &t.position, sizeof t.position);
    EXPECT_MEMEQ(&b.rotation, &t.rotation, sizeof t.rotation);
    EXPECT_FLOAT_EQ(b.scale.x, -1.0, 0.0); /* not in the data: kept */

    /* Reading with REPLICATE from data that has every field: scale is skipped. */
    de_mem_writer_reset(&f->mw);
    ASSERT_EQ(ser(f->r, f->ids.transform, &t, NULL, &k_self, &f->mw), DE_OK);
    memset(&b, 0, sizeof b);
    b.scale.y = -5.0f;
    size_t used = 0;
    ASSERT_EQ(deser(f->r, f->ids.transform, &b, &rep, NULL, f->mw.data, f->mw.size, &used), DE_OK);
    EXPECT_EQ(used, f->mw.size);
    EXPECT_MEMEQ(&b.position, &t.position, sizeof t.position);
    EXPECT_FLOAT_EQ(b.scale.y, -5.0, 0.0);

    /* Nested: TEntityData.transform is DEFAULT only, so REPLICATE writes nothing below it. */
    TEntityData d;
    fill_entity_data(&d);
    de_mem_writer_reset(&f->mw);
    ASSERT_EQ(ser(f->r, f->ids.entity_data, &d, &rep, &k_compact, &f->mw), DE_OK);
    EXPECT_EQ(f->mw.size, 32u);
    de_ser_options ser_only = {DE_SER_BINARY, DE_FIELD_SERIALIZE, NULL};
    de_mem_writer_reset(&f->mw);
    ASSERT_EQ(ser(f->r, f->ids.entity_data, &d, &ser_only, &k_self, &f->mw), DE_OK);
    TEntityData e;
    memset(&e, 0, sizeof e);
    ASSERT_EQ(deser(f->r, f->ids.entity_data, &e, NULL, NULL, f->mw.data, f->mw.size, NULL), DE_OK);
    EXPECT_MEMEQ(&e, &d, sizeof d);

    de_ser_options bad = {DE_SER_BINARY, 0x100u, NULL};
    EXPECT_EQ(ser(f->r, f->ids.transform, &t, &bad, NULL, &f->mw), DE_ERR_INVALID_ARGUMENT);
}

/* ---- bin.entity_asset_callbacks ------------------------------------------------------------ */

typedef struct cb_log {
    u32 to_scene, to_entity, to_id, to_asset;
    u32 bad_user;
    u32 bad_type;
    u64 last_entity, last_scene, last_id;
    u32 last_handle;
} cb_log;

#define SCENE_XOR     0x5A5A000000000000ull
#define ASSET_ID_BASE 0xA55E700000000000ull

static u64 cb_entity_to_scene(u64 entity_bits, void *user) {
    cb_log *l = (cb_log *)user;
    l->to_scene++;
    l->last_entity = entity_bits;
    return entity_bits ^ SCENE_XOR;
}

static u64 cb_scene_to_entity(u64 scene_id, void *user) {
    cb_log *l = (cb_log *)user;
    l->to_entity++;
    l->last_scene = scene_id;
    return scene_id ^ SCENE_XOR;
}

static u64 cb_asset_to_id(u32 handle, const char *asset_type, void *user) {
    cb_log *l = (cb_log *)user;
    l->to_id++;
    l->last_handle = handle;
    if (!asset_type || strcmp(asset_type, "mesh") != 0)
        l->bad_type++;
    return ASSET_ID_BASE | handle;
}

static u32 cb_id_to_asset(u64 id, const char *asset_type, void *user) {
    cb_log *l = (cb_log *)user;
    l->to_asset++;
    l->last_id = id;
    if (!asset_type || strcmp(asset_type, "mesh") != 0)
        l->bad_type++;
    return (u32)(id & 0xFFFFFFFFu);
}

TEST_F(bin, entity_asset_callbacks, bin_fx) {
    TAllKinds a;
    fill_all_kinds(&a);
    cb_log log;
    memset(&log, 0, sizeof log);
    de_ser_context ctx = {cb_entity_to_scene, cb_scene_to_entity, cb_asset_to_id, cb_id_to_asset,
                          &log};
    de_ser_options opt = {DE_SER_BINARY, 0, &ctx};

    const de_ser_binary_options *modes[2] = {&k_self, &k_compact};
    for (u32 m = 0; m < 2; ++m) {
        memset(&log, 0, sizeof log);
        de_mem_writer_reset(&f->mw);
        ASSERT_EQ(ser(f->r, f->ids.all_kinds, &a, &opt, modes[m], &f->mw), DE_OK);
        EXPECT_EQ(log.to_scene, 1u); /* once per value, even with the measuring pass */
        EXPECT_EQ(log.to_id, 1u);
        EXPECT_EQ(log.last_entity, a.entity);
        EXPECT_EQ(log.last_handle, a.mesh.id);

        /* The mapped values are in the bytes, the raw ones are not. */
        u64 scene = a.entity ^ SCENE_XOR, asset = ASSET_ID_BASE | a.mesh.id;
        bool found_scene = false, found_asset = false;
        for (size_t i = 0; i + 8 <= f->mw.size; ++i) {
            found_scene = found_scene || memcmp(f->mw.data + i, &scene, 8) == 0;
            found_asset = found_asset || memcmp(f->mw.data + i, &asset, 8) == 0;
        }
        EXPECT_TRUE(found_scene);
        EXPECT_TRUE(found_asset);

        TAllKinds b;
        memset(&b, 0, sizeof b);
        ASSERT_EQ(deser(f->r, f->ids.all_kinds, &b, &opt, NULL, f->mw.data, f->mw.size, NULL),
                  DE_OK);
        EXPECT_EQ(log.to_entity, 1u);
        EXPECT_EQ(log.to_asset, 1u);
        EXPECT_EQ(log.last_scene, scene);
        EXPECT_EQ(log.last_id, asset);
        EXPECT_EQ(log.bad_type, 0u);
        EXPECT_MEMEQ(&a, &b, sizeof a);
    }

    /* The schema path maps too (TAllKinds read back through a registry where it changed). */
    de_type_registry *r2;
    ASSERT_EQ(de_type_registry_create(NULL, &r2), DE_OK);
    test_type_ids ids2;
    ASSERT_EQ(test_types_register(r2, &ids2), DE_OK);
    de_type_desc d2 = DE_TYPE_DESC(TAllKinds, 2, k_tallkinds_fields);
    ASSERT_EQ(de_type_register_ex(r2, &d2, &ids2.all_kinds, NULL), DE_OK);
    memset(&log, 0, sizeof log);
    de_mem_writer_reset(&f->mw);
    ASSERT_EQ(ser(f->r, f->ids.all_kinds, &a, &opt, &k_self, &f->mw), DE_OK);
    TAllKinds c;
    memset(&c, 0, sizeof c);
    de_ser_binary_stats st;
    memset(&st, 0, sizeof st);
    de_ser_binary_options bo = {DE_SER_BINARY_SCHEMA_DEFAULT, &st, false};
    ASSERT_EQ(deser(r2, ids2.all_kinds, &c, &opt, &bo, f->mw.data, f->mw.size, NULL), DE_OK);
    EXPECT_EQ(st.schema_matched, 1u);
    EXPECT_EQ(log.to_entity, 1u);
    EXPECT_EQ(log.to_asset, 1u);
    EXPECT_EQ(log.bad_type, 0u);
    EXPECT_MEMEQ(&a, &c, sizeof a);
    de_type_registry_destroy(r2);

    /* NULL callbacks or no context: raw values both ways. */
    de_ser_context none = {NULL, NULL, NULL, NULL, &log};
    de_ser_options raw = {DE_SER_BINARY, 0, &none};
    memset(&log, 0, sizeof log);
    de_mem_writer_reset(&f->mw);
    ASSERT_EQ(ser(f->r, f->ids.all_kinds, &a, &raw, &k_compact, &f->mw), DE_OK);
    TAllKinds b;
    memset(&b, 0, sizeof b);
    ASSERT_EQ(deser(f->r, f->ids.all_kinds, &b, NULL, NULL, f->mw.data, f->mw.size, NULL), DE_OK);
    EXPECT_MEMEQ(&a, &b, sizeof a);
    EXPECT_EQ(log.to_scene + log.to_entity + log.to_id + log.to_asset, 0u);

    /* An asset ID read without id_to_asset does not fit a handle: a parse error, not a wrap. */
    de_mem_writer_reset(&f->mw);
    ASSERT_EQ(ser(f->r, f->ids.all_kinds, &a, &opt, &k_compact, &f->mw), DE_OK);
    EXPECT_EQ(deser(f->r, f->ids.all_kinds, &b, NULL, NULL, f->mw.data, f->mw.size, NULL),
              DE_ERR_PARSE);
}

/* ---- Memory writer and reader -------------------------------------------------------------- */

static de_result failing_write(void *user, const void *data, size_t bytes) {
    (void)data;
    size_t *budget = (size_t *)user;
    if (bytes > *budget)
        return DE_FAIL(DE_ERR_IO, "test writer is full");
    *budget -= bytes;
    return DE_OK;
}

TEST_F(bin, mem_reader_writer, bin_fx) {
    TAllKinds a;
    fill_all_kinds(&a);

    /* A fixed buffer that is too small fails without overrunning it. */
    u8 small[40];
    de_mem_writer fw;
    de_mem_writer_init_fixed(&fw, small, sizeof small);
    EXPECT_EQ(ser(f->r, f->ids.all_kinds, &a, NULL, &k_compact, &fw), DE_ERR_OUT_OF_MEMORY);
    EXPECT_TRUE(fw.size <= sizeof small);
    de_mem_writer_free(&fw);

    /* A large enough fixed buffer holds the same bytes as a growing one. */
    ASSERT_EQ(ser(f->r, f->ids.all_kinds, &a, NULL, &k_self, &f->mw), DE_OK);
    u8 big[4096];
    de_mem_writer_init_fixed(&fw, big, sizeof big);
    ASSERT_EQ(ser(f->r, f->ids.all_kinds, &a, NULL, &k_self, &fw), DE_OK);
    ASSERT_EQ(fw.size, f->mw.size);
    EXPECT_MEMEQ(big, f->mw.data, fw.size);

    /* A writer error is returned for every failure point. */
    for (size_t budget = 0; budget < f->mw.size; budget += 7) {
        size_t left = budget;
        de_writer w = {failing_write, &left};
        EXPECT_EQ(de_serialize_binary(f->r, f->ids.all_kinds, &a, NULL, &k_self, &w), DE_ERR_IO);
    }

    /* The memory reader reports 0 bytes at the end. */
    de_mem_reader mr;
    de_mem_reader_init(&mr, big, 4);
    de_reader rd = de_reader_from_mem(&mr);
    u8 buf[8];
    size_t got = 99;
    ASSERT_EQ(rd.read(rd.user, buf, 8, &got), DE_OK);
    EXPECT_EQ(got, 4u);
    ASSERT_EQ(rd.read(rd.user, buf, 8, &got), DE_OK);
    EXPECT_EQ(got, 0u);
}

/* ---- de_serialize_object / de_deserialize_object with DE_SER_BINARY ------------------------ */

TEST_F(bin, object_api_dispatch, bin_fx) {
    TAllKinds a, b;
    fill_all_kinds(&a);
    de_ser_options opt = {DE_SER_BINARY, 0, NULL};
    de_writer w = de_writer_from_mem(&f->mw);
    ASSERT_EQ(de_serialize_object(f->r, f->ids.all_kinds, &a, &opt, &w), DE_OK);

    /* The same bytes as de_serialize_binary with default options. */
    de_mem_writer direct;
    de_mem_writer_init(&direct, NULL);
    ASSERT_EQ(ser(f->r, f->ids.all_kinds, &a, &opt, NULL, &direct), DE_OK);
    ASSERT_EQ(direct.size, f->mw.size);
    EXPECT_MEMEQ(direct.data, f->mw.data, f->mw.size);
    de_mem_writer_free(&direct);

    de_mem_reader mr;
    de_mem_reader_init(&mr, f->mw.data, f->mw.size);
    de_reader rd = de_reader_from_mem(&mr);
    memset(&b, 0, sizeof b);
    ASSERT_EQ(de_deserialize_object(f->r, f->ids.all_kinds, &b, &opt, &rd), DE_OK);
    EXPECT_EQ(mr.pos, f->mw.size);
    EXPECT_MEMEQ(&a, &b, sizeof a);
}
