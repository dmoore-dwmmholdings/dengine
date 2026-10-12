/*
 * schema_hash.c: the schema hash of struct types and enums (DENG-294).
 *
 * The hash is a cross-platform contract (binary files and the golden test store it), so it is
 * computed over an explicit byte encoding, never over a struct in memory (no padding, no
 * pointers):
 *
 *   type:  str("de.type.v1") str(name) u32(version) u32(size) u32(field_count)
 *          per field: str(name) u32(kind) u32(size) u32(offset)
 *                     u32(flags & DE_FIELD_SCHEMA_FLAGS) u64(nested) u32(array_count)
 *   enum:  str("de.enum.v1") str(name) u32(is_flags) u32(count)
 *          per value: str(name) u64(value as two's complement)
 *
 *   str(s) = u32(byte length) followed by the bytes (no NUL); all integers little-endian.
 *   nested = 0 for kinds other than STRUCT, ENUM, FLAGS, FIXED_ARRAY and ASSET; otherwise
 *            the schema hash of the registered type or enum named by attrs.nested_type, the
 *            de_hash64_str of the name when it names no registered type (built-in element
 *            kinds) or the field is an ASSET (the asset type name), or 0 when nested_type is NULL.
 * array_count is 0 except for FIXED_ARRAY.
 *
 * The bytes go through a 256-byte buffer; each full buffer is hashed with de_hash64 seeded by the
 * running hash (seed 0 for the first), and so is the final partial buffer. Range, units and
 * tooltip are not hashed, so editing them never invalidates saved data.
 */
#include "reflect_internal.h"

#include <string.h>

#define HASHER_BUF 256u

typedef struct hasher {
    u8 buf[HASHER_BUF];
    u32 len;
    u64 h;
} hasher;

static void h_flush(hasher *s) {
    s->h = de_hash64(s->buf, s->len, s->h);
    s->len = 0;
}

static void h_bytes(hasher *s, const void *data, size_t n) {
    const u8 *p = (const u8 *)data;
    while (n > 0) {
        size_t room = HASHER_BUF - s->len;
        size_t take = n < room ? n : room;
        memcpy(s->buf + s->len, p, take);
        s->len += (u32)take;
        p += take;
        n -= take;
        if (s->len == HASHER_BUF)
            h_flush(s);
    }
}

static void h_u32(hasher *s, u32 v) {
    u8 b[4] = {(u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24)};
    h_bytes(s, b, sizeof b);
}

static void h_u64(hasher *s, u64 v) {
    h_u32(s, (u32)v);
    h_u32(s, (u32)(v >> 32));
}

static void h_str(hasher *s, const char *str) {
    size_t n = str ? strlen(str) : 0;
    h_u32(s, (u32)n);
    h_bytes(s, str, n);
}

static u64 h_finish(hasher *s) {
    h_flush(s);
    return s->h;
}

u64 de__reflect_hash_enum(const de_enum_info *e) {
    hasher s;
    s.len = 0;
    s.h = 0;
    h_str(&s, "de.enum.v1");
    h_str(&s, e->name);
    h_u32(&s, e->is_flags ? 1u : 0u);
    h_u32(&s, e->count);
    for (u32 i = 0; i < e->count; ++i) {
        h_str(&s, e->values[i].name);
        h_u64(&s, (u64)e->values[i].value);
    }
    return h_finish(&s);
}

/* The nested hash of one field (see the file comment). */
static u64 nested_hash(const de_type_registry *r, const char *nested, u32 depth) {
    if (!nested)
        return 0;
    const de__type_entry *e = de__reflect_find_entry(r, nested);
    if (!e)
        return de_hash64_str(nested);
    if (e->kind == DE__ENTRY_ENUM)
        return de__reflect_hash_enum(e->enum_info);
    /* Recompute from the nested type's current layout, so the result does not depend on the
     * order in which cached hashes are refreshed. Validation rejects cycles; the depth limit is
     * only a guard. */
    if (depth >= DE__TYPE_MAX_DEPTH)
        return 0;
    return de__reflect_hash_type(r, &e->cur->desc, depth + 1);
}

u64 de__reflect_hash_type(const de_type_registry *r, const de_type_desc *desc, u32 depth) {
    hasher s;
    s.len = 0;
    s.h = 0;
    h_str(&s, "de.type.v1");
    h_str(&s, desc->name);
    h_u32(&s, desc->version);
    h_u32(&s, desc->size);
    h_u32(&s, desc->field_count);
    for (u32 i = 0; i < desc->field_count; ++i) {
        const de_field_info *f = &desc->fields[i];
        h_str(&s, f->name);
        h_u32(&s, (u32)f->kind);
        h_u32(&s, f->size);
        h_u32(&s, f->offset);
        h_u32(&s, f->flags & (u32)DE_FIELD_SCHEMA_FLAGS);
        const char *nested = f->attrs.nested_type;
        u64 nh = 0;
        if (f->kind == DE_FK_ASSET)
            nh = nested ? de_hash64_str(nested) : 0; /* an asset type, never a registered type */
        else if (f->kind == DE_FK_STRUCT || f->kind == DE_FK_ENUM || f->kind == DE_FK_FLAGS ||
                 f->kind == DE_FK_FIXED_ARRAY)
            nh = nested_hash(r, nested, depth);
        h_u64(&s, nh);
        h_u32(&s, f->kind == DE_FK_FIXED_ARRAY ? f->attrs.array_count : 0);
    }
    return h_finish(&s);
}

void de__reflect_rehash_all(de_type_registry *r) {
    de__type_entry *entries = (de__type_entry *)r->entries.data;
    for (u32 i = 0; i < r->entries.count; ++i) {
        de__type_entry *e = &entries[i];
        e->hash = e->kind == DE__ENTRY_ENUM ? de__reflect_hash_enum(e->enum_info)
                                            : de__reflect_hash_type(r, &e->cur->desc, 0);
    }
}
