/*
 * editor_prefab_source.c: in-place edits of a prefab's source TOML (DENG-798, DIDE-R37).
 *
 * prefab.applyOverride writes the changed fields back to `*.prefab.toml`. The file is edited as
 * text, not re-serialized: only the value span of each changed `key = value` line is replaced,
 * so comments, blank lines, key order, quoting of other values and line endings stay as they
 * are. A field that the component table does not set yet is inserted after the table's last key.
 * A whole-component edit (DENG-810: path "Component") deletes the component's table, from its
 * header line to its last key line, or writes a new table at the end of the entity's block.
 *
 * Two entity layouts are understood (Trama "host: Scene text serialization"):
 *   array:  [[entity]] / id = "0x1" / [entity.Comp]
 *   keyed:  [entity.<id>.Comp]
 * Anything else that would hold the field (dotted keys, an inline `Comp = { ... }`, a sub-table
 * `[entity.Comp.field]`, a missing entity or component) is refused with "source_layout": the
 * editor never guesses where a value lives.
 *
 * Values are formatted from the editor's MessagePack encoding (editor_value.c) and the field's
 * reflection info, with the text writer's number formats (shortest round trip, ser_common.c).
 * The output depends only on the input bytes and the edits, so it is deterministic.
 */
#include "host/editor/editor_internal.h"

#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>

#include "core/de_toml_internal.h"
#include "reflect/ser_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------- Growable text ---------- */

typedef struct sbuf {
    char *p;
    size_t len, cap;
    bool oom;
} sbuf;

static void sb_put(sbuf *b, const char *s, size_t n) {
    if (b->oom)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *p = (char *)DE_REALLOC(de_heap_allocator(), b->p, b->cap, cap, 1, DE_MEM_GENERAL);
        if (p == NULL) {
            b->oom = true;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_puts(sbuf *b, const char *s) {
    sb_put(b, s, strlen(s));
}

static void sb_putf(sbuf *b, const char *fmt, ...) {
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < sizeof buf)
        sb_put(b, buf, (size_t)n);
    else
        b->oom = true;
}

static void sb_free(sbuf *b) {
    if (b->p != NULL)
        DE_FREE(de_heap_allocator(), b->p, b->cap);
    memset(b, 0, sizeof *b);
}

/* ---------- Values ---------- */

static void put_string(sbuf *b, const char *s, size_t n) {
    static const char k_hex[] = "0123456789ABCDEF";
    sb_put(b, "\"", 1);
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ch = (unsigned char)s[i];
        switch (ch) {
        case '"': sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\b': sb_puts(b, "\\b"); break;
        case '\t': sb_puts(b, "\\t"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\f': sb_puts(b, "\\f"); break;
        case '\r': sb_puts(b, "\\r"); break;
        default:
            if (ch < 0x20 || ch == 0x7F) {
                const char u[6] = {'\\', 'u', '0', '0', k_hex[ch >> 4], k_hex[ch & 0xF]};
                sb_put(b, u, 6);
            } else {
                sb_put(b, (const char *)&s[i], 1); /* UTF-8 is checked by the final parse */
            }
        }
    }
    sb_put(b, "\"", 1);
}

static void put_hex(sbuf *b, u64 v) {
    char buf[DE__SER_NUM_CAP];
    de__ser_format_hex64(v, buf);
    sb_put(b, "\"", 1);
    sb_puts(b, buf);
    sb_put(b, "\"", 1);
}

static bool node_f64(mpack_node_t n, f64 *out) {
    switch (mpack_node_type(n)) {
    case mpack_type_float: *out = mpack_node_float(n); return true;
    case mpack_type_double: *out = mpack_node_double(n); return true;
    case mpack_type_int: *out = (f64)mpack_node_i64(n); return true;
    case mpack_type_uint: *out = (f64)mpack_node_u64(n); return true;
    default: return false;
    }
}

static bool put_f32(sbuf *b, mpack_node_t n) {
    f64 d;
    f32 v;
    if (!node_f64(n, &d) || !de__ser_f64_to_f32(d, &v))
        return false;
    char buf[DE__SER_NUM_CAP];
    de__ser_format_f32(v, buf);
    sb_puts(b, buf);
    return true;
}

static bool put_f64(sbuf *b, mpack_node_t n) {
    f64 d;
    if (!node_f64(n, &d))
        return false;
    char buf[DE__SER_NUM_CAP];
    de__ser_format_f64(d, buf);
    sb_puts(b, buf);
    return true;
}

static bool put_int(sbuf *b, mpack_node_t n) {
    if (mpack_node_type(n) == mpack_type_int) {
        sb_putf(b, "%lld", (long long)mpack_node_i64(n));
        return true;
    }
    if (mpack_node_type(n) != mpack_type_uint)
        return false;
    const u64 v = mpack_node_u64(n);
    if (v <= (u64)INT64_MAX)
        sb_putf(b, "%llu", (unsigned long long)v);
    else
        put_hex(b, v); /* TOML integers are signed 64-bit, like the text writer */
    return true;
}

static bool put_str_node(sbuf *b, mpack_node_t n) {
    if (mpack_node_type(n) != mpack_type_str)
        return false;
    put_string(b, mpack_node_str(n), mpack_node_strlen(n));
    return true;
}

static bool put_value(sbuf *b, const de_type_registry *r, de_type_id owner, u32 index,
                      const de_field_info *f, mpack_node_t n);

static bool put_struct(sbuf *b, const de_type_registry *r, const char *type, mpack_node_t n) {
    const de_type_id t = type != NULL ? de_type_find(r, type) : (de_type_id){0};
    u32 count = 0;
    const de_field_info *fields = t.id ? de_type_fields(r, t, &count) : NULL;
    if (fields == NULL || mpack_node_type(n) != mpack_type_map)
        return false;
    sb_puts(b, "{ ");
    bool any = false;
    for (u32 i = 0; i < count; ++i) {
        if (!mpack_node_map_contains_cstr(n, fields[i].name))
            continue;
        sb_puts(b, any ? ", " : "");
        sb_puts(b, fields[i].name);
        sb_puts(b, " = ");
        if (!put_value(b, r, t, i, &fields[i], mpack_node_map_cstr(n, fields[i].name)))
            return false;
        any = true;
    }
    sb_puts(b, any ? " }" : "}");
    return true;
}

/* The TOML text of field `f` (field `index` of `owner`) for its editor value `n`. */
static bool put_value(sbuf *b, const de_type_registry *r, de_type_id owner, u32 index,
                      const de_field_info *f, mpack_node_t n) {
    const mpack_type_t t = mpack_node_type(n);
    switch (f->kind) {
    case DE_FK_BOOL:
        if (t != mpack_type_bool)
            return false;
        sb_puts(b, mpack_node_bool(n) ? "true" : "false");
        return true;
    case DE_FK_I8:
    case DE_FK_I16:
    case DE_FK_I32:
    case DE_FK_I64:
    case DE_FK_U8:
    case DE_FK_U16:
    case DE_FK_U32:
    case DE_FK_U64: return put_int(b, n);
    case DE_FK_F32: return put_f32(b, n);
    case DE_FK_F64: return put_f64(b, n);
    case DE_FK_VEC2:
    case DE_FK_VEC3:
    case DE_FK_VEC4:
    case DE_FK_QUAT:
    case DE_FK_COLOR:
    case DE_FK_MAT4:
    case DE_FK_DVEC3: {
        const u32 count = f->kind == DE_FK_DVEC3 ? 3u : f->size / 4u;
        if (t != mpack_type_array || mpack_node_array_length(n) != count)
            return false;
        sb_puts(b, "[");
        for (u32 i = 0; i < count; ++i) {
            sb_puts(b, i ? ", " : "");
            const mpack_node_t e = mpack_node_array_at(n, i);
            if (!(f->kind == DE_FK_DVEC3 ? put_f64(b, e) : put_f32(b, e)))
                return false;
        }
        sb_puts(b, "]");
        return true;
    }
    case DE_FK_NAME:
        if (t == mpack_type_uint) {
            put_hex(b, mpack_node_u64(n));
            return true;
        }
        return put_str_node(b, n);
    case DE_FK_STRING: return put_str_node(b, n);
    case DE_FK_ENTITY:
        /* A prefab-local scene id; prefab sources write ids in short hex ("0x1"). */
        if (t != mpack_type_uint && t != mpack_type_int)
            return false;
        sb_putf(b, "\"0x%llx\"", (unsigned long long)mpack_node_u64(n));
        return true;
    case DE_FK_ASSET:
        return put_str_node(b, n); /* a path; a bare id has no source spelling */
    case DE_FK_ENUM:
        if (t == mpack_type_str)
            return put_str_node(b, n);
        return put_int(b, n);
    case DE_FK_FLAGS:
        sb_puts(b, "[");
        if (t == mpack_type_array) {
            for (u32 i = 0; i < mpack_node_array_length(n); ++i) {
                sb_puts(b, i ? ", " : "");
                if (!put_str_node(b, mpack_node_array_at(n, i)))
                    return false;
            }
        } else if (!put_int(b, n)) {
            return false;
        }
        sb_puts(b, "]");
        return true;
    case DE_FK_STRUCT: return put_struct(b, r, f->attrs.nested_type, n);
    case DE_FK_FIXED_ARRAY: {
        const de_field_info *el = owner.id ? de_type_array_element(r, owner, index) : NULL;
        if (el == NULL || t != mpack_type_array)
            return false;
        sb_puts(b, "[");
        for (u32 i = 0; i < mpack_node_array_length(n); ++i) {
            sb_puts(b, i ? ", " : "");
            if (!put_value(b, r, (de_type_id){0}, 0, el, mpack_node_array_at(n, i)))
                return false;
        }
        sb_puts(b, "]");
        return true;
    }
    }
    return false;
}

/* ---------- Scanning ---------- */

#define SEG_MAX 4u
#define SEG_CAP 128u

enum { IT_TABLE, IT_ARRAY, IT_KEY };

typedef struct item {
    u8 kind;
    u8 nseg; /* SEG_MAX + 1: deeper than we look */
    char seg[SEG_MAX][SEG_CAP];
    size_t vs, ve; /* IT_KEY: the value span */
    size_t line_start; /* where the item's line begins */
    size_t line_end; /* just past the line's newline (or the end of the text) */
} item;

typedef struct scan {
    const char *s;
    size_t n, p;
    item *items;
    u32 count, cap;
    bool oom;
} scan;

static bool at_eol(const scan *c) {
    return c->p >= c->n || c->s[c->p] == '\n' || (c->s[c->p] == '\r' && c->p + 1 < c->n &&
                                                  c->s[c->p + 1] == '\n');
}

static void skip_ws(scan *c) {
    while (c->p < c->n && (c->s[c->p] == ' ' || c->s[c->p] == '\t'))
        ++c->p;
}

/* Whitespace, an optional comment, then the end of the line. */
static bool finish_line(scan *c, size_t *line_end) {
    skip_ws(c);
    if (c->p < c->n && c->s[c->p] == '#')
        while (c->p < c->n && c->s[c->p] != '\n')
            ++c->p;
    if (c->p < c->n && c->s[c->p] == '\r')
        ++c->p;
    if (c->p < c->n && c->s[c->p] != '\n')
        return false;
    if (c->p < c->n)
        ++c->p;
    *line_end = c->p;
    return true;
}

static bool bare_char(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == '_' || ch == '-';
}

/* A dotted key into `it->seg`. Escapes in quoted keys other than \" and \\ are refused. */
static bool scan_key(scan *c, item *it) {
    it->nseg = 0;
    for (;;) {
        skip_ws(c);
        char tmp[SEG_CAP];
        size_t len = 0;
        if (c->p < c->n && (c->s[c->p] == '"' || c->s[c->p] == '\'')) {
            const char q = c->s[c->p++];
            while (c->p < c->n && c->s[c->p] != q) {
                char ch = c->s[c->p++];
                if (ch == '\n')
                    return false;
                if (q == '"' && ch == '\\') {
                    if (c->p >= c->n || (c->s[c->p] != '"' && c->s[c->p] != '\\'))
                        return false;
                    ch = c->s[c->p++];
                }
                if (len + 1 >= SEG_CAP)
                    return false;
                tmp[len++] = ch;
            }
            if (c->p >= c->n)
                return false;
            ++c->p;
        } else {
            while (c->p < c->n && bare_char(c->s[c->p])) {
                if (len + 1 >= SEG_CAP)
                    return false;
                tmp[len++] = c->s[c->p++];
            }
            if (len == 0)
                return false;
        }
        tmp[len] = '\0';
        if (it->nseg < SEG_MAX)
            memcpy(it->seg[it->nseg], tmp, len + 1);
        if (it->nseg <= SEG_MAX)
            ++it->nseg;
        skip_ws(c);
        if (c->p < c->n && c->s[c->p] == '.') {
            ++c->p;
            continue;
        }
        return true;
    }
}

static bool scan_string(scan *c) {
    const char q = c->s[c->p];
    const bool multi = c->p + 2 < c->n && c->s[c->p + 1] == q && c->s[c->p + 2] == q;
    c->p += multi ? 3 : 1;
    while (c->p < c->n) {
        const char ch = c->s[c->p];
        if (q == '"' && ch == '\\') {
            c->p += 2;
            continue;
        }
        if (!multi && ch == '\n')
            return false;
        if (ch == q) {
            if (!multi) {
                ++c->p;
                return true;
            }
            if (c->p + 2 < c->n && c->s[c->p + 1] == q && c->s[c->p + 2] == q) {
                c->p += 3;
                /* Up to two more quotes belong to the content. */
                for (int k = 0; k < 2 && c->p < c->n && c->s[c->p] == q; ++k)
                    ++c->p;
                return true;
            }
        }
        ++c->p;
    }
    return false;
}

/* One value; `c->p` ends just past it. */
static bool scan_value(scan *c) {
    if (c->p >= c->n)
        return false;
    const char ch = c->s[c->p];
    if (ch == '"' || ch == '\'')
        return scan_string(c);
    if (ch == '[' || ch == '{') {
        u32 depth = 0;
        while (c->p < c->n) {
            const char x = c->s[c->p];
            if (x == '"' || x == '\'') {
                if (!scan_string(c))
                    return false;
                continue;
            }
            if (x == '#') {
                while (c->p < c->n && c->s[c->p] != '\n')
                    ++c->p;
                continue;
            }
            ++c->p;
            if (x == '[' || x == '{')
                ++depth;
            else if ((x == ']' || x == '}') && --depth == 0)
                return true;
        }
        return false;
    }
    const size_t start = c->p;
    while (c->p < c->n && c->s[c->p] != ' ' && c->s[c->p] != '\t' && c->s[c->p] != '#' &&
           c->s[c->p] != '\n' && c->s[c->p] != '\r' && c->s[c->p] != ',')
        ++c->p;
    return c->p > start;
}

static item *push(scan *c) {
    if (c->count == c->cap) {
        const u32 cap = c->cap ? c->cap * 2 : 32;
        item *p = (item *)DE_REALLOC(de_heap_allocator(), c->items, sizeof(item) * c->cap,
                                     sizeof(item) * cap, 8, DE_MEM_GENERAL);
        if (p == NULL) {
            c->oom = true;
            return NULL;
        }
        c->items = p;
        c->cap = cap;
    }
    item *it = &c->items[c->count++];
    memset(it, 0, sizeof *it);
    return it;
}

static void scan_free(scan *c) {
    if (c->items != NULL)
        DE_FREE(de_heap_allocator(), c->items, sizeof(item) * c->cap);
    c->items = NULL;
    c->count = c->cap = 0;
}

/* Splits the text into headers and key lines. False when it is not TOML we can follow. */
static bool scan_all(scan *c, const char *s, size_t n) {
    scan_free(c);
    c->s = s;
    c->n = n;
    c->p = 0;
    c->oom = false;
    while (c->p < c->n) {
        const size_t line_start = c->p;
        skip_ws(c);
        size_t end;
        if (at_eol(c) || c->s[c->p] == '#') {
            if (!finish_line(c, &end))
                return false;
            continue;
        }
        item *it = push(c);
        if (it == NULL)
            return false;
        it->line_start = line_start;
        if (c->s[c->p] == '[') {
            const bool arr = c->p + 1 < c->n && c->s[c->p + 1] == '[';
            c->p += arr ? 2 : 1;
            it->kind = arr ? IT_ARRAY : IT_TABLE;
            if (!scan_key(c, it))
                return false;
            if (c->p >= c->n || c->s[c->p] != ']')
                return false;
            ++c->p;
            if (arr) {
                if (c->p >= c->n || c->s[c->p] != ']')
                    return false;
                ++c->p;
            }
        } else {
            it->kind = IT_KEY;
            if (!scan_key(c, it))
                return false;
            if (c->p >= c->n || c->s[c->p] != '=')
                return false;
            ++c->p;
            skip_ws(c);
            it->vs = c->p;
            if (!scan_value(c))
                return false;
            it->ve = c->p;
        }
        if (!finish_line(c, &end))
            return false;
        it->line_end = end;
    }
    return true;
}

/* ---------- Locating a field ---------- */

static bool seg_is(const item *it, u32 i, const char *s) {
    return it->nseg > i && i < SEG_MAX && strcmp(it->seg[i], s) == 0;
}

/* An id as the sources spell it: "0x1" (or any hex), or a decimal integer. */
static bool parse_id(const char *s, size_t len, u64 *out) {
    if (len >= 2 && (s[0] == '"' || s[0] == '\'') && s[len - 1] == s[0]) {
        ++s;
        len -= 2;
    }
    if (de__ser_parse_hex64(s, len, out))
        return true;
    if (len == 0 || len > 19)
        return false;
    u64 v = 0;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10u + (u64)(s[i] - '0');
    }
    *out = v;
    return true;
}

enum { LOC_KEY, LOC_TABLE, LOC_NO_TABLE, LOC_LAYOUT };

typedef struct loc {
    int what;
    u32 key;      /* LOC_KEY: the item */
    size_t after; /* LOC_TABLE: where a new key line goes */
} loc;

static loc locate(const scan *c, u64 sid, const char *comp, const char *field) {
    bool array = false;
    for (u32 i = 0; i < c->count; ++i)
        array |= c->items[i].kind == IT_ARRAY && c->items[i].nseg == 1 &&
                 seg_is(&c->items[i], 0, "entity");
    loc out = {LOC_NO_TABLE, 0, 0};
    bool in_entity = false; /* the current [[entity]] block (array layout) is `sid` */
    bool in_block = false;  /* keys go to the block's top level */
    bool in_table = false;  /* keys go to the matching component table */
    bool found = false;
    for (u32 i = 0; i < c->count; ++i) {
        const item *it = &c->items[i];
        if (it->kind == IT_ARRAY) {
            const bool ent = it->nseg == 1 && seg_is(it, 0, "entity");
            in_entity = false;
            in_block = ent;
            in_table = false;
            continue;
        }
        if (it->kind == IT_TABLE) {
            in_block = false;
            in_table = false;
            u64 id = 0;
            if (array) {
                if (!in_entity || !seg_is(it, 0, "entity"))
                    continue;
                if (it->nseg == 2 && seg_is(it, 1, comp)) {
                    if (found)
                        return (loc){LOC_LAYOUT, 0, 0};
                    in_table = found = true;
                    out = (loc){LOC_TABLE, 0, it->line_end};
                } else if (it->nseg >= 3 && seg_is(it, 1, comp) && seg_is(it, 2, field)) {
                    return (loc){LOC_LAYOUT, 0, 0};
                }
            } else if (seg_is(it, 0, "entity") && it->nseg >= 2 && it->nseg <= SEG_MAX &&
                       parse_id(it->seg[1], strlen(it->seg[1]), &id) && id == sid) {
                if (it->nseg == 3 && seg_is(it, 2, comp)) {
                    if (found)
                        return (loc){LOC_LAYOUT, 0, 0};
                    in_table = found = true;
                    out = (loc){LOC_TABLE, 0, it->line_end};
                } else if (it->nseg >= 4 && seg_is(it, 2, comp) && seg_is(it, 3, field)) {
                    return (loc){LOC_LAYOUT, 0, 0};
                } else if (it->nseg == 2) {
                    in_block = true;
                    in_entity = true;
                }
            }
            continue;
        }
        /* A key line. */
        if (in_block && array && it->nseg == 1 && seg_is(it, 0, "id")) {
            u64 id = 0;
            in_entity = parse_id(c->s + it->vs, it->ve - it->vs, &id) && id == sid;
            continue;
        }
        if (in_block && in_entity && seg_is(it, 0, comp))
            return (loc){LOC_LAYOUT, 0, 0}; /* Comp.field = ... or Comp = { ... } */
        if (!in_table)
            continue;
        if (seg_is(it, 0, field)) {
            if (it->nseg != 1)
                return (loc){LOC_LAYOUT, 0, 0};
            out = (loc){LOC_KEY, i, 0};
            in_table = false; /* keep scanning for conflicts only */
            continue;
        }
        if (out.what == LOC_TABLE)
            out.after = it->line_end;
    }
    return out;
}



static const char *failf(char *msg, size_t cap, const char *reason, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, cap, fmt, ap);
    va_end(ap);
    return reason;
}

/* ---------- Locating a whole component (DENG-810) ---------- */

enum { CL_TABLE, CL_NONE, CL_LAYOUT };

typedef struct cloc {
    int what;
    size_t start, end;  /* CL_TABLE: the table, from its header line to its last key line */
    size_t insert;      /* CL_NONE: where a new table goes (the end of the entity's block) */
    bool keyed;         /* the keyed layout: a new header is [entity.<id>.Comp] */
    char id[SEG_CAP];   /* the keyed layout: the id as the source spells it */
} cloc;

static cloc locate_comp(const scan *c, u64 sid, const char *comp) {
    bool array = false;
    for (u32 i = 0; i < c->count; ++i)
        array |= c->items[i].kind == IT_ARRAY && c->items[i].nseg == 1 &&
                 seg_is(&c->items[i], 0, "entity");
    cloc out;
    memset(&out, 0, sizeof out);
    out.what = CL_LAYOUT;
    out.keyed = !array;
    bool seen = false;     /* the entity exists */
    bool found = false;    /* its component table exists */
    bool in_entity = false;
    bool in_block = false; /* keys go to the entity's top level */
    bool in_comp = false;  /* keys go to the component table */
    for (u32 i = 0; i < c->count; ++i) {
        const item *it = &c->items[i];
        if (it->kind == IT_ARRAY) {
            in_entity = false;
            in_block = it->nseg == 1 && seg_is(it, 0, "entity");
            in_comp = false;
            continue;
        }
        if (it->kind == IT_TABLE) {
            in_block = false;
            in_comp = false;
            u64 id = 0;
            if (array) {
                if (!seg_is(it, 0, "entity"))
                    in_entity = false;
                if (!in_entity)
                    continue;
                if (seg_is(it, 1, comp)) {
                    if (found || it->nseg != 2)
                        return (cloc){CL_LAYOUT, 0, 0, 0, false, {0}};
                    found = in_comp = true;
                    out.start = it->line_start;
                    out.end = it->line_end;
                }
            } else if (seg_is(it, 0, "entity") && it->nseg >= 2 && it->nseg <= SEG_MAX &&
                       parse_id(it->seg[1], strlen(it->seg[1]), &id) && id == sid) {
                if (!seen)
                    snprintf(out.id, sizeof out.id, "%s", it->seg[1]);
                seen = in_entity = true;
                if (seg_is(it, 2, comp)) {
                    if (found || it->nseg != 3)
                        return (cloc){CL_LAYOUT, 0, 0, 0, false, {0}};
                    found = in_comp = true;
                    out.start = it->line_start;
                    out.end = it->line_end;
                } else if (it->nseg == 2) {
                    in_block = true;
                }
            } else {
                in_entity = false;
                continue;
            }
            out.insert = it->line_end;
            continue;
        }
        if (array && in_block && it->nseg == 1 && seg_is(it, 0, "id")) {
            u64 id = 0;
            in_entity = parse_id(c->s + it->vs, it->ve - it->vs, &id) && id == sid;
            seen |= in_entity;
        }
        if (!in_entity)
            continue;
        if (in_block && seg_is(it, 0, comp))
            return (cloc){CL_LAYOUT, 0, 0, 0, false, {0}}; /* Comp.field = or Comp = { } */
        if (in_comp)
            out.end = it->line_end;
        out.insert = it->line_end;
    }
    if (!seen)
        return out;
    out.what = found ? CL_TABLE : CL_NONE;
    return out;
}

/* The table text of component `comp` for its editor map `n`, fields in type order. */
static bool put_table(sbuf *b, const de_type_registry *r, const cloc *l, const char *comp,
                      mpack_node_t n, const char *eol) {
    const de_type_id t = de_type_find(r, comp);
    u32 count = 0;
    const de_field_info *fields = t.id ? de_type_fields(r, t, &count) : NULL;
    if (t.id == 0 || mpack_node_type(n) != mpack_type_map)
        return false;
    if (l->keyed)
        sb_putf(b, "[entity.%s.%s]%s", l->id, comp, eol);
    else
        sb_putf(b, "[entity.%s]%s", comp, eol);
    for (u32 i = 0; i < count && fields != NULL; ++i) {
        if (!mpack_node_map_contains_cstr(n, fields[i].name))
            continue;
        sb_puts(b, fields[i].name);
        sb_puts(b, " = ");
        if (!put_value(b, r, t, i, &fields[i], mpack_node_map_cstr(n, fields[i].name)))
            return false;
        sb_puts(b, eol);
    }
    return true;
}

/* A whole-component edit: `ed->path` is "Component", `ed->val` its map or nil. NULL with `next`
 * set, or NULL with `next` empty when a follow edit has nothing to do, or a reason. */
static const char *comp_edit(const de_type_registry *types, const sbuf *cur, scan *c,
                             const de__ed_src_edit *ed, const char *eol, sbuf *next, bool *skip,
                             char *msg, size_t msg_cap) {
    *skip = false;
    if (strlen(ed->path) >= SEG_CAP)
        return failf(msg, msg_cap, "source_layout", "%s is not a component name", ed->path);
    if (!scan_all(c, cur->p, cur->len))
        return failf(msg, msg_cap, c->oom ? "memory" : "source_layout",
                     c->oom ? "out of memory" : "the source is not TOML the editor can edit");
    const cloc l = locate_comp(c, ed->sid, ed->path);
    if (l.what == CL_LAYOUT)
        return failf(msg, msg_cap, "source_layout", "the source does not hold entity 0x%llx with "
                     "%s as a table", (unsigned long long)ed->sid, ed->path);
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, (const char *)ed->val, ed->val_len);
    mpack_tree_parse(&tree);
    const char *why = NULL;
    if (mpack_tree_error(&tree) != mpack_ok) {
        why = failf(msg, msg_cap, "value", "%s has a malformed value", ed->path);
    } else if (mpack_node_type(mpack_tree_root(&tree)) == mpack_type_nil) {
        if (l.what == CL_TABLE) {
            sb_put(next, cur->p, l.start);
            sb_put(next, cur->p + l.end, cur->len - l.end);
        } else if (ed->follow) {
            *skip = true;
        } else {
            why = failf(msg, msg_cap, "source_layout", "the source has no [entity.%s] table for "
                        "0x%llx", ed->path, (unsigned long long)ed->sid);
        }
    } else if (l.what == CL_TABLE && ed->follow) {
        *skip = true; /* a level that sets the component itself keeps it */
    } else {
        sbuf table = {0};
        const size_t at = l.what == CL_TABLE ? l.start : l.insert;
        const bool ok = put_table(&table, types, &l, ed->path, mpack_tree_root(&tree), eol);
        if (!ok || table.oom) {
            why = failf(msg, msg_cap, "value", "%s has no source spelling for its value",
                        ed->path);
        } else {
            sb_put(next, cur->p, at);
            if (at > 0 && cur->p[at - 1] != '\n')
                sb_puts(next, eol);
            sb_put(next, table.p, table.len);
            const size_t rest = l.what == CL_TABLE ? l.end : at;
            sb_put(next, cur->p + rest, cur->len - rest);
        }
        sb_free(&table);
    }
    (void)mpack_tree_destroy(&tree);
    return why;
}

/* ---------- Entry point ---------- */

const char *de__ed_prefab_source_edit(const de_type_registry *types, const char *src, size_t len,
                                      const de__ed_src_edit *edits, u32 count, char **out,
                                      size_t *out_len, char *msg, size_t msg_cap) {
    *out = NULL;
    *out_len = 0;
    sbuf cur = {0};
    sb_put(&cur, src, len);
    if (cur.oom)
        return failf(msg, msg_cap, "memory", "out of memory");
    const bool crlf = strstr(cur.p, "\r\n") != NULL;
    const char *eol = crlf ? "\r\n" : "\n";
    scan c;
    memset(&c, 0, sizeof c);
    const char *why = NULL;
    for (u32 e = 0; e < count && why == NULL; ++e) {
        const de__ed_src_edit *ed = &edits[e];
        const char *dot = strchr(ed->path, '.');
        if (dot == NULL) {
            sbuf next = {0};
            bool skip = false;
            why = comp_edit(types, &cur, &c, ed, eol, &next, &skip, msg, msg_cap);
            if (why == NULL && !skip && next.oom)
                why = failf(msg, msg_cap, "memory", "out of memory");
            if (why != NULL || skip) {
                sb_free(&next);
                continue;
            }
            sb_free(&cur);
            cur = next;
            continue;
        }
        char comp[SEG_CAP];
        if ((size_t)(dot - ed->path) >= sizeof comp || strchr(dot + 1, '.')) {
            why = failf(msg, msg_cap, "source_layout", "%s is not Component.field", ed->path);
            break;
        }
        memcpy(comp, ed->path, (size_t)(dot - ed->path));
        comp[dot - ed->path] = '\0';
        const char *field = dot + 1;
        /* The field's reflection info guides the formatting. */
        const de_type_id t = de_type_find(types, comp);
        u32 nf = 0, fi = 0;
        const de_field_info *fields = t.id ? de_type_fields(types, t, &nf) : NULL;
        while (fields != NULL && fi < nf && strcmp(fields[fi].name, field) != 0)
            ++fi;
        if (fields == NULL || fi == nf) {
            why = failf(msg, msg_cap, "value", "no type info for %s", ed->path);
            break;
        }
        mpack_tree_t tree;
        mpack_tree_init_data(&tree, (const char *)ed->val, ed->val_len);
        mpack_tree_parse(&tree);
        sbuf val = {0};
        const bool ok = mpack_tree_error(&tree) == mpack_ok &&
                        put_value(&val, types, t, fi, &fields[fi], mpack_tree_root(&tree));
        (void)mpack_tree_destroy(&tree);
        if (!ok || val.oom) {
            sb_free(&val);
            why = failf(msg, msg_cap, "value", "%s has no source spelling for its value", ed->path);
            break;
        }
        if (!scan_all(&c, cur.p, cur.len)) {
            sb_free(&val);
            why = failf(msg, msg_cap, c.oom ? "memory" : "source_layout",
                        c.oom ? "out of memory" : "the source is not TOML the editor can edit");
            break;
        }
        const loc l = locate(&c, ed->sid, comp, field);
        sbuf next = {0};
        if (l.what == LOC_KEY) {
            const item *it = &c.items[l.key];
            sb_put(&next, cur.p, it->vs);
            sb_put(&next, val.p, val.len);
            sb_put(&next, cur.p + it->ve, cur.len - it->ve);
        } else if (l.what == LOC_TABLE && !ed->follow) {
            sb_put(&next, cur.p, l.after);
            if (l.after > 0 && cur.p[l.after - 1] != '\n')
                sb_puts(&next, eol);
            sb_puts(&next, field);
            sb_puts(&next, " = ");
            sb_put(&next, val.p, val.len);
            sb_puts(&next, eol);
            sb_put(&next, cur.p + l.after, cur.len - l.after);
        } else if (ed->follow && (l.what == LOC_TABLE || l.what == LOC_NO_TABLE)) {
            /* A level that inherits the field keeps inheriting it. */
            sb_free(&val);
            continue;
        } else {
            sb_free(&val);
            why = failf(msg, msg_cap, "source_layout", "the source does not hold %s of 0x%llx in "
                        "a [entity.%s] table", ed->path, (unsigned long long)ed->sid, comp);
            break;
        }
        sb_free(&val);
        if (next.oom) {
            sb_free(&next);
            why = failf(msg, msg_cap, "memory", "out of memory");
            break;
        }
        sb_free(&cur);
        cur = next;
    }
    scan_free(&c);
    if (why == NULL) {
        toml_result_t res = de__toml_parse(NULL, cur.p, (int)cur.len);
        if (!res.ok)
            why = failf(msg, msg_cap, "source_layout", "the edited source does not parse: %s",
                        res.errmsg);
        de__toml_free(res);
    }
    if (why != NULL) {
        sb_free(&cur);
        return why;
    }
    *out = cur.p;
    *out_len = cur.len;
    /* The caller frees with de__ed_prefab_source_free(*out, *out_len): an exact block. */
    if (cur.cap > cur.len + 1) {
        char *shrunk = (char *)DE_REALLOC(de_heap_allocator(), cur.p, cur.cap, cur.len + 1, 1,
                                          DE_MEM_GENERAL);
        if (shrunk != NULL)
            *out = shrunk;
        else {
            /* Keep the bigger block: copy into an exact one. */
            char *exact = (char *)DE_ALLOC(de_heap_allocator(), cur.len + 1, 1, DE_MEM_GENERAL);
            if (exact == NULL) {
                sb_free(&cur);
                *out = NULL;
                return failf(msg, msg_cap, "memory", "out of memory");
            }
            memcpy(exact, cur.p, cur.len + 1);
            sb_free(&cur);
            *out = exact;
        }
    }
    return NULL;
}

void de__ed_prefab_source_free(char *text, size_t len) {
    if (text != NULL)
        DE_FREE(de_heap_allocator(), text, len + 1);
}
