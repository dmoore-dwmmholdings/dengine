/*
 * ser_text_reader.c: the TOML text reader (DENG-295, SPEC §9.2): de_deserialize_object and the
 * text document reader (de_text_doc_*).
 *
 * The text is parsed by the vendored tomlc17 through core's entry point (de__toml_parse), so the
 * parser's blocks come from the caller's allocator. The tree is then walked in document order and
 * each key is matched by name against the type's fields. The rules (lenient inputs, errors,
 * versions, the migrate data) are documented in de_reflect.h ("Text format").
 *
 * Modes: a document with the type's current version is read strictly (the first bad value is an
 * error). An older one is read leniently: values that no longer fit are skipped, then the migrate
 * hook (if any) gets every stored value as synthesized fields over a scratch blob.
 */
#include "ser_internal.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum read_mode {
    MODE_STRICT,  /* current version: errors stop the read */
    MODE_MIGRATE, /* older version, migrate hook: skip quietly, the hook sees the old data */
    MODE_LENIENT  /* older version, no hook: skip with a warning */
} read_mode;

typedef struct rctx {
    const de_type_registry *r;
    const de_ser_options *opt;
    const char *source;
    const char *type_name;
    read_mode mode;
    de__ser_path path;
} rctx;

/* ---- Messages ------------------------------------------------------------------------------ */

static const char *toml_type_name(toml_type_t t) {
    switch (t) {
    case TOML_STRING:
        return "a string";
    case TOML_INT64:
        return "an integer";
    case TOML_FP64:
        return "a float";
    case TOML_BOOLEAN:
        return "a boolean";
    case TOML_DATE:
    case TOML_TIME:
    case TOML_DATETIME:
    case TOML_DATETIMETZ:
        return "a date or time";
    case TOML_ARRAY:
        return "an array";
    case TOML_TABLE:
        return "a table";
    default:
        return "an unknown value";
    }
}

/* Formats "<source>: line N: type T, field <path>: <what>". */
static void describe(rctx *c, int line, char *buf, size_t cap, const char *fmt, va_list ap) {
    int n;
    if (line > 0)
        n = snprintf(buf, cap, "%s: line %d: type %s, field %s: ", c->source, line, c->type_name,
                     de__ser_path_str(&c->path));
    else
        n = snprintf(buf, cap, "%s: type %s, field %s: ", c->source, c->type_name,
                     de__ser_path_str(&c->path));
    if (n < 0 || (size_t)n >= cap)
        return;
    vsnprintf(buf + n, cap - (size_t)n, fmt, ap);
}

static de_result fail(rctx *c, const toml_datum_t *v, de_result code, const char *fmt, ...)
    DE_PRINTF_FMT(4, 5);
static de_result fail(rctx *c, const toml_datum_t *v, de_result code, const char *fmt, ...) {
    char buf[DE_ERROR_MESSAGE_MAX];
    va_list ap;
    va_start(ap, fmt);
    describe(c, v ? v->lineno : 0, buf, sizeof buf, fmt, ap);
    va_end(ap);
    return DE_FAIL(code, "%s", buf);
}

static void warn(rctx *c, const toml_datum_t *v, const char *fmt, ...) DE_PRINTF_FMT(3, 4);
static void warn(rctx *c, const toml_datum_t *v, const char *fmt, ...) {
    char buf[DE_LOG_MESSAGE_MAX];
    va_list ap;
    va_start(ap, fmt);
    describe(c, v ? v->lineno : 0, buf, sizeof buf, fmt, ap);
    va_end(ap);
    DE_WARN(de__ser_log_category(), "%s", buf);
}

static de_result mismatch(rctx *c, const toml_datum_t *v, const char *expected) {
    return fail(c, v, DE_ERR_PARSE, "expected %s, got %s", expected, toml_type_name(v->type));
}

/* ---- Scalars ------------------------------------------------------------------------------- */

static void store_uint(u8 *p, u32 size, u64 v) {
    switch (size) {
    case 1:
        p[0] = (u8)v;
        break;
    case 2: {
        u16 x = (u16)v;
        memcpy(p, &x, 2);
        break;
    }
    case 4: {
        u32 x = (u32)v;
        memcpy(p, &x, 4);
        break;
    }
    default:
        memcpy(p, &v, 8);
        break;
    }
}

static bool is_number(const toml_datum_t *v) {
    return v->type == TOML_INT64 || v->type == TOML_FP64;
}

static f64 number_value(const toml_datum_t *v) {
    return v->type == TOML_INT64 ? (f64)v->u.int64 : v->u.fp64;
}

static de_result read_f32(rctx *c, const toml_datum_t *v, u8 *dst) {
    if (!is_number(v))
        return mismatch(c, v, "a float");
    f32 x;
    if (!de__ser_f64_to_f32(number_value(v), &x))
        return fail(c, v, DE_ERR_PARSE, "%g is out of range for an f32", number_value(v));
    memcpy(dst, &x, 4);
    return DE_OK;
}

static de_result read_f64(rctx *c, const toml_datum_t *v, u8 *dst) {
    if (!is_number(v))
        return mismatch(c, v, "a float");
    f64 x = number_value(v);
    memcpy(dst, &x, 8);
    return DE_OK;
}

/* A u64 from a non-negative integer or a "0x..." string. */
static de_result read_u64_value(rctx *c, const toml_datum_t *v, u64 *out) {
    if (v->type == TOML_INT64) {
        if (v->u.int64 < 0)
            return fail(c, v, DE_ERR_PARSE, "%lld is out of range for an unsigned value",
                        (long long)v->u.int64);
        *out = (u64)v->u.int64;
        return DE_OK;
    }
    if (v->type == TOML_STRING) {
        if (de__ser_parse_hex64(v->u.str.ptr, (size_t)v->u.str.len, out))
            return DE_OK;
        return fail(c, v, DE_ERR_PARSE, "\"%.40s\" is not a \"0x...\" hex value", v->u.str.ptr);
    }
    return mismatch(c, v, "an integer or a \"0x...\" string");
}

static de_result read_int(rctx *c, const de_field_info *f, const toml_datum_t *v, u8 *dst) {
    if (f->kind == DE_FK_U64) {
        u64 x;
        DE_TRY(read_u64_value(c, v, &x));
        memcpy(dst, &x, 8);
        return DE_OK;
    }
    if (v->type != TOML_INT64)
        return mismatch(c, v, "an integer");
    i64 x = v->u.int64;
    i64 lo, hi;
    switch (f->kind) {
    case DE_FK_I8:
        lo = INT8_MIN, hi = INT8_MAX;
        break;
    case DE_FK_I16:
        lo = INT16_MIN, hi = INT16_MAX;
        break;
    case DE_FK_I32:
        lo = INT32_MIN, hi = INT32_MAX;
        break;
    case DE_FK_U8:
        lo = 0, hi = UINT8_MAX;
        break;
    case DE_FK_U16:
        lo = 0, hi = UINT16_MAX;
        break;
    case DE_FK_U32:
        lo = 0, hi = (i64)UINT32_MAX;
        break;
    default: /* I64 */
        lo = INT64_MIN, hi = INT64_MAX;
        break;
    }
    if (x < lo || x > hi)
        return fail(c, v, DE_ERR_PARSE, "%lld is out of range [%lld, %lld]", (long long)x,
                    (long long)lo, (long long)hi);
    store_uint(dst, f->size, (u64)x);
    return DE_OK;
}

/* An integer that fits `size` bytes as either a signed or an unsigned value. */
static bool fits_bytes(i64 x, u32 size) {
    if (size >= 8)
        return true;
    i64 lo = -((i64)1 << (size * 8u - 1u));
    i64 hi = ((i64)1 << (size * 8u)) - 1;
    return x >= lo && x <= hi;
}

static const de_enum_info *enum_of(rctx *c, const de_field_info *f) {
    const de__type_entry *e = de__reflect_find_entry(c->r, f->attrs.nested_type);
    return (e && e->kind == DE__ENTRY_ENUM) ? e->enum_info : NULL;
}

static bool enum_lookup(const de_enum_info *info, const char *name, i64 *out) {
    for (u32 i = 0; info && i < info->count; ++i) {
        if (strcmp(info->values[i].name, name) == 0) {
            *out = info->values[i].value;
            return true;
        }
    }
    return false;
}

/* One ENUM value, or one FLAGS array item: a value name, an integer, or a "0x..." string (the
 * writer's form for leftover flag bits above INT64_MAX; DENG-297). */
static de_result read_enum_item(rctx *c, const de_field_info *f, const de_enum_info *info,
                                const toml_datum_t *v, i64 *out) {
    if (v->type == TOML_STRING) {
        if (enum_lookup(info, v->u.str.ptr, out))
            return DE_OK;
        u64 bits;
        if (de__ser_parse_hex64(v->u.str.ptr, (size_t)v->u.str.len, &bits)) {
            if (f->size < 8 && bits >> (f->size * 8u) != 0)
                return fail(c, v, DE_ERR_PARSE, "%.40s does not fit %u bytes", v->u.str.ptr,
                            (unsigned)f->size);
            *out = (i64)bits;
            return DE_OK;
        }
        return fail(c, v, DE_ERR_PARSE, "\"%.64s\" is not a value of %s %s", v->u.str.ptr,
                    f->kind == DE_FK_FLAGS ? "flags" : "enum", f->attrs.nested_type);
    }
    if (v->type == TOML_INT64) {
        if (!fits_bytes(v->u.int64, f->size))
            return fail(c, v, DE_ERR_PARSE, "%lld does not fit %u bytes", (long long)v->u.int64,
                        (unsigned)f->size);
        *out = v->u.int64;
        return DE_OK;
    }
    return mismatch(c, v, f->kind == DE_FK_FLAGS ? "a flag name" : "an enum value name");
}

static de_result read_enum(rctx *c, const de_field_info *f, const toml_datum_t *v, u8 *dst) {
    const de_enum_info *info = enum_of(c, f);
    if (f->kind == DE_FK_ENUM) {
        i64 x;
        DE_TRY(read_enum_item(c, f, info, v, &x));
        store_uint(dst, f->size, (u64)x);
        return DE_OK;
    }
    if (v->type == TOML_INT64) {
        i64 x;
        DE_TRY(read_enum_item(c, f, info, v, &x));
        store_uint(dst, f->size, (u64)x);
        return DE_OK;
    }
    if (v->type != TOML_ARRAY)
        return mismatch(c, v, "an array of flag names");
    u64 bits = 0;
    for (int32_t i = 0; i < v->u.arr.size; ++i) {
        size_t at = de__ser_path_push_index(&c->path, (u32)i);
        i64 x = 0;
        de_result res = read_enum_item(c, f, info, &v->u.arr.elem[i], &x);
        de__ser_path_pop(&c->path, at);
        if (res != DE_OK)
            return res;
        bits |= (u64)x;
    }
    store_uint(dst, f->size, bits);
    return DE_OK;
}

static de_result read_string(rctx *c, const de_field_info *f, const toml_datum_t *v, u8 *dst) {
    if (v->type != TOML_STRING)
        return mismatch(c, v, "a string");
    size_t len = (size_t)v->u.str.len;
    if (memchr(v->u.str.ptr, '\0', len) != NULL)
        return fail(c, v, DE_ERR_PARSE, "the string holds a NUL character");
    if (len + 1 > f->size)
        return fail(c, v, DE_ERR_PARSE,
                    "a string of %zu bytes does not fit %u bytes (NUL included)", len,
                    (unsigned)f->size);
    memcpy(dst, v->u.str.ptr, len);
    memset(dst + len, 0, f->size - len);
    return DE_OK;
}

static de_result read_floats(rctx *c, const toml_datum_t *v, u32 n, bool f64s, u8 *dst) {
    if (v->type != TOML_ARRAY)
        return mismatch(c, v, "an array of floats");
    if (v->u.arr.size != (int32_t)n)
        return fail(c, v, DE_ERR_PARSE, "expected %u floats, got %d", (unsigned)n,
                    (int)v->u.arr.size);
    for (u32 i = 0; i < n; ++i) {
        size_t at = de__ser_path_push_index(&c->path, i);
        de_result res = f64s ? read_f64(c, &v->u.arr.elem[i], dst + 8u * i)
                             : read_f32(c, &v->u.arr.elem[i], dst + 4u * i);
        de__ser_path_pop(&c->path, at);
        if (res != DE_OK)
            return res;
    }
    return DE_OK;
}

/* ---- Entities and assets ------------------------------------------------------------------- */

static de_result read_entity(rctx *c, const toml_datum_t *v, u8 *dst) {
    u64 id;
    DE_TRY(read_u64_value(c, v, &id));
    const de_ser_context *ctx = c->opt->ctx;
    u64 bits = (ctx && ctx->scene_id_to_entity) ? ctx->scene_id_to_entity(id, ctx->user) : id;
    memcpy(dst, &bits, 8);
    return DE_OK;
}

/* The asset ID from a "0x..." string, or from a path (de_hash64 of its bytes). */
static de_result asset_id_of_string(rctx *c, const toml_datum_t *v, bool path_only, u64 *out) {
    if (!path_only && de__ser_parse_hex64(v->u.str.ptr, (size_t)v->u.str.len, out))
        return DE_OK;
    if (v->u.str.len == 0)
        return fail(c, v, DE_ERR_PARSE, "an empty asset path");
    *out = de_hash64(v->u.str.ptr, (size_t)v->u.str.len, 0);
    return DE_OK;
}

static de_result read_asset(rctx *c, const de_field_info *f, const toml_datum_t *v, u8 *dst) {
    u64 id = 0;
    if (v->type == TOML_STRING) {
        DE_TRY(asset_id_of_string(c, v, false, &id));
    } else if (v->type == TOML_TABLE) {
        const toml_datum_t *idv = NULL, *pathv = NULL;
        for (int32_t i = 0; i < v->u.tab.size; ++i) {
            const char *k = v->u.tab.key[i];
            if (strcmp(k, "id") == 0)
                idv = &v->u.tab.value[i];
            else if (strcmp(k, "path") == 0)
                pathv = &v->u.tab.value[i];
            else
                warn(c, &v->u.tab.value[i], "unknown asset key '%s' skipped", k);
        }
        if (idv != NULL) {
            DE_TRY(read_u64_value(c, idv, &id));
        } else if (pathv != NULL) {
            if (pathv->type != TOML_STRING)
                return mismatch(c, pathv, "an asset path string");
            DE_TRY(asset_id_of_string(c, pathv, true, &id));
        } else {
            return fail(c, v, DE_ERR_PARSE, "an asset needs an 'id' or a 'path'");
        }
    } else {
        return mismatch(c, v, "an asset table { id = \"0x...\" }");
    }
    const de_ser_context *ctx = c->opt->ctx;
    u32 h;
    if (ctx && ctx->id_to_asset) {
        h = ctx->id_to_asset(id, f->attrs.nested_type, ctx->user);
    } else {
        if (id > UINT32_MAX)
            return fail(c, v, DE_ERR_PARSE,
                        "asset ID 0x%016llx does not fit a raw handle (no de_ser_context)",
                        (unsigned long long)id);
        h = (u32)id;
    }
    memcpy(dst, &h, 4);
    return DE_OK;
}

/* ---- Structs and arrays -------------------------------------------------------------------- */

static de_result read_struct(rctx *c, const de__type_layout *layout, const toml_datum_t *tab,
                             u8 *base, bool top);

static de_result read_value(rctx *c, const de_field_info *f, const de__field_extra *ex,
                            const toml_datum_t *v, u8 *dst) {
    switch (f->kind) {
    case DE_FK_BOOL:
        if (v->type != TOML_BOOLEAN)
            return mismatch(c, v, "a boolean");
        dst[0] = v->u.boolean ? 1 : 0;
        return DE_OK;
    case DE_FK_I8:
    case DE_FK_I16:
    case DE_FK_I32:
    case DE_FK_I64:
    case DE_FK_U8:
    case DE_FK_U16:
    case DE_FK_U32:
    case DE_FK_U64:
        return read_int(c, f, v, dst);
    case DE_FK_F32:
        return read_f32(c, v, dst);
    case DE_FK_F64:
        return read_f64(c, v, dst);
    case DE_FK_VEC2:
    case DE_FK_VEC3:
    case DE_FK_VEC4:
    case DE_FK_QUAT:
    case DE_FK_COLOR:
    case DE_FK_MAT4:
        return read_floats(c, v, f->size / 4u, false, dst);
    case DE_FK_DVEC3:
        return read_floats(c, v, 3, true, dst);
    case DE_FK_NAME: {
        if (v->type != TOML_STRING)
            return mismatch(c, v, "a name string");
        de_name n;
        if (!de__ser_parse_hex64(v->u.str.ptr, (size_t)v->u.str.len, &n.hash))
            n = de_name_from_view((de_str){v->u.str.ptr, (size_t)v->u.str.len});
        memcpy(dst, &n.hash, 8);
        return DE_OK;
    }
    case DE_FK_STRING:
        return read_string(c, f, v, dst);
    case DE_FK_ENTITY:
        return read_entity(c, v, dst);
    case DE_FK_ASSET:
        return read_asset(c, f, v, dst);
    case DE_FK_ENUM:
    case DE_FK_FLAGS:
        return read_enum(c, f, v, dst);
    case DE_FK_STRUCT: {
        const de__type_layout *sub = de__reflect_find_layout(c->r, f->attrs.nested_type);
        if (sub == NULL)
            return fail(c, v, DE_ERR_INVALID_ARGUMENT, "nested type %s is not registered",
                        f->attrs.nested_type);
        if (v->type != TOML_TABLE)
            return mismatch(c, v, "a table");
        return read_struct(c, sub, v, dst, false);
    }
    case DE_FK_FIXED_ARRAY: {
        if (v->type != TOML_ARRAY)
            return mismatch(c, v, "an array");
        u32 n = f->attrs.array_count;
        if (v->u.arr.size > (int32_t)n)
            return fail(c, v, DE_ERR_PARSE, "%d elements do not fit an array of %u",
                        (int)v->u.arr.size, (unsigned)n);
        const de_field_info *el = &ex->element;
        /* Fewer elements than the array holds: the rest keep their values. */
        for (int32_t i = 0; i < v->u.arr.size; ++i) {
            size_t at = de__ser_path_push_index(&c->path, (u32)i);
            de_result res = read_value(c, el, NULL, &v->u.arr.elem[i], dst + (size_t)i * el->size);
            de__ser_path_pop(&c->path, at);
            if (res != DE_OK)
                return res;
        }
        return DE_OK;
    }
    }
    return fail(c, v, DE_ERR_INVALID_ARGUMENT, "unknown field kind %d", (int)f->kind);
}

static i64 find_field(const de__type_layout *layout, const char *name) {
    for (u32 i = 0; i < layout->desc.field_count; ++i)
        if (strcmp(layout->fields[i].name, name) == 0)
            return (i64)i;
    return -1;
}

static bool is_header_key(const char *k) {
    return strcmp(k, "__type") == 0 || strcmp(k, "__version") == 0;
}

static de_result read_struct(rctx *c, const de__type_layout *layout, const toml_datum_t *tab,
                             u8 *base, bool top) {
    u32 req = c->opt->required_flags;
    for (int32_t k = 0; k < tab->u.tab.size; ++k) {
        const char *key = tab->u.tab.key[k];
        const toml_datum_t *v = &tab->u.tab.value[k];
        if (top && is_header_key(key))
            continue;
        size_t at = de__ser_path_push_name(&c->path, key);
        i64 i = find_field(layout, key);
        de_result res = DE_OK;
        if (i < 0) {
            /* The migrate hook gets old fields itself: no warning for them. */
            if (c->mode != MODE_MIGRATE)
                warn(c, v, "unknown field skipped");
        } else {
            const de_field_info *f = &layout->fields[i];
            if ((f->flags & req) == req)
                res = read_value(c, f, &layout->extra[i], v, base + f->offset);
        }
        de__ser_path_pop(&c->path, at);
        if (res != DE_OK) {
            if (c->mode == MODE_STRICT)
                return res;
            if (c->mode == MODE_LENIENT) {
                const de_error *e = de_error_last();
                DE_WARN(de__ser_log_category(), "%s (skipped)", e ? e->message : "bad value");
            }
        }
    }
    return DE_OK;
}

/* ---- Migrate data -------------------------------------------------------------------------- */

/*
 * Builds the synthesized old fields (see de_reflect.h). Two passes over the tree: the first
 * (`fields` NULL) only counts fields, name bytes and blob bytes; the second fills the memory the
 * first one sized.
 */
typedef struct mig {
    de_field_info *fields;
    char *names;
    u8 *blob;
    u32 count;
    size_t names_len;
    size_t blob_len;
    char name[DE__SER_PATH_CAP];
    size_t name_len;
} mig;

static size_t align8(size_t n) {
    return (n + 7u) & ~(size_t)7u;
}

static bool mig_push(mig *m, const char *s, size_t n, bool dot) {
    size_t need = m->name_len + (dot ? 1u : 0u) + n;
    if (need + 1 > sizeof m->name)
        return false;
    if (dot)
        m->name[m->name_len++] = '.';
    memcpy(m->name + m->name_len, s, n);
    m->name_len += n;
    m->name[m->name_len] = '\0';
    return true;
}

/* Reserves a field of `size` bytes; returns its blob memory (NULL in the counting pass). */
static u8 *mig_add(mig *m, de_field_kind kind, size_t size, const char *elem, u32 count) {
    size_t off = align8(m->blob_len);
    u8 *mem = NULL;
    if (m->fields != NULL) {
        de_field_info *f = &m->fields[m->count];
        memset(f, 0, sizeof *f);
        memcpy(m->names + m->names_len, m->name, m->name_len + 1);
        f->name = m->names + m->names_len;
        f->offset = (u32)off;
        f->size = (u32)size;
        f->kind = kind;
        f->attrs.nested_type = elem;
        f->attrs.array_count = count;
        mem = m->blob + off;
    }
    m->count++;
    m->names_len += m->name_len + 1;
    m->blob_len = off + size;
    return mem;
}

static bool is_hex_id(const toml_datum_t *v) {
    u64 ignored;
    return v->type == TOML_STRING &&
           de__ser_parse_hex64(v->u.str.ptr, (size_t)v->u.str.len, &ignored);
}

static void mig_table(mig *m, const toml_datum_t *tab, bool top);

static void mig_value(mig *m, const toml_datum_t *v) {
    u8 *mem;
    switch (v->type) {
    case TOML_BOOLEAN:
        if ((mem = mig_add(m, DE_FK_BOOL, 1, NULL, 0)) != NULL)
            mem[0] = v->u.boolean ? 1 : 0;
        return;
    case TOML_INT64:
        if ((mem = mig_add(m, DE_FK_I64, 8, NULL, 0)) != NULL)
            memcpy(mem, &v->u.int64, 8);
        return;
    case TOML_FP64:
        if ((mem = mig_add(m, DE_FK_F64, 8, NULL, 0)) != NULL)
            memcpy(mem, &v->u.fp64, 8);
        return;
    case TOML_STRING:
        if (is_hex_id(v)) {
            u64 x = 0;
            de__ser_parse_hex64(v->u.str.ptr, (size_t)v->u.str.len, &x);
            if ((mem = mig_add(m, DE_FK_U64, 8, NULL, 0)) != NULL)
                memcpy(mem, &x, 8);
        } else if ((mem = mig_add(m, DE_FK_STRING, (size_t)v->u.str.len + 1, NULL, 0)) != NULL) {
            memcpy(mem, v->u.str.ptr, (size_t)v->u.str.len + 1);
        }
        return;
    case TOML_TABLE:
        mig_table(m, v, false);
        return;
    case TOML_ARRAY: {
        int32_t n = v->u.arr.size;
        bool all_tables = n > 0, all_int = true, all_num = true, all_bool = true, all_str = true;
        size_t longest = 0;
        for (int32_t i = 0; i < n; ++i) {
            const toml_datum_t *e = &v->u.arr.elem[i];
            all_tables = all_tables && e->type == TOML_TABLE;
            all_int = all_int && e->type == TOML_INT64;
            all_num = all_num && is_number(e);
            all_bool = all_bool && e->type == TOML_BOOLEAN;
            all_str = all_str && e->type == TOML_STRING;
            if (e->type == TOML_STRING && (size_t)e->u.str.len > longest)
                longest = (size_t)e->u.str.len;
        }
        if (all_tables) {
            size_t at = m->name_len;
            for (int32_t i = 0; i < n; ++i) {
                char idx[16];
                int k = snprintf(idx, sizeof idx, "[%d]", (int)i);
                if (mig_push(m, idx, (size_t)k, false))
                    mig_table(m, &v->u.arr.elem[i], false);
                m->name_len = at;
                m->name[at] = '\0';
            }
            return;
        }
        u32 count = (u32)n;
        if (all_int || all_num) { /* an empty array lands here, as i64[0] */
            if ((mem = mig_add(m, DE_FK_FIXED_ARRAY, 8u * count, all_int ? "i64" : "f64", count)) !=
                NULL) {
                for (u32 i = 0; i < count; ++i) {
                    const toml_datum_t *e = &v->u.arr.elem[i];
                    if (all_int)
                        memcpy(mem + 8u * i, &e->u.int64, 8);
                    else {
                        f64 x = number_value(e);
                        memcpy(mem + 8u * i, &x, 8);
                    }
                }
            }
        } else if (all_bool) {
            if ((mem = mig_add(m, DE_FK_FIXED_ARRAY, count, "bool", count)) != NULL)
                for (u32 i = 0; i < count; ++i)
                    mem[i] = v->u.arr.elem[i].u.boolean ? 1 : 0;
        } else if (all_str) {
            size_t esz = longest + 1;
            if ((mem = mig_add(m, DE_FK_FIXED_ARRAY, esz * count, "string", count)) != NULL) {
                memset(mem, 0, esz * count);
                for (u32 i = 0; i < count; ++i)
                    memcpy(mem + esz * i, v->u.arr.elem[i].u.str.ptr,
                           (size_t)v->u.arr.elem[i].u.str.len);
            }
        }
        /* Mixed arrays are not passed. */
        return;
    }
    default: /* dates and times */
        return;
    }
}

static void mig_table(mig *m, const toml_datum_t *tab, bool top) {
    for (int32_t k = 0; k < tab->u.tab.size; ++k) {
        const char *key = tab->u.tab.key[k];
        if (top && is_header_key(key))
            continue;
        size_t at = m->name_len;
        if (mig_push(m, key, (size_t)tab->u.tab.len[k], at != 0))
            mig_value(m, &tab->u.tab.value[k]);
        m->name_len = at;
        m->name[at] = '\0';
    }
}

static de_result run_migrate(rctx *c, const de_type_desc *desc, const toml_datum_t *tab, void *obj,
                             u32 from_version, const de_allocator *a) {
    mig m;
    memset(&m, 0, sizeof m);
    mig_table(&m, tab, true); /* counting pass */
    size_t fields_bytes = align8(sizeof(de_field_info) * m.count);
    size_t blob_off = fields_bytes;
    size_t names_off = blob_off + align8(m.blob_len);
    size_t total = names_off + m.names_len + 1;
    u8 *mem = (u8 *)DE_ALLOC(a, total, 16, DE_MEM_GENERAL);
    if (mem == NULL)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "%s: type %s: no memory for the migrate data",
                            c->source, c->type_name);
    memset(mem, 0, total);
    u32 count = m.count;
    size_t blob_len = m.blob_len;
    memset(&m, 0, sizeof m);
    m.fields = (de_field_info *)(void *)mem;
    m.blob = mem + blob_off;
    m.names = (char *)mem + names_off;
    mig_table(&m, tab, true); /* filling pass */
    DE_ASSERT(m.count == count && m.blob_len == blob_len);
    (void)count;
    (void)blob_len;
    bool keep = desc->hooks.migrate(obj, from_version, m.blob, m.fields, m.count);
    DE_FREE(a, mem, total);
    if (!keep)
        return DE_FAIL(DE_ERR_VERSION_MISMATCH,
                       "%s: type %s: the migrate hook dropped the version %u object", c->source,
                       c->type_name, (unsigned)from_version);
    return DE_OK;
}

/* ---- Object entry point -------------------------------------------------------------------- */

de_result de__ser_text_read_table(const de_type_registry *r, de_type_id type, void *obj,
                                  const de_ser_options *opt, const toml_datum_t *tab,
                                  const de_allocator *a, const char *source) {
    const de__type_entry *e = de__reflect_entry(r, type);
    if (e == NULL || e->kind != DE__ENTRY_STRUCT)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: invalid type id 0x%x", source,
                       (unsigned)type.id);
    if (obj == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: object is NULL", source);
    const de_type_desc *desc = &e->cur->desc;
    rctx c;
    memset(&c, 0, sizeof c);
    c.r = r;
    c.opt = opt;
    c.source = source;
    c.type_name = desc->name;
    c.mode = MODE_STRICT;
    if (tab->type != TOML_TABLE)
        return fail(&c, tab, DE_ERR_PARSE, "expected a table, got %s", toml_type_name(tab->type));

    u32 version = desc->version;
    for (int32_t k = 0; k < tab->u.tab.size; ++k) {
        const char *key = tab->u.tab.key[k];
        const toml_datum_t *v = &tab->u.tab.value[k];
        if (strcmp(key, "__type") == 0) {
            if (v->type != TOML_STRING)
                return mismatch(&c, v, "the type name string");
            if (strcmp(v->u.str.ptr, desc->name) != 0)
                return fail(&c, v, DE_ERR_PARSE, "the text holds type \"%.64s\"", v->u.str.ptr);
        } else if (strcmp(key, "__version") == 0) {
            if (v->type != TOML_INT64)
                return mismatch(&c, v, "the version integer");
            if (v->u.int64 < 0 || v->u.int64 > (i64)UINT32_MAX)
                return fail(&c, v, DE_ERR_PARSE, "version %lld is out of range",
                            (long long)v->u.int64);
            version = (u32)v->u.int64;
            if (version > desc->version)
                return fail(&c, v, DE_ERR_VERSION_MISMATCH,
                            "stored version %u is newer than the type's version %u",
                            (unsigned)version, (unsigned)desc->version);
        }
    }
    if (version < desc->version) {
        if (desc->hooks.migrate != NULL) {
            c.mode = MODE_MIGRATE;
        } else {
            c.mode = MODE_LENIENT;
            warn(&c, NULL,
                 "stored version %u is older than %u and the type has no migrate hook; fields "
                 "are matched by name",
                 (unsigned)version, (unsigned)desc->version);
        }
    }
    DE_TRY(read_struct(&c, e->cur, tab, (u8 *)obj, true));
    if (c.mode == MODE_MIGRATE)
        return run_migrate(&c, desc, tab, obj, version, a);
    return DE_OK;
}

/* ---- Reading the input --------------------------------------------------------------------- */

/* Reads all of `rd` into one NUL-terminated block of `*cap` bytes from `a`. */
static de_result read_all(const de_allocator *a, de_reader *rd, char **out, size_t *len,
                          size_t *cap) {
    size_t c = 4096, n = 0;
    char *buf = (char *)DE_ALLOC(a, c, 1, DE_MEM_GENERAL);
    if (buf == NULL)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "text reader: no memory for the input");
    for (;;) {
        if (c - n < 2) {
            if (c > (size_t)INT_MAX / 2) {
                DE_FREE(a, buf, c);
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "text reader: the input is too large");
            }
            char *grown = (char *)DE_REALLOC(a, buf, c, c * 2, 1, DE_MEM_GENERAL);
            if (grown == NULL) {
                DE_FREE(a, buf, c);
                return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "text reader: no memory for the input");
            }
            buf = grown;
            c *= 2;
        }
        size_t got = 0;
        de_result res = rd->read(rd->user, buf + n, c - n - 1, &got);
        if (res != DE_OK) {
            DE_FREE(a, buf, c);
            return DE_FAIL_WRAP(res, "text reader: the reader failed");
        }
        if (got == 0)
            break;
        n += got;
    }
    buf[n] = '\0';
    *out = buf;
    *len = n;
    *cap = c;
    return DE_OK;
}

static const de_ser_options *default_options(const de_ser_options *opt, de_ser_options *storage) {
    if (opt != NULL)
        return opt;
    memset(storage, 0, sizeof *storage);
    storage->format = DE_SER_TEXT;
    return storage;
}

static de_result parse_error(const toml_result_t *res, const char *source) {
    return DE_FAIL(DE_ERR_PARSE, "%s: %s", source, res->errmsg);
}

de_result de_deserialize_object(const de_type_registry *r, de_type_id type, void *obj,
                                const de_ser_options *opt, de_reader *rd) {
    de_ser_options defaults;
    opt = default_options(opt, &defaults);
    if (r == NULL || rd == NULL || rd->read == NULL || obj == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_deserialize_object: NULL registry, object or reader");
    switch (opt->format) {
    case DE_SER_TEXT: {
        const de_allocator *a = de_heap_allocator();
        /* Zeroed: DE_TRY cannot show GCC that read_all's failure paths return != DE_OK. */
        char *text = NULL;
        size_t len = 0, cap = 0;
        DE_TRY(read_all(a, rd, &text, &len, &cap));
        toml_result_t res = de__toml_parse(a, text, (int)len);
        de_result out = res.ok ? de__ser_text_read_table(r, type, obj, opt, &res.toptab, a, "text")
                               : parse_error(&res, "text");
        de__toml_free(res);
        DE_FREE(a, text, cap);
        return out;
    }
    case DE_SER_BINARY:
        /* The binary serializer (DENG-296, ser_binary.c), with its default options. */
        return de_deserialize_binary(r, type, obj, opt, NULL, rd);
    }
    return DE_FAIL(DE_ERR_UNSUPPORTED, "de_deserialize_object: format %d is not available",
                   (int)opt->format);
}

/* ---- Text documents ------------------------------------------------------------------------ */

struct de_text_doc {
    const de_allocator *a;
    toml_result_t res;
    char *source;
    size_t source_size;
};

de_result de_text_doc_parse(const de_allocator *a, const char *text, size_t len, const char *source,
                            de_text_doc **out) {
    if (out == NULL || (text == NULL && len != 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_text_doc_parse: NULL text or output");
    *out = NULL;
    if (len > (size_t)INT_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_text_doc_parse: %zu bytes is too large", len);
    if (a == NULL)
        a = de_heap_allocator();
    if (source == NULL)
        source = "text";
    size_t ssize = strlen(source) + 1;
    de_text_doc *doc = DE_NEW(a, de_text_doc, DE_MEM_GENERAL);
    if (doc == NULL)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "%s: no memory for the document", source);
    memset(doc, 0, sizeof *doc);
    doc->a = a;
    doc->source = (char *)DE_ALLOC(a, ssize, 1, DE_MEM_GENERAL);
    if (doc->source == NULL) {
        DE_FREE(a, doc, sizeof *doc);
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "%s: no memory for the document", source);
    }
    memcpy(doc->source, source, ssize);
    doc->source_size = ssize;
    doc->res = de__toml_parse(a, text, (int)len);
    if (!doc->res.ok) {
        de_result err = parse_error(&doc->res, source);
        de_text_doc_destroy(doc);
        return err;
    }
    *out = doc;
    return DE_OK;
}

void de_text_doc_destroy(de_text_doc *doc) {
    if (doc == NULL)
        return;
    const de_allocator *a = doc->a;
    de__toml_free(doc->res);
    DE_FREE(a, doc->source, doc->source_size);
    DE_FREE(a, doc, sizeof *doc);
}

/* The `[[key]]` array, or NULL. */
static const toml_datum_t *doc_array(const de_text_doc *doc, const char *key) {
    const toml_datum_t *top = &doc->res.toptab;
    for (int32_t i = 0; i < top->u.tab.size; ++i) {
        if (strcmp(top->u.tab.key[i], key) != 0)
            continue;
        const toml_datum_t *v = &top->u.tab.value[i];
        if (v->type != TOML_ARRAY)
            return NULL;
        for (int32_t k = 0; k < v->u.arr.size; ++k)
            if (v->u.arr.elem[k].type != TOML_TABLE)
                return NULL;
        return v;
    }
    return NULL;
}

/* Entry `index` of `[[key]]` (NULL key: the top-level table, index 0), or NULL. */
static const toml_datum_t *doc_entry(const de_text_doc *doc, const char *key, u32 index) {
    if (doc == NULL)
        return NULL;
    if (key == NULL)
        return index == 0 ? &doc->res.toptab : NULL;
    const toml_datum_t *arr = doc_array(doc, key);
    if (arr == NULL || index >= (u32)arr->u.arr.size)
        return NULL;
    return &arr->u.arr.elem[index];
}

u32 de_text_doc_count(const de_text_doc *doc, const char *table_key) {
    if (doc == NULL)
        return 0;
    if (table_key == NULL)
        return 1;
    const toml_datum_t *arr = doc_array(doc, table_key);
    return arr ? (u32)arr->u.arr.size : 0;
}

const char *de_text_doc_type(const de_text_doc *doc, const char *table_key, u32 index) {
    const toml_datum_t *t = doc_entry(doc, table_key, index);
    if (t == NULL)
        return NULL;
    for (int32_t i = 0; i < t->u.tab.size; ++i)
        if (strcmp(t->u.tab.key[i], "__type") == 0)
            return t->u.tab.value[i].type == TOML_STRING ? t->u.tab.value[i].u.str.ptr : NULL;
    return NULL;
}

de_result de_text_doc_read_object(const de_text_doc *doc, const char *table_key, u32 index,
                                  const de_type_registry *r, de_type_id type, void *obj,
                                  const de_ser_options *opt) {
    if (doc == NULL || r == NULL || obj == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_text_doc_read_object: NULL document, registry or object");
    const toml_datum_t *t = doc_entry(doc, table_key, index);
    if (t == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: no entry %u of [[%s]]", doc->source,
                       (unsigned)index, table_key ? table_key : "(top level)");
    de_ser_options defaults;
    opt = default_options(opt, &defaults);
    return de__ser_text_read_table(r, type, obj, opt, t, doc->a, doc->source);
}
