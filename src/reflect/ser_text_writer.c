/*
 * ser_text_writer.c: the TOML text writer (DENG-295, SPEC §9.2) and de_serialize_object.
 *
 * Hand-written: tomlc17 only parses. The output is a function of the object bytes, the type
 * description and the options only: declaration order, '\n', one space around '=', ", " between
 * array items, locale-independent numbers (ser_common.c). That makes it diff-friendly and lets
 * tests compare it byte for byte with golden files. The format itself is described in
 * de_reflect.h ("Text format").
 *
 * Nothing is allocated: text is staged in a fixed buffer (de__ser_out) in front of the writer.
 */
#include "ser_internal.h"

#include <stdint.h>
#include <string.h>

typedef struct wctx {
    const de_type_registry *r;
    const de_ser_options *opt;
    const char *type_name; /* top-level type, for messages */
    de__ser_out *out;
    de__ser_path path;
    de_result err; /* first failure that is not the writer's */
} wctx;

/* ---- Small helpers ------------------------------------------------------------------------- */

static bool field_selected(const wctx *c, const de_field_info *f) {
    u32 req = c->opt->required_flags;
    return (f->flags & req) == req;
}

static de_result fail(wctx *c, de_result code, const char *what) {
    if (c->err == DE_OK)
        c->err = DE_FAIL(code, "text writer: type %s, field %s: %s", c->type_name,
                         de__ser_path_str(&c->path), what);
    return c->err;
}

static bool ok(const wctx *c) {
    return c->err == DE_OK && c->out->err == DE_OK;
}

static void put(wctx *c, const char *s) {
    de__ser_out_puts(c->out, s);
}

static u64 load_uint(const u8 *p, u32 size) {
    switch (size) {
    case 1:
        return p[0];
    case 2: {
        u16 v;
        memcpy(&v, p, 2);
        return v;
    }
    case 4: {
        u32 v;
        memcpy(&v, p, 4);
        return v;
    }
    default: {
        u64 v;
        memcpy(&v, p, 8);
        return v;
    }
    }
}

static u64 size_mask(u32 size) {
    return size >= 8 ? ~(u64)0 : (((u64)1 << (size * 8u)) - 1u);
}

static i64 sign_extend(u64 raw, u32 size) {
    if (size >= 8)
        return (i64)raw;
    u64 sign = (u64)1 << (size * 8u - 1u);
    return (i64)((raw ^ sign) - sign);
}

static void put_hex(wctx *c, u64 v) {
    char buf[DE__SER_NUM_CAP];
    buf[0] = '"';
    size_t n = de__ser_format_hex64(v, buf + 1);
    buf[n + 1] = '"';
    buf[n + 2] = '\0';
    put(c, buf);
}

static void put_f32(wctx *c, f32 v) {
    char buf[DE__SER_NUM_CAP];
    de__ser_format_f32(v, buf);
    put(c, buf);
}

static void put_f64(wctx *c, f64 v) {
    char buf[DE__SER_NUM_CAP];
    de__ser_format_f64(v, buf);
    put(c, buf);
}

/* Length of the valid UTF-8 sequence at `s` (at most `n` bytes), or 0. Rejects overlong forms,
 * surrogates and code points above U+10FFFF, like tomlc17's check. */
static size_t utf8_seq(const unsigned char *s, size_t n) {
    unsigned char b = s[0];
    if (b < 0x80)
        return 1;
    size_t len;
    u32 cp;
    if (b >= 0xC2 && b <= 0xDF) {
        len = 2;
        cp = b & 0x1Fu;
    } else if (b >= 0xE0 && b <= 0xEF) {
        len = 3;
        cp = b & 0x0Fu;
    } else if (b >= 0xF0 && b <= 0xF4) {
        len = 4;
        cp = b & 0x07u;
    } else {
        return 0;
    }
    if (len > n)
        return 0;
    for (size_t i = 1; i < len; ++i) {
        if ((s[i] & 0xC0u) != 0x80u)
            return 0;
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    if ((len == 3 && cp < 0x800u) || (len == 4 && cp < 0x10000u) || cp > 0x10FFFFu ||
        (cp >= 0xD800u && cp <= 0xDFFFu))
        return 0;
    return len;
}

/* Writes `n` bytes as a TOML basic string. Fails on invalid UTF-8 (TOML files are UTF-8). */
static void put_string(wctx *c, const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    de__ser_out_put(c->out, "\"", 1);
    size_t run = 0; /* start of the pending run of bytes that need no escape */
    size_t i = 0;
    while (i < n) {
        unsigned char ch = p[i];
        const char *esc = NULL;
        char ubuf[8];
        switch (ch) {
        case '"':
            esc = "\\\"";
            break;
        case '\\':
            esc = "\\\\";
            break;
        case '\b':
            esc = "\\b";
            break;
        case '\t':
            esc = "\\t";
            break;
        case '\n':
            esc = "\\n";
            break;
        case '\f':
            esc = "\\f";
            break;
        case '\r':
            esc = "\\r";
            break;
        default:
            if (ch < 0x20 || ch == 0x7F) {
                static const char k_hex[] = "0123456789ABCDEF";
                memcpy(ubuf, "\\u00", 4);
                ubuf[4] = k_hex[ch >> 4];
                ubuf[5] = k_hex[ch & 0xF];
                ubuf[6] = '\0';
                esc = ubuf;
            }
            break;
        }
        if (esc != NULL) {
            de__ser_out_put(c->out, s + run, i - run);
            put(c, esc);
            ++i;
            run = i;
            continue;
        }
        size_t len = utf8_seq(p + i, n - i);
        if (len == 0) {
            fail(c, DE_ERR_INVALID_ARGUMENT, "the string is not valid UTF-8");
            return;
        }
        i += len;
    }
    de__ser_out_put(c->out, s + run, n - run);
    de__ser_out_put(c->out, "\"", 1);
}

/* ---- Values -------------------------------------------------------------------------------- */

static void write_value(wctx *c, const de_field_info *f, const de__field_extra *ex, const u8 *p,
                        bool top);

/* `{ a = 1, b = 2 }`: the selected fields of struct `layout` at `p`. */
static void write_inline_struct(wctx *c, const de__type_layout *layout, const u8 *p) {
    bool any = false;
    put(c, "{");
    for (u32 i = 0; i < layout->desc.field_count && ok(c); ++i) {
        const de_field_info *f = &layout->fields[i];
        if (!field_selected(c, f))
            continue;
        size_t at = de__ser_path_push_name(&c->path, f->name);
        put(c, any ? ", " : " ");
        put(c, f->name);
        put(c, " = ");
        write_value(c, f, &layout->extra[i], p + f->offset, false);
        de__ser_path_pop(&c->path, at);
        any = true;
    }
    put(c, any ? " }" : "}");
}

static void write_enum(wctx *c, const de_field_info *f, const u8 *p) {
    const de__type_entry *e = de__reflect_find_entry(c->r, f->attrs.nested_type);
    u64 mask = size_mask(f->size);
    u64 raw = load_uint(p, f->size);
    const de_enum_info *info = (e && e->kind == DE__ENTRY_ENUM) ? e->enum_info : NULL;
    if (f->kind == DE_FK_ENUM) {
        for (u32 i = 0; info && i < info->count; ++i) {
            if (((u64)info->values[i].value & mask) == raw) {
                put_string(c, info->values[i].name, strlen(info->values[i].name));
                return;
            }
        }
        de__ser_out_putf(c->out, "%lld", (long long)sign_extend(raw, f->size));
        return;
    }
    /* FLAGS: names of the values whose bits are all set, in registration order, then the bits
     * left over as an integer. */
    put(c, "[");
    bool any = false;
    u64 rest = raw;
    for (u32 i = 0; info && i < info->count && rest != 0; ++i) {
        u64 v = (u64)info->values[i].value & mask;
        if (v != 0 && (rest & v) == v) {
            put(c, any ? ", " : "");
            put_string(c, info->values[i].name, strlen(info->values[i].name));
            rest &= ~v;
            any = true;
        }
    }
    if (rest != 0) {
        put(c, any ? ", " : "");
        if (rest <= (u64)INT64_MAX)
            de__ser_out_putf(c->out, "%llu", (unsigned long long)rest);
        else
            put_hex(c, rest);
    }
    put(c, "]");
}

static void write_array(wctx *c, const de_field_info *f, const de__field_extra *ex, const u8 *p,
                        bool top) {
    const de_field_info *el = &ex->element;
    u32 n = f->attrs.array_count;
    /* Arrays of structs get one element per line where TOML allows it (outside inline tables),
     * so a change to one element is a one-line diff. */
    bool multiline = top && el->kind == DE_FK_STRUCT;
    put(c, multiline ? "[\n" : "[");
    for (u32 i = 0; i < n && ok(c); ++i) {
        size_t at = de__ser_path_push_index(&c->path, i);
        if (multiline)
            put(c, "    ");
        else if (i != 0)
            put(c, ", ");
        write_value(c, el, NULL, p + (size_t)i * el->size, false);
        if (multiline)
            put(c, ",\n");
        de__ser_path_pop(&c->path, at);
    }
    put(c, "]");
}

/*
 * Writes the value of field `f` at `p`. `ex` is the field's derived data (needed for
 * FIXED_ARRAY; NULL for array elements, which are never arrays). `top` is true for a value on
 * its own `key = value` line (multi-line arrays allowed).
 */
static void write_value(wctx *c, const de_field_info *f, const de__field_extra *ex, const u8 *p,
                        bool top) {
    if (!ok(c))
        return;
    const de_ser_context *ctx = c->opt->ctx;
    switch (f->kind) {
    case DE_FK_BOOL:
        put(c, p[0] ? "true" : "false");
        return;
    case DE_FK_I8:
    case DE_FK_I16:
    case DE_FK_I32:
    case DE_FK_I64:
        de__ser_out_putf(c->out, "%lld", (long long)sign_extend(load_uint(p, f->size), f->size));
        return;
    case DE_FK_U8:
    case DE_FK_U16:
    case DE_FK_U32:
    case DE_FK_U64: {
        u64 v = load_uint(p, f->size);
        if (v <= (u64)INT64_MAX)
            de__ser_out_putf(c->out, "%llu", (unsigned long long)v);
        else
            put_hex(c, v); /* TOML integers are signed 64-bit */
        return;
    }
    case DE_FK_F32: {
        f32 v;
        memcpy(&v, p, 4);
        put_f32(c, v);
        return;
    }
    case DE_FK_F64: {
        f64 v;
        memcpy(&v, p, 8);
        put_f64(c, v);
        return;
    }
    case DE_FK_VEC2:
    case DE_FK_VEC3:
    case DE_FK_VEC4:
    case DE_FK_QUAT:
    case DE_FK_COLOR:
    case DE_FK_MAT4: {
        put(c, "[");
        for (u32 i = 0; i < f->size / 4u; ++i) {
            f32 v;
            memcpy(&v, p + 4u * i, 4);
            if (i != 0)
                put(c, ", ");
            put_f32(c, v);
        }
        put(c, "]");
        return;
    }
    case DE_FK_DVEC3: {
        put(c, "[");
        for (u32 i = 0; i < 3; ++i) {
            f64 v;
            memcpy(&v, p + 8u * i, 8);
            if (i != 0)
                put(c, ", ");
            put_f64(c, v);
        }
        put(c, "]");
        return;
    }
    case DE_FK_NAME: {
        de_name n;
        memcpy(&n.hash, p, 8);
        const char *s = de_name_str(n);
        if (s != NULL) {
            size_t len = strlen(s);
            u64 ignored;
            /* A name that reads like a hash would come back as that hash: write the hash. */
            if (!de__ser_parse_hex64(s, len, &ignored)) {
                put_string(c, s, len);
                return;
            }
        }
        put_hex(c, n.hash);
        return;
    }
    case DE_FK_STRING: {
        const char *s = (const char *)p;
        const char *end = (const char *)memchr(s, '\0', f->size);
        if (end == NULL) {
            fail(c, DE_ERR_INVALID_ARGUMENT, "the string buffer is not NUL-terminated");
            return;
        }
        put_string(c, s, (size_t)(end - s));
        return;
    }
    case DE_FK_ENTITY: {
        u64 bits;
        memcpy(&bits, p, 8);
        u64 id = (ctx && ctx->entity_to_scene_id) ? ctx->entity_to_scene_id(bits, ctx->user) : bits;
        put_hex(c, id);
        return;
    }
    case DE_FK_ASSET: {
        u32 h;
        memcpy(&h, p, 4);
        u64 id = (ctx && ctx->asset_to_id) ? ctx->asset_to_id(h, f->attrs.nested_type, ctx->user)
                                           : (u64)h;
        de_name n;
        n.hash = id;
        const char *path = de_name_str(n);
        put(c, "{ id = ");
        put_hex(c, id);
        if (path != NULL) {
            put(c, ", path = ");
            put_string(c, path, strlen(path));
        }
        put(c, " }");
        return;
    }
    case DE_FK_ENUM:
    case DE_FK_FLAGS:
        write_enum(c, f, p);
        return;
    case DE_FK_STRUCT: {
        const de__type_layout *sub = de__reflect_find_layout(c->r, f->attrs.nested_type);
        if (sub == NULL) {
            fail(c, DE_ERR_INVALID_ARGUMENT, "the nested type is not registered");
            return;
        }
        write_inline_struct(c, sub, p);
        return;
    }
    case DE_FK_FIXED_ARRAY:
        DE_ASSERT(ex != NULL);
        write_array(c, f, ex, p, top);
        return;
    }
    fail(c, DE_ERR_INVALID_ARGUMENT, "unknown field kind");
}

/*
 * One `key = value` line per selected field of `layout`; nested structs become dotted keys
 * (`prefix` is "" or "transform.position."). `prefix_len` + names stay below the path cap.
 */
static void write_lines(wctx *c, const de__type_layout *layout, const u8 *p, char *prefix,
                        size_t prefix_len) {
    for (u32 i = 0; i < layout->desc.field_count && ok(c); ++i) {
        const de_field_info *f = &layout->fields[i];
        if (!field_selected(c, f))
            continue;
        size_t at = de__ser_path_push_name(&c->path, f->name);
        size_t nlen = strlen(f->name);
        if (f->kind == DE_FK_STRUCT) {
            const de__type_layout *sub = de__reflect_find_layout(c->r, f->attrs.nested_type);
            if (sub == NULL) {
                fail(c, DE_ERR_INVALID_ARGUMENT, "the nested type is not registered");
            } else if (prefix_len + nlen + 2 > DE__SER_PATH_CAP) {
                fail(c, DE_ERR_INVALID_ARGUMENT, "the dotted key is too long");
            } else {
                memcpy(prefix + prefix_len, f->name, nlen);
                prefix[prefix_len + nlen] = '.';
                prefix[prefix_len + nlen + 1] = '\0';
                write_lines(c, sub, p + f->offset, prefix, prefix_len + nlen + 1);
                prefix[prefix_len] = '\0';
            }
        } else {
            de__ser_out_put(c->out, prefix, prefix_len);
            put(c, f->name);
            put(c, " = ");
            write_value(c, f, &layout->extra[i], p + f->offset, true);
            put(c, "\n");
        }
        de__ser_path_pop(&c->path, at);
    }
}

de_result de__ser_text_write_body(de__ser_out *o, const de_type_registry *r, de_type_id type,
                                  const void *obj, const de_ser_options *opt) {
    const de__type_entry *e = de__reflect_entry(r, type);
    if (e == NULL || e->kind != DE__ENTRY_STRUCT)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "text writer: invalid type id 0x%x",
                       (unsigned)type.id);
    if (obj == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "text writer: object is NULL");
    wctx c;
    memset(&c, 0, sizeof c);
    c.r = r;
    c.opt = opt;
    c.type_name = e->cur->desc.name;
    c.out = o;
    put(&c, "__type = ");
    put_string(&c, c.type_name, strlen(c.type_name));
    de__ser_out_putf(o, "\n__version = %u\n", (unsigned)e->cur->desc.version);
    char prefix[DE__SER_PATH_CAP];
    prefix[0] = '\0';
    write_lines(&c, e->cur, (const u8 *)obj, prefix, 0);
    if (c.err != DE_OK)
        return c.err;
    return o->err;
}

/* ---- Public API ---------------------------------------------------------------------------- */

static const de_ser_options *default_options(const de_ser_options *opt, de_ser_options *storage) {
    if (opt != NULL)
        return opt;
    memset(storage, 0, sizeof *storage);
    storage->format = DE_SER_TEXT;
    return storage;
}

de_result de_serialize_object(const de_type_registry *r, de_type_id type, const void *obj,
                              const de_ser_options *opt, de_writer *w) {
    de_ser_options defaults;
    opt = default_options(opt, &defaults);
    if (r == NULL || w == NULL || w->write == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_serialize_object: NULL registry or writer");
    switch (opt->format) {
    case DE_SER_TEXT: {
        de__ser_out o;
        de__ser_out_init(&o, w);
        de_result res = de__ser_text_write_body(&o, r, type, obj, opt);
        if (res != DE_OK)
            return res;
        return de__ser_out_flush(&o);
    }
    case DE_SER_BINARY:
        /* The binary serializer (DENG-296, ser_binary.c), with its default options. */
        return de_serialize_binary(r, type, obj, opt, NULL, w);
    }
    return DE_FAIL(DE_ERR_UNSUPPORTED, "de_serialize_object: format %d is not available",
                   (int)opt->format);
}

static bool bare_key(const char *k) {
    if (k == NULL || k[0] == '\0')
        return false;
    for (const char *p = k; *p != '\0'; ++p) {
        char ch = *p;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-'))
            return false;
    }
    return true;
}

de_result de_text_doc_begin(de_text_doc_writer *doc, de_writer *w) {
    if (doc == NULL || w == NULL || w->write == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_text_doc_begin: NULL document or writer");
    doc->w = w;
    doc->object_count = 0;
    return DE_OK;
}

de_result de_text_doc_write_object(de_text_doc_writer *doc, const char *table_key,
                                   const de_type_registry *r, de_type_id type, const void *obj,
                                   const de_ser_options *opt) {
    if (doc == NULL || doc->w == NULL || r == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_text_doc_write_object: NULL document or registry");
    if (!bare_key(table_key))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_text_doc_write_object: table key '%s' is not a TOML bare key",
                       table_key ? table_key : "(null)");
    de_ser_options defaults;
    opt = default_options(opt, &defaults);
    de__ser_out o;
    de__ser_out_init(&o, doc->w);
    if (doc->object_count != 0)
        de__ser_out_put(&o, "\n", 1);
    de__ser_out_puts(&o, "[[");
    de__ser_out_puts(&o, table_key);
    de__ser_out_puts(&o, "]]\n");
    de_result res = de__ser_text_write_body(&o, r, type, obj, opt);
    if (res != DE_OK)
        return res;
    res = de__ser_out_flush(&o);
    if (res == DE_OK)
        doc->object_count++;
    return res;
}

de_result de_text_doc_end(de_text_doc_writer *doc) {
    if (doc == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_text_doc_end: NULL document");
    doc->w = NULL;
    return DE_OK;
}
