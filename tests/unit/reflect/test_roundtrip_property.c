/*
 * test_roundtrip_property.c: property tests of the text and binary serializers (DENG-297).
 *
 * Each test generates PROP_TYPES random struct types (random field kinds, sizes and counts,
 * nested structs up to PROP_MAX_DEPTH levels below the top type, packed or padded layouts,
 * memory order different from declaration order), registers them at run time in a fresh
 * registry, and fills PROP_VALUES random objects of each from a seeded de_rng. Then:
 *
 *   prop.text_roundtrip           text write -> read gives the same object (floats bit-exact,
 *                                 NaN payloads aside), and writing it again gives the same bytes;
 *   prop.binary_roundtrip         binary write -> read gives the same bytes of memory, in both
 *                                 schema modes, with and without the fast path;
 *   prop.text_binary_equivalence  text -> object -> binary -> object equals the original, and
 *                                 both formats load the same fields under a required_flags
 *                                 filter (into the same prefilled object).
 *
 * Reproduce a failure with the seed the failure prints: DE_PROP_SEED=<seed> test_prop
 * --filter=prop.<name>. The prop.seed_b / prop.seed_c CTest entries run every test again with
 * two other seeds.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#endif
#include "de_test.h"

#include <dengine/de_math.h>
#include <dengine/de_reflect.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROP_DEFAULT_SEED 0x297u
#define PROP_TYPES        500u
#define PROP_VALUES       20u
#define PROP_MAX_DEPTH    3u  /* nested struct levels below the top type */
#define PROP_MAX_FIELDS   9u  /* per struct */
#define PROP_MAX_STRUCTS  24u /* per registry (top type included) */
#define PROP_MAX_OBJECT   (64u * 1024u)

/* ---- Seed ---------------------------------------------------------------------------------- */

static u64 prop_seed(void) {
    const char *s = getenv("DE_PROP_SEED");
    if (s != NULL && *s != '\0')
        return (u64)strtoull(s, NULL, 0);
    return PROP_DEFAULT_SEED;
}

static u32 rnd(de_rng *g, u32 n) {
    return de_rng_range(g, 0, n);
}

static u64 rnd64(de_rng *g) {
    u64 hi = de_rng_u32(g);
    return (hi << 32) | de_rng_u32(g);
}

/* ---- Enums ----------------------------------------------------------------------------------
 * E: an enum whose values fit any size (signed and unsigned readings); G: an enum for 8-byte
 * fields only; F: flags with single- and multi-bit values; W: flags for 8-byte fields, bit 63
 * included (a negative i64 value). */

static const de_enum_value k_e_values[] = {
    {"Zero", 0}, {"One", 1}, {"Big", 200}, {"Neg", -3}, {"Min", -128},
};
static const de_enum_value k_g_values[] = {
    {"Huge", (i64)1 << 40},
    {"NegHuge", -((i64)1 << 50)},
    {"Nil", 0},
};
static const de_enum_value k_f_values[] = {
    {"A", 1}, {"B", 2}, {"C", 4}, {"D", 64}, {"CD", 68},
};
static const de_enum_value k_w_values[] = {
    {"Lo", 1},
    {"Mid", (i64)1 << 40},
    {"Top", INT64_MIN},
};

#define PROP_COUNT_OF(a) ((u32)(sizeof(a) / sizeof((a)[0])))

/* ---- Random types --------------------------------------------------------------------------- */

typedef struct gtype {
    char name[8];
    de_field_info fields[PROP_MAX_FIELDS];
    char fnames[PROP_MAX_FIELDS][8];
    u32 field_count, size, align, height; /* height: struct levels below this type */
    de_type_id id;
} gtype;

typedef struct gen {
    de_rng *rng;
    de_type_registry *r;
    gtype types[PROP_MAX_STRUCTS];
    u32 count;
} gen;

/* The built-in element names of FIXED_ARRAY (de_field_kind order) and their fixed sizes. */
static const struct {
    const char *name;
    de_field_kind kind;
    u32 size, align;
} k_builtin[] = {
    {"bool", DE_FK_BOOL, 1, 1},   {"i8", DE_FK_I8, 1, 1},         {"i16", DE_FK_I16, 2, 2},
    {"i32", DE_FK_I32, 4, 4},     {"i64", DE_FK_I64, 8, 8},       {"u8", DE_FK_U8, 1, 1},
    {"u16", DE_FK_U16, 2, 2},     {"u32", DE_FK_U32, 4, 4},       {"u64", DE_FK_U64, 8, 8},
    {"f32", DE_FK_F32, 4, 4},     {"f64", DE_FK_F64, 8, 8},       {"vec2", DE_FK_VEC2, 8, 4},
    {"vec3", DE_FK_VEC3, 12, 4},  {"vec4", DE_FK_VEC4, 16, 4},    {"dvec3", DE_FK_DVEC3, 24, 8},
    {"quat", DE_FK_QUAT, 16, 4},  {"mat4", DE_FK_MAT4, 64, 4},    {"color", DE_FK_COLOR, 16, 4},
    {"name", DE_FK_NAME, 8, 8},   {"string", DE_FK_STRING, 0, 1}, {"entity", DE_FK_ENTITY, 8, 8},
    {"asset", DE_FK_ASSET, 4, 4},
};

static u32 align_up(u32 v, u32 a) {
    return (v + a - 1u) / a * a;
}

static u32 int_size(de_rng *g) {
    static const u32 k_sizes[] = {1, 2, 4, 8};
    return k_sizes[rnd(g, 4)];
}

static u32 gen_struct(gen *g, u32 depth);

/* A struct type for a field at `depth` (the field's struct is at depth `depth`): an existing
 * one that is shallow enough, or a new one. Returns the index, or UINT32_MAX if none fits. */
static u32 pick_struct(gen *g, u32 depth) {
    if (depth >= PROP_MAX_DEPTH)
        return UINT32_MAX;
    u32 max_height = PROP_MAX_DEPTH - depth - 1u;
    if (g->count > 0 && rnd(g->rng, 3) == 0) {
        u32 i = rnd(g->rng, g->count);
        if (g->types[i].height <= max_height && g->types[i].id.id != 0)
            return i;
    }
    if (g->count >= PROP_MAX_STRUCTS - 1u)
        return UINT32_MAX;
    return gen_struct(g, depth + 1u);
}

/* Fills kind, size, attrs of `f`; returns the field's natural alignment and its struct height
 * (+1 for a nested struct) through `*height`. */
static u32 gen_field(gen *g, de_field_info *f, u32 depth, u32 *height) {
    de_rng *rng = g->rng;
    *height = 0;
    for (;;) {
        /* One field in five is a nested struct, so most types nest (the kinds are uniform
         * otherwise); pick_struct stops at PROP_MAX_DEPTH. */
        de_field_kind k =
            rnd(rng, 5) == 0 ? DE_FK_STRUCT : (de_field_kind)rnd(rng, DE_FIELD_KIND_COUNT);
        f->kind = k;
        memset(&f->attrs, 0, sizeof f->attrs);
        switch (k) {
        case DE_FK_STRING:
            f->size = 1u + rnd(rng, 40);
            return 1;
        case DE_FK_ENUM:
            if (rnd(rng, 4) == 0) {
                f->attrs.nested_type = "G";
                f->size = 8;
            } else {
                f->attrs.nested_type = "E";
                f->size = int_size(rng);
            }
            return f->size;
        case DE_FK_FLAGS:
            if (rnd(rng, 3) == 0) {
                f->attrs.nested_type = "W";
                f->size = 8;
            } else {
                f->attrs.nested_type = "F";
                f->size = int_size(rng);
            }
            return f->size;
        case DE_FK_ASSET:
            f->size = 4;
            if (rnd(rng, 2) == 0)
                f->attrs.nested_type = "mesh";
            return 4;
        case DE_FK_STRUCT: {
            u32 s = pick_struct(g, depth);
            if (s == UINT32_MAX)
                continue;
            f->attrs.nested_type = g->types[s].name;
            f->size = g->types[s].size;
            *height = g->types[s].height + 1u;
            return g->types[s].align;
        }
        case DE_FK_FIXED_ARRAY: {
            u32 count = 1u + rnd(rng, 5);
            u32 elem, align;
            u32 choice = rnd(rng, 10);
            if (choice == 0) { /* no nested_type: unsigned integers */
                elem = int_size(rng);
                align = elem;
            } else if (choice == 1) {
                f->attrs.nested_type = rnd(rng, 2) ? "E" : "F";
                elem = int_size(rng);
                align = elem;
            } else if (choice <= 3) {
                u32 s = pick_struct(g, depth);
                if (s == UINT32_MAX)
                    continue;
                f->attrs.nested_type = g->types[s].name;
                elem = g->types[s].size;
                align = g->types[s].align;
                *height = g->types[s].height + 1u;
            } else {
                u32 b = rnd(rng, PROP_COUNT_OF(k_builtin));
                f->attrs.nested_type = k_builtin[b].name;
                elem = k_builtin[b].size != 0 ? k_builtin[b].size : 1u + rnd(rng, 12);
                align = k_builtin[b].align;
            }
            f->attrs.array_count = count;
            f->size = elem * count;
            return align;
        }
        default:
            for (u32 b = 0; b < PROP_COUNT_OF(k_builtin); ++b) {
                if (k_builtin[b].kind == k) {
                    f->size = k_builtin[b].size;
                    return k_builtin[b].align;
                }
            }
            continue;
        }
    }
}

static u32 random_flags(de_rng *g) {
    /* Mostly the default; sometimes subsets, so required_flags filters select differently. */
    static const u32 k_flags[] = {
        DE_FIELD_DEFAULT,
        DE_FIELD_DEFAULT | DE_FIELD_REPLICATE,
        DE_FIELD_SERIALIZE,
        DE_FIELD_SNAPSHOT | DE_FIELD_REPLICATE,
        DE_FIELD_SERIALIZE | DE_FIELD_EDITOR_HIDDEN,
        0,
    };
    return k_flags[rnd(g, PROP_COUNT_OF(k_flags))];
}

/* Generates, lays out and registers one struct type at `depth`. Returns its index. */
static u32 gen_struct(gen *g, u32 depth) {
    de_rng *rng = g->rng;
    u32 idx = g->count++;
    gtype *t = &g->types[idx];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "S%u", idx);

    u32 n = rnd(rng, 12) == 0 ? 0u : 1u + rnd(rng, PROP_MAX_FIELDS);
    u32 aligns[PROP_MAX_FIELDS];
    for (u32 i = 0; i < n; ++i) {
        de_field_info *f = &t->fields[i];
        snprintf(t->fnames[i], sizeof t->fnames[i], "a%u", i);
        f->name = t->fnames[i];
        f->flags = random_flags(rng);
        u32 h;
        aligns[i] = gen_field(g, f, depth, &h);
        if (h > t->height)
            t->height = h;
    }
    t = &g->types[idx]; /* (gen_field may have added types; the array does not move) */
    t->field_count = n;

    /* Memory order: declaration order, or shuffled. Packed (alignment 1) or natural, with
     * random gaps. */
    u32 order[PROP_MAX_FIELDS];
    for (u32 i = 0; i < n; ++i)
        order[i] = i;
    if (rnd(rng, 4) == 0) {
        for (u32 i = n; i > 1; --i) {
            u32 j = rnd(rng, i);
            u32 tmp = order[i - 1];
            order[i - 1] = order[j];
            order[j] = tmp;
        }
    }
    bool packed = rnd(rng, 4) == 0;
    u32 type_align = 1, at = 0;
    for (u32 o = 0; o < n; ++o) {
        u32 i = order[o];
        u32 a = packed ? 1u : aligns[i];
        if (rnd(rng, 4) == 0)
            at += 1u + rnd(rng, 3);
        at = align_up(at, a);
        t->fields[i].offset = at;
        at += t->fields[i].size;
        if (a > type_align)
            type_align = a;
    }
    if (at == 0)
        at = 1u + rnd(rng, 8); /* an empty struct still has a size */
    t->align = type_align;
    t->size = align_up(at, type_align);

    de_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = t->name;
    d.size = t->size;
    d.align = rnd(rng, 2) ? t->align : 0;
    d.version = 1u + rnd(rng, 3);
    d.fields = t->fields;
    d.field_count = n;
    d.is_pod = rnd(rng, 4) != 0;
    de_result res = de_type_register_ex(g->r, &d, &t->id, NULL);
    if (res != DE_OK) {
        const de_error *e = de_error_last();
        fprintf(stderr, "prop: registering %s failed: %s\n", t->name, e ? e->message : "?");
    }
    ASSERT_EQ(res, DE_OK);
    return idx;
}

/* A fresh registry with the enums and one random top type (and the types it nests). */
static const gtype *gen_type(gen *g, de_rng *rng) {
    memset(g, 0, sizeof *g);
    g->rng = rng;
    ASSERT_EQ(de_type_registry_create(NULL, &g->r), DE_OK);
    ASSERT_EQ(de_enum_register(g->r, "E", k_e_values, PROP_COUNT_OF(k_e_values), false), DE_OK);
    ASSERT_EQ(de_enum_register(g->r, "G", k_g_values, PROP_COUNT_OF(k_g_values), false), DE_OK);
    ASSERT_EQ(de_enum_register(g->r, "F", k_f_values, PROP_COUNT_OF(k_f_values), true), DE_OK);
    ASSERT_EQ(de_enum_register(g->r, "W", k_w_values, PROP_COUNT_OF(k_w_values), true), DE_OK);
    /* The top type is generated last-registered-first: its nested types register before it. */
    u32 top = gen_struct(g, 0);
    ASSERT_TRUE(g->types[top].size <= PROP_MAX_OBJECT);
    return &g->types[top];
}

static void gen_free(gen *g) {
    de_type_registry_destroy(g->r);
    g->r = NULL;
}

/* ---- Random values -------------------------------------------------------------------------- */

static f64 random_f64(de_rng *g) {
    static const u64 k_special[] = {
        0x0000000000000000ull, 0x8000000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
        0x7ff8000000000000ull, 0x7ff0000000000001ull, 0xfff8000000000123ull, 0x0000000000000001ull,
        0x000fffffffffffffull, 0x0010000000000000ull, 0x7fefffffffffffffull, 0x3ff0000000000000ull,
    };
    f64 v;
    u32 c = rnd(g, 8);
    if (c == 0) {
        u64 bits = k_special[rnd(g, PROP_COUNT_OF(k_special))];
        memcpy(&v, &bits, 8);
    } else if (c <= 2) {
        u64 bits = rnd64(g);
        memcpy(&v, &bits, 8);
    } else {
        v = (de_rng_f64(g) - 0.5) * 2.0;
        for (u32 e = rnd(g, 12); e > 0; --e)
            v *= 10.0;
    }
    return v;
}

static f32 random_f32(de_rng *g) {
    static const u32 k_special[] = {
        0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0x7f800001u,
        0xffc00123u, 0x00000001u, 0x007fffffu, 0x00800000u, 0x7f7fffffu, 0x3f800000u,
    };
    f32 v;
    u32 c = rnd(g, 8);
    if (c == 0) {
        u32 bits = k_special[rnd(g, PROP_COUNT_OF(k_special))];
        memcpy(&v, &bits, 4);
    } else if (c <= 2) {
        u32 bits = de_rng_u32(g);
        memcpy(&v, &bits, 4);
    } else {
        v = (de_rng_f32(g) - 0.5f) * 2.0f;
        for (u32 e = rnd(g, 6); e > 0; --e)
            v *= 10.0f;
    }
    return v;
}

/* A NUL-terminated string of at most size - 1 bytes: ASCII, escapes, control characters and
 * multi-byte UTF-8 (never invalid UTF-8, which the text format refuses). */
static void random_string(de_rng *g, u8 *dst, u32 size) {
    static const char *const k_pieces[] = {
        "\"",
        "\\",
        "\n",
        "\t",
        "\r",
        "\b",
        "\f",
        "\x01",
        "\x1f",
        "\x7f",
        "\xc3\xa9",
        "\xe2\x82\xac",
        "\xf0\x9f\x98\x80",
        "'",
        "=",
        "#",
        "[",
        "{",
        " ",
    };
    memset(dst, 0, size);
    u32 len = rnd(g, size);
    u32 at = 0;
    while (at < len) {
        if (rnd(g, 4) == 0) {
            const char *p = k_pieces[rnd(g, PROP_COUNT_OF(k_pieces))];
            size_t n = strlen(p);
            if (at + n > len)
                break;
            memcpy(dst + at, p, n);
            at += (u32)n;
        } else {
            dst[at++] = (u8)(0x20u + rnd(g, 0x5Fu));
        }
    }
}

static void random_enum(de_rng *g, const de_type_registry *r, const char *nested, u32 size,
                        bool flags, u8 *dst) {
    u64 v;
    const de_enum_info *e = de_enum_get(r, de_type_find(r, nested));
    if (!flags && e != NULL && rnd(g, 4) != 0) {
        v = (u64)e->values[rnd(g, e->count)].value;
    } else if (flags && e != NULL && rnd(g, 2) == 0) {
        v = 0;
        for (u32 i = 0; i < e->count; ++i)
            if (rnd(g, 2))
                v |= (u64)e->values[i].value;
    } else {
        v = rnd64(g);
    }
    memcpy(dst, &v, size); /* little-endian: the low `size` bytes */
}

static void fill_struct(de_rng *g, const de_type_registry *r, de_type_id t, u8 *obj);

static void fill_value(de_rng *g, const de_type_registry *r, de_field_kind k, u32 size,
                       const char *nested, u8 *dst) {
    switch (k) {
    case DE_FK_BOOL:
        dst[0] = (u8)rnd(g, 2);
        return;
    case DE_FK_F32:
    case DE_FK_VEC2:
    case DE_FK_VEC3:
    case DE_FK_VEC4:
    case DE_FK_QUAT:
    case DE_FK_COLOR:
    case DE_FK_MAT4:
        for (u32 i = 0; i < size / 4u; ++i) {
            f32 v = random_f32(g);
            memcpy(dst + 4u * i, &v, 4);
        }
        return;
    case DE_FK_F64:
    case DE_FK_DVEC3:
        for (u32 i = 0; i < size / 8u; ++i) {
            f64 v = random_f64(g);
            memcpy(dst + 8u * i, &v, 8);
        }
        return;
    case DE_FK_STRING:
        random_string(g, dst, size);
        return;
    case DE_FK_ENUM:
    case DE_FK_FLAGS:
        random_enum(g, r, nested, size, k == DE_FK_FLAGS, dst);
        return;
    case DE_FK_STRUCT:
        fill_struct(g, r, de_type_find(r, nested), dst);
        return;
    case DE_FK_FIXED_ARRAY:
        return; /* handled by fill_struct */
    default: {  /* integers, NAME, ENTITY, ASSET: any bits */
        u64 v = rnd64(g);
        memcpy(dst, &v, size);
        return;
    }
    }
}

static void fill_struct(de_rng *g, const de_type_registry *r, de_type_id t, u8 *obj) {
    u32 n = 0;
    const de_field_info *fs = de_type_fields(r, t, &n);
    for (u32 i = 0; i < n; ++i) {
        const de_field_info *f = &fs[i];
        if (f->kind == DE_FK_FIXED_ARRAY) {
            const de_field_info *el = de_type_array_element(r, t, i);
            ASSERT_TRUE(el != NULL);
            for (u32 e = 0; e < f->attrs.array_count; ++e)
                fill_value(g, r, el->kind, el->size, el->attrs.nested_type,
                           obj + f->offset + (size_t)e * el->size);
        } else {
            fill_value(g, r, f->kind, f->size, f->attrs.nested_type, obj + f->offset);
        }
    }
}

/* ---- Object walks: NaN normalization and the required_flags selection ------------------------ */

static void norm_struct(const de_type_registry *r, de_type_id t, u8 *obj);

static void norm_value(const de_type_registry *r, de_field_kind k, u32 size, const char *nested,
                       u8 *p) {
    switch (k) {
    case DE_FK_F32:
    case DE_FK_VEC2:
    case DE_FK_VEC3:
    case DE_FK_VEC4:
    case DE_FK_QUAT:
    case DE_FK_COLOR:
    case DE_FK_MAT4:
        for (u32 i = 0; i < size / 4u; ++i) {
            u32 bits;
            memcpy(&bits, p + 4u * i, 4);
            if ((bits & 0x7f800000u) == 0x7f800000u && (bits & 0x007fffffu) != 0) {
                bits = 0x7fc00000u;
                memcpy(p + 4u * i, &bits, 4);
            }
        }
        return;
    case DE_FK_F64:
    case DE_FK_DVEC3:
        for (u32 i = 0; i < size / 8u; ++i) {
            u64 bits;
            memcpy(&bits, p + 8u * i, 8);
            if ((bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull &&
                (bits & 0x000fffffffffffffull) != 0) {
                bits = 0x7ff8000000000000ull;
                memcpy(p + 8u * i, &bits, 8);
            }
        }
        return;
    case DE_FK_STRUCT:
        norm_struct(r, de_type_find(r, nested), p);
        return;
    default:
        return;
    }
}

/* Replaces every NaN (any sign or payload) by the canonical quiet NaN: the text format keeps
 * "NaN" but not its payload. */
static void norm_struct(const de_type_registry *r, de_type_id t, u8 *obj) {
    u32 n = 0;
    const de_field_info *fs = de_type_fields(r, t, &n);
    for (u32 i = 0; i < n; ++i) {
        const de_field_info *f = &fs[i];
        if (f->kind == DE_FK_FIXED_ARRAY) {
            const de_field_info *el = de_type_array_element(r, t, i);
            for (u32 e = 0; e < f->attrs.array_count; ++e)
                norm_value(r, el->kind, el->size, el->attrs.nested_type,
                           obj + f->offset + (size_t)e * el->size);
        } else {
            norm_value(r, f->kind, f->size, f->attrs.nested_type, obj + f->offset);
        }
    }
}

/* Copies the fields `req` selects from `src` to `dst`: the documented rule of both formats,
 * `(flags & req) == req`, at every struct nesting level (array elements included). */
static void select_struct(const de_type_registry *r, de_type_id t, u32 req, const u8 *src,
                          u8 *dst) {
    u32 n = 0;
    const de_field_info *fs = de_type_fields(r, t, &n);
    for (u32 i = 0; i < n; ++i) {
        const de_field_info *f = &fs[i];
        if ((f->flags & req) != req)
            continue;
        const de_field_info *el =
            f->kind == DE_FK_FIXED_ARRAY ? de_type_array_element(r, t, i) : NULL;
        if (f->kind == DE_FK_STRUCT) {
            select_struct(r, de_type_find(r, f->attrs.nested_type), req, src + f->offset,
                          dst + f->offset);
        } else if (el != NULL && el->kind == DE_FK_STRUCT) {
            de_type_id et = de_type_find(r, el->attrs.nested_type);
            for (u32 e = 0; e < f->attrs.array_count; ++e) {
                size_t at = f->offset + (size_t)e * el->size;
                select_struct(r, et, req, src + at, dst + at);
            }
        } else {
            memcpy(dst + f->offset, src + f->offset, f->size);
        }
    }
}

/* ---- Entity and asset mapping ---------------------------------------------------------------- */

#define PROP_ENTITY_KEY 0x5a5a00ff12345678ull

typedef struct map_state {
    u32 calls, bad_user;
} map_state;

static map_state *g_map_expected;

static void map_check(void *user) {
    map_state *m = (map_state *)user;
    if (m != g_map_expected)
        g_map_expected->bad_user++;
    else
        m->calls++;
}

static u64 map_e2s(u64 bits, void *user) {
    map_check(user);
    return bits ^ PROP_ENTITY_KEY;
}
static u64 map_s2e(u64 id, void *user) {
    map_check(user);
    return id ^ PROP_ENTITY_KEY;
}
static u64 map_a2i(u32 h, const char *type, void *user) {
    (void)type;
    map_check(user);
    return ((u64)h << 20) | 0xABCDEu;
}
static u32 map_i2a(u64 id, const char *type, void *user) {
    (void)type;
    map_check(user);
    return (u32)(id >> 20);
}

/* ---- Serializer calls ------------------------------------------------------------------------ */

typedef struct prop_run {
    const char *test;
    u64 seed;
    u32 type_index, value_index;
    const gen *g;
    const gtype *top;
    de_mem_writer text, bin;
    u8 *a, *b, *c, *d; /* objects of PROP_MAX_OBJECT bytes */
    map_state map;
    de_ser_context ctx;
} prop_run;

static void dump_type(const prop_run *p, const char *name) {
    for (u32 i = 0; i < p->g->count; ++i) {
        const gtype *t = &p->g->types[i];
        if (strcmp(t->name, name) != 0)
            continue;
        fprintf(stderr, "  type %s: size %u align %u, %u fields\n", t->name, t->size, t->align,
                t->field_count);
        for (u32 f = 0; f < t->field_count; ++f) {
            const de_field_info *fi = &t->fields[f];
            fprintf(stderr, "    %s: kind %d offset %u size %u flags 0x%x nested %s count %u\n",
                    fi->name, (int)fi->kind, fi->offset, fi->size, fi->flags,
                    fi->attrs.nested_type ? fi->attrs.nested_type : "-", fi->attrs.array_count);
        }
        for (u32 f = 0; f < t->field_count; ++f) {
            const char *n = t->fields[f].attrs.nested_type;
            if (n != NULL && n[0] == 'S')
                dump_type(p, n);
        }
    }
}

/* Prints everything needed to reproduce the failure, then fails the test. */
static void prop_fail(const prop_run *p, const char *what) {
    const de_error *e = de_error_last();
    fprintf(stderr,
            "prop.%s FAILED: %s\n  seed 0x%llx (rerun: DE_PROP_SEED=0x%llx), type #%u, value "
            "#%u\n  last error: %s\n",
            p->test, what, (unsigned long long)p->seed, (unsigned long long)p->seed, p->type_index,
            p->value_index, e && e->result != DE_OK ? e->message : "none");
    dump_type(p, p->top->name);
    if (p->text.size > 0 && p->text.size < 16384)
        fprintf(stderr, "  text:\n%.*s\n", (int)p->text.size, (const char *)p->text.data);
    FAIL(what);
}

static void prop_check(const prop_run *p, bool ok, const char *what) {
    if (!ok)
        prop_fail(p, what);
}

static de_ser_options opts(const prop_run *p, u32 req, bool ctx) {
    de_ser_options o;
    memset(&o, 0, sizeof o);
    o.format = DE_SER_TEXT;
    o.required_flags = req;
    o.ctx = ctx ? &p->ctx : NULL;
    return o;
}

static void text_write(prop_run *p, const void *obj, const de_ser_options *o) {
    de_mem_writer_reset(&p->text);
    de_writer w = de_writer_from_mem(&p->text);
    prop_check(p, de_serialize_object(p->g->r, p->top->id, obj, o, &w) == DE_OK, "text write");
}

static void text_read(prop_run *p, void *obj, const de_ser_options *o) {
    de_mem_reader mr;
    de_mem_reader_init(&mr, p->text.data, p->text.size);
    de_reader rd = de_reader_from_mem(&mr);
    prop_check(p, de_deserialize_object(p->g->r, p->top->id, obj, o, &rd) == DE_OK, "text read");
}

static void bin_write(prop_run *p, const void *obj, const de_ser_options *o,
                      const de_ser_binary_options *bo) {
    de_mem_writer_reset(&p->bin);
    de_writer w = de_writer_from_mem(&p->bin);
    prop_check(p, de_serialize_binary(p->g->r, p->top->id, obj, o, bo, &w) == DE_OK,
               "binary write");
}

static void bin_read(prop_run *p, void *obj, const de_ser_options *o,
                     const de_ser_binary_options *bo) {
    de_mem_reader mr;
    de_mem_reader_init(&mr, p->bin.data, p->bin.size);
    de_reader rd = de_reader_from_mem(&mr);
    prop_check(p, de_deserialize_binary(p->g->r, p->top->id, obj, o, bo, &rd) == DE_OK,
               "binary read");
    prop_check(p, mr.pos == mr.size, "binary read left bytes of the object unread");
}

/* Fresh (constructed) object of the top type. */
static void construct(prop_run *p, u8 *obj) {
    memset(obj, 0, p->top->size);
    prop_check(p, de_type_construct(p->g->r, p->top->id, obj) == DE_OK, "construct");
}

static void normalize(prop_run *p, u8 *obj) {
    norm_struct(p->g->r, p->top->id, obj);
}

static bool same(const prop_run *p, const u8 *x, const u8 *y) {
    return memcmp(x, y, p->top->size) == 0;
}

/* Runs `body` for PROP_TYPES random types and PROP_VALUES random objects each. */
typedef void (*prop_body)(prop_run *p, de_rng *rng);

static void prop_main(const char *test, prop_body body) {
    prop_run p;
    memset(&p, 0, sizeof p);
    p.test = test;
    p.seed = prop_seed();
    p.a = (u8 *)calloc(1, PROP_MAX_OBJECT);
    p.b = (u8 *)calloc(1, PROP_MAX_OBJECT);
    p.c = (u8 *)calloc(1, PROP_MAX_OBJECT);
    p.d = (u8 *)calloc(1, PROP_MAX_OBJECT);
    gen *g = (gen *)calloc(1, sizeof(gen));
    ASSERT_TRUE(p.a && p.b && p.c && p.d && g);
    de_mem_writer_init(&p.text, NULL);
    de_mem_writer_init(&p.bin, NULL);
    p.ctx.entity_to_scene_id = map_e2s;
    p.ctx.scene_id_to_entity = map_s2e;
    p.ctx.asset_to_id = map_a2i;
    p.ctx.id_to_asset = map_i2a;
    p.ctx.user = &p.map;
    g_map_expected = &p.map;

    /* Types and values come from separate streams of the same seed. */
    de_rng types = de_rng_seed(p.seed, 1);
    de_rng values = de_rng_seed(p.seed, 2);
    u64 structs = 0, fields = 0, bytes = 0;
    u32 heights[4] = {0, 0, 0, 0};
    for (u32 t = 0; t < PROP_TYPES; ++t) {
        p.type_index = t;
        p.value_index = 0;
        p.g = g;
        p.top = gen_type(g, &types);
        structs += g->count;
        heights[p.top->height < 4 ? p.top->height : 3]++;
        for (u32 i = 0; i < g->count; ++i)
            fields += g->types[i].field_count;
        bytes += p.top->size;
        for (u32 v = 0; v < PROP_VALUES; ++v) {
            p.value_index = v;
            body(&p, &values);
        }
        gen_free(g);
    }
    if (p.map.bad_user != 0)
        prop_fail(&p, "a de_ser_context callback did not get ctx->user");
    printf("prop.%s: seed 0x%llx, %u types (%llu structs, %llu fields, %llu object bytes), %u "
           "objects each, %u context calls; nesting depth 0/1/2/3: %u/%u/%u/%u types\n",
           test, (unsigned long long)p.seed, PROP_TYPES, (unsigned long long)structs,
           (unsigned long long)fields, (unsigned long long)bytes, PROP_VALUES, p.map.calls,
           heights[0], heights[1], heights[2], heights[3]);

    de_mem_writer_free(&p.text);
    de_mem_writer_free(&p.bin);
    free(g);
    free(p.a);
    free(p.b);
    free(p.c);
    free(p.d);
}

/* ---- prop.text_roundtrip ----------------------------------------------------------------------
 */

static void text_roundtrip_body(prop_run *p, de_rng *rng) {
    const de_type_registry *r = p->g->r;
    construct(p, p->a);
    fill_struct(rng, r, p->top->id, p->a);
    de_ser_options o = opts(p, 0, p->value_index % 2u == 1u);

    text_write(p, p->a, &o);
    construct(p, p->b);
    text_read(p, p->b, &o);

    /* write -> read -> write gives the same bytes (NaN is always written "nan"). */
    de_mem_writer first = p->text;
    de_mem_writer_init(&p->text, NULL);
    text_write(p, p->b, &o);
    bool same_text =
        first.size == p->text.size && memcmp(first.data, p->text.data, first.size) == 0;
    de_mem_writer_free(&p->text);
    p->text = first;
    prop_check(p, same_text, "text write -> read -> write changed the text");

    memcpy(p->c, p->a, p->top->size);
    normalize(p, p->c);
    normalize(p, p->b);
    prop_check(p, same(p, p->b, p->c), "text round trip changed the object");
}

TEST(prop, text_roundtrip) {
    prop_main("text_roundtrip", text_roundtrip_body);
}

/* ---- prop.binary_roundtrip --------------------------------------------------------------------
 */

static void binary_roundtrip_body(prop_run *p, de_rng *rng) {
    const de_type_registry *r = p->g->r;
    construct(p, p->a);
    fill_struct(rng, r, p->top->id, p->a);
    de_ser_options o = opts(p, 0, p->value_index % 3u == 2u);
    o.format = DE_SER_BINARY;
    de_ser_binary_options bo;
    memset(&bo, 0, sizeof bo);
    bo.schema = p->value_index % 2u ? DE_SER_BINARY_SELF_DESCRIBING : DE_SER_BINARY_COMPACT;

    bin_write(p, p->a, &o, &bo);
    construct(p, p->b);
    bin_read(p, p->b, &o, &bo);
    prop_check(p, same(p, p->a, p->b), "binary round trip changed the object bytes");

    /* The field-wise path gives the same bytes and the same object as the fast path. */
    de_mem_writer first = p->bin;
    de_mem_writer_init(&p->bin, NULL);
    de_ser_binary_options slow = bo;
    slow.disable_fast_path = true;
    bin_write(p, p->b, &o, &slow);
    bool same_bytes = first.size == p->bin.size && memcmp(first.data, p->bin.data, first.size) == 0;
    construct(p, p->c);
    bin_read(p, p->c, &o, &slow);
    de_mem_writer_free(&p->bin);
    p->bin = first;
    prop_check(p, same_bytes, "binary write -> read -> write (field-wise) changed the bytes");
    prop_check(p, same(p, p->a, p->c), "field-wise binary read changed the object bytes");

    /* Through the object API (DE_SER_BINARY dispatch, default binary options). */
    de_mem_writer_reset(&p->bin);
    de_writer w = de_writer_from_mem(&p->bin);
    prop_check(p, de_serialize_object(r, p->top->id, p->a, &o, &w) == DE_OK, "object API write");
    de_mem_reader mr;
    de_mem_reader_init(&mr, p->bin.data, p->bin.size);
    de_reader rd = de_reader_from_mem(&mr);
    construct(p, p->d);
    prop_check(p, de_deserialize_object(r, p->top->id, p->d, &o, &rd) == DE_OK, "object API read");
    prop_check(p, same(p, p->a, p->d), "object API binary round trip changed the object bytes");
}

TEST(prop, binary_roundtrip) {
    prop_main("binary_roundtrip", binary_roundtrip_body);
}

/* ---- prop.text_binary_equivalence -------------------------------------------------------------
 */

static void equivalence_body(prop_run *p, de_rng *rng) {
    const de_type_registry *r = p->g->r;
    construct(p, p->a);
    fill_struct(rng, r, p->top->id, p->a);
    bool ctx = p->value_index % 2u == 0u;
    de_ser_binary_options bo;
    memset(&bo, 0, sizeof bo);
    bo.schema = p->value_index % 4u < 2u ? DE_SER_BINARY_SELF_DESCRIBING : DE_SER_BINARY_COMPACT;

    /* text -> object -> binary -> object equals the original. */
    de_ser_options o = opts(p, 0, ctx);
    text_write(p, p->a, &o);
    construct(p, p->b);
    text_read(p, p->b, &o);
    bin_write(p, p->b, &o, &bo);
    construct(p, p->c);
    bin_read(p, p->c, &o, &bo);
    prop_check(p, same(p, p->b, p->c), "binary changed the object read from text");
    memcpy(p->d, p->a, p->top->size);
    normalize(p, p->d);
    normalize(p, p->c);
    prop_check(p, same(p, p->c, p->d),
               "text -> object -> binary -> object differs from the original");

    /* Both formats load the same fields under a filter: into the same prefilled object, the
     * selected fields of the original replace the background, the others stay. */
    static const u32 k_req[] = {DE_FIELD_SERIALIZE, DE_FIELD_REPLICATE,
                                DE_FIELD_SERIALIZE | DE_FIELD_SNAPSHOT, DE_FIELD_EDITOR_HIDDEN};
    u32 req = k_req[rnd(rng, PROP_COUNT_OF(k_req))];
    o = opts(p, req, ctx);
    construct(p, p->d);
    fill_struct(rng, r, p->top->id, p->d); /* the background */

    memcpy(p->b, p->d, p->top->size);
    text_write(p, p->a, &o);
    text_read(p, p->b, &o);
    memcpy(p->c, p->d, p->top->size);
    bin_write(p, p->a, &o, &bo);
    bin_read(p, p->c, &o, &bo);
    select_struct(r, p->top->id, req, p->a, p->d); /* the expected object */
    normalize(p, p->b);
    normalize(p, p->c);
    normalize(p, p->d);
    prop_check(p, same(p, p->b, p->c), "text and binary loaded different fields under a filter");
    prop_check(p, same(p, p->c, p->d), "the filtered load differs from the required_flags rule");
}

TEST(prop, text_binary_equivalence) {
    prop_main("text_binary_equivalence", equivalence_body);
}

/* ---- Regressions found by the property tests ---------------------------------------------- */

/* Leftover FLAGS bits above INT64_MAX are written as a "0x..." string (TOML integers are signed
 * 64-bit); the reader refused that string as an unknown flag name (DENG-297, seed 0x297). */
TEST(prop, flags_high_bits_roundtrip) {
    de_type_registry *r = NULL;
    ASSERT_EQ(de_type_registry_create(NULL, &r), DE_OK);
    ASSERT_EQ(de_enum_register(r, "F", k_f_values, PROP_COUNT_OF(k_f_values), true), DE_OK);
    typedef struct TWide {
        u64 bits;
        u8 small;
    } TWide;
    static const de_field_info k_fields[] = {
        DE_FIELD_EX(TWide, bits, DE_FK_FLAGS, DE_FIELD_DEFAULT, {0, 0, 0, 0, "F", 0}),
        DE_FIELD_EX(TWide, small, DE_FK_FLAGS, DE_FIELD_DEFAULT, {0, 0, 0, 0, "F", 0}),
    };
    de_type_id id = de_type_register(r, &DE_TYPE_DESC(TWide, 1, k_fields));
    ASSERT_NE(id.id, 0u);

    TWide in = {0x8000000000000005ull, 0x83u}, out = {0, 0};
    de_mem_writer mw;
    de_mem_writer_init(&mw, NULL);
    de_writer w = de_writer_from_mem(&mw);
    ASSERT_EQ(de_serialize_object(r, id, &in, NULL, &w), DE_OK);
    const char *expect = "bits = [\"A\", \"C\", \"0x8000000000000000\"]";
    bool found = false;
    for (size_t i = 0; !found && i + strlen(expect) <= mw.size; ++i)
        found = memcmp(mw.data + i, expect, strlen(expect)) == 0;
    ASSERT_TRUE(found);
    de_mem_reader mr;
    de_mem_reader_init(&mr, mw.data, mw.size);
    de_reader rd = de_reader_from_mem(&mr);
    ASSERT_EQ(de_deserialize_object(r, id, &out, NULL, &rd), DE_OK);
    ASSERT_EQ(out.bits, in.bits);
    ASSERT_EQ(out.small, in.small);

    /* A hex string that does not fit the field is a parse error, not a truncation. */
    static const char k_bad[] = "small = [\"0x100\"]\n";
    de_mem_reader_init(&mr, k_bad, sizeof k_bad - 1);
    rd = de_reader_from_mem(&mr);
    ASSERT_EQ(de_deserialize_object(r, id, &out, NULL, &rd), DE_ERR_PARSE);
    de_error_clear();

    de_mem_writer_free(&mw);
    de_type_registry_destroy(r);
}
