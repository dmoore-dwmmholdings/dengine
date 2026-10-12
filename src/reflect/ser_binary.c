/*
 * ser_binary.c: the versioned little-endian binary serializer (DENG-296, SPEC §9.2), and the
 * memory reader/writer helpers. The byte layout is in ser_binary_format.h.
 *
 * Writing: header, optional schema block (type table in topological order), then the present
 * fields. A POD type whose packed encoding equals its memory image is written with one write of
 * the object (the fast path); otherwise the payload is measured first (strings vary) and then
 * encoded field by field through a 1 KiB batch buffer, so every de_ser_context callback runs
 * exactly once per value.
 *
 * Reading has three paths:
 *   1. Stored schema hash == current: decode with the current layout (memcpy fast path, or field
 *      by field, skipping fields the reader's required_flags do not select).
 *   2. Hash differs, schema block present: parse and validate the stored type table, decode the
 *      payload into an image of the OLD layout (old offsets, sizes, kinds) in thread scratch,
 *      then copy matching fields (by name hash) into the object, run migrate hooks (nested types
 *      first) and log at most one warning for what was not loaded.
 *   3. Hash differs, no schema block: DE_ERR_VERSION_MISMATCH.
 *
 * Untrusted input: every read goes through a region with a byte budget (the payload size), the
 * schema block is parsed from memory with bounds checks, every stored field is checked to lie
 * inside its stored type, nested references point forward only (no cycles), nesting depth is
 * limited and the old-image decode has a work budget. Corrupt data is DE_ERR_PARSE.
 */
#include "reflect_internal.h"
#include "ser_binary_format.h"

#include <stdio.h>
#include <string.h>

#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
#define DE__BIN_DEFAULT_SELF_DESCRIBING true
#else
#define DE__BIN_DEFAULT_SELF_DESCRIBING false
#endif

#define BIN_BUF 1024u

/* ---- Little-endian helpers ------------------------------------------------------------------ */

static void put_u16(u8 *p, u16 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void put_u32(u8 *p, u32 v) {
    for (u32 i = 0; i < 4; ++i)
        p[i] = (u8)(v >> (8 * i));
}

static void put_u64(u8 *p, u64 v) {
    for (u32 i = 0; i < 8; ++i)
        p[i] = (u8)(v >> (8 * i));
}

static u16 get_u16(const u8 *p) {
    return (u16)(p[0] | (p[1] << 8));
}

static u32 get_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 get_u64(const u8 *p) {
    return (u64)get_u32(p) | ((u64)get_u32(p + 4) << 32);
}

/* ---- Field selection and the fast-path summary --------------------------------------------- */

static bool selected(u32 flags, u32 req) {
    return (flags & req) == req;
}

/* true if every field of a type with summary `sum` is selected by `req`. */
static bool all_selected(u32 sum, u32 req) {
    return (req & ~(u32)DE_FIELD_SCHEMA_FLAGS) == 0 && ((sum & DE__BIN_SUM_FLAGS_AND) & req) == req;
}

/* Kinds whose encoding is their memory image (BOOL is normalised to 0/1, STRING has a length,
 * ASSET widens to u64; STRUCT and FIXED_ARRAY depend on their contents). */
static bool kind_packs(de_field_kind k) {
    switch (k) {
    case DE_FK_BOOL:
    case DE_FK_STRING:
    case DE_FK_ASSET:
    case DE_FK_STRUCT:
    case DE_FK_FIXED_ARRAY:
        return false;
    default:
        return (u32)k < DE_FIELD_KIND_COUNT;
    }
}

static u32 value_summary(const de_type_registry *r, de_field_kind k, const char *nested, u32 size,
                         u32 depth) {
    if (k == DE_FK_STRUCT) {
        const de__type_layout *nl = nested ? de__reflect_find_layout(r, nested) : NULL;
        if (!nl || nl->desc.size != size || depth >= DE__TYPE_MAX_DEPTH)
            return DE__BIN_SUM_FLAGS_AND; /* not packed */
        return de__ser_binary_summary(r, nl, depth + 1);
    }
    u32 s = DE__BIN_SUM_FLAGS_AND;
    if (kind_packs(k))
        s |= DE__BIN_SUM_PACKED;
    if (k == DE_FK_ENTITY)
        s |= DE__BIN_SUM_HAS_ENTITY;
    return s;
}

u32 de__ser_binary_summary(const de_type_registry *r, const de__type_layout *l, u32 depth) {
    u32 and_flags = DE__BIN_SUM_FLAGS_AND;
    bool packed = l->desc.field_count > 0;
    bool entity = false;
    u32 expect = 0;
    for (u32 i = 0; i < l->desc.field_count; ++i) {
        const de_field_info *f = &l->fields[i];
        u32 vs;
        if (f->kind == DE_FK_FIXED_ARRAY) {
            const de_field_info *el = &l->extra[i].element;
            vs = value_summary(r, el->kind, el->attrs.nested_type, el->size, depth);
        } else {
            vs = value_summary(r, f->kind, f->attrs.nested_type, f->size, depth);
        }
        and_flags &= (f->flags & (u32)DE_FIELD_SCHEMA_FLAGS) & (vs & DE__BIN_SUM_FLAGS_AND);
        packed = packed && (vs & DE__BIN_SUM_PACKED) && f->offset == expect;
        entity = entity || (vs & DE__BIN_SUM_HAS_ENTITY);
        expect = f->offset + f->size;
    }
    packed = packed && expect == l->desc.size;
    return and_flags | (packed ? DE__BIN_SUM_PACKED : 0u) | (entity ? DE__BIN_SUM_HAS_ENTITY : 0u);
}

/* The nested hash of a field, as the schema hash defines it (schema_hash.c). */
static u64 field_nested_hash(const de_type_registry *r, const de_field_info *f) {
    const char *n = f->attrs.nested_type;
    if (!n)
        return 0;
    if (f->kind == DE_FK_ASSET)
        return de_hash64_str(n);
    if (f->kind == DE_FK_STRUCT || f->kind == DE_FK_ENUM || f->kind == DE_FK_FLAGS ||
        f->kind == DE_FK_FIXED_ARRAY) {
        const de__type_entry *e = de__reflect_find_entry(r, n);
        return e ? e->hash : de_hash64_str(n);
    }
    return 0;
}

/* The nested struct layout `name` if it still has `size` bytes, else NULL (a nested type that a
 * hot reload resized without re-registering its users). */
static const de__type_layout *nested_layout(const de_type_registry *r, const char *name, u32 size) {
    const de__type_layout *l = name ? de__reflect_find_layout(r, name) : NULL;
    return (l && l->desc.size == size) ? l : NULL;
}

static de_log_category log_cat(void) {
    return de_log_register_category("reflect", DE_LOG_INFO);
}

/* ---- Output: batched writes, or counting only ----------------------------------------------- */

typedef struct bin_out {
    de_writer *w; /* NULL: count bytes only */
    u64 bytes;
    de_result err;
    u32 len;
    u8 buf[BIN_BUF];
} bin_out;

static void out_init(bin_out *o, de_writer *w) {
    o->w = w;
    o->bytes = 0;
    o->err = DE_OK;
    o->len = 0;
}

static void out_flush(bin_out *o) {
    if (o->w && o->len && o->err == DE_OK)
        o->err = o->w->write(o->w->user, o->buf, o->len);
    o->len = 0;
}

static void out_bytes(bin_out *o, const void *p, size_t n) {
    o->bytes += n;
    if (!o->w || o->err != DE_OK || n == 0)
        return;
    if (n > BIN_BUF - o->len) {
        out_flush(o);
        if (n >= BIN_BUF) {
            if (o->err == DE_OK)
                o->err = o->w->write(o->w->user, p, n);
            return;
        }
    }
    memcpy(o->buf + o->len, p, n);
    o->len += (u32)n;
}

static void out_u8(bin_out *o, u8 v) {
    out_bytes(o, &v, 1);
}

static void out_u32(bin_out *o, u32 v) {
    u8 b[4];
    put_u32(b, v);
    out_bytes(o, b, 4);
}

static void out_u64(bin_out *o, u64 v) {
    u8 b[8];
    put_u64(b, v);
    out_bytes(o, b, 8);
}

static void out_str(bin_out *o, const char *s) {
    size_t n = s ? strlen(s) : 0;
    out_u32(o, (u32)n);
    out_bytes(o, s, n);
}

/* ---- Encoding with the current layout ------------------------------------------------------- */

typedef struct enc_state {
    const de_type_registry *r;
    const de_ser_context *ctx;
    const char *type_name;
    u32 req;
    bin_out *o;
    de_result err;
} enc_state;

static void enc_struct(enc_state *s, const de__type_layout *l, const u8 *obj, u32 depth);

static void enc_value(enc_state *s, de_field_kind k, u32 size, const char *nested, const u8 *src,
                      u32 depth) {
    bool real = s->o->w != NULL; /* the measuring pass calls no callback */
    switch (k) {
    case DE_FK_BOOL:
        out_u8(s->o, src[0] != 0 ? 1u : 0u);
        break;
    case DE_FK_STRING: {
        const u8 *nul = (const u8 *)memchr(src, 0, size);
        size_t n = nul ? (size_t)(nul - src) : size;
        if (n > (size_t)size - 1)
            n = (size_t)size - 1;
        out_u32(s->o, (u32)n);
        out_bytes(s->o, src, n);
        break;
    }
    case DE_FK_ENTITY: {
        u64 v;
        memcpy(&v, src, 8);
        if (real && s->ctx && s->ctx->entity_to_scene_id)
            v = s->ctx->entity_to_scene_id(v, s->ctx->user);
        out_u64(s->o, v);
        break;
    }
    case DE_FK_ASSET: {
        u32 h;
        memcpy(&h, src, 4);
        u64 v = h;
        if (real && s->ctx && s->ctx->asset_to_id)
            v = s->ctx->asset_to_id(h, nested, s->ctx->user);
        out_u64(s->o, v);
        break;
    }
    case DE_FK_STRUCT: {
        const de__type_layout *nl = nested_layout(s->r, nested, size);
        if (!nl || depth >= DE__TYPE_MAX_DEPTH) {
            if (s->err == DE_OK)
                s->err = DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                                 "type '%s': nested type '%s' is missing or no longer %u bytes "
                                 "(re-register the types that nest it)",
                                 s->type_name, nested ? nested : "(null)", size);
            return;
        }
        enc_struct(s, nl, src, depth + 1);
        break;
    }
    default: /* numbers, vectors, matrices, names, enums, flags: the memory image (LE host) */
        out_bytes(s->o, src, size);
        break;
    }
}

static void enc_struct(enc_state *s, const de__type_layout *l, const u8 *obj, u32 depth) {
    for (u32 i = 0; i < l->desc.field_count && s->err == DE_OK; ++i) {
        const de_field_info *f = &l->fields[i];
        if (!selected(f->flags, s->req))
            continue;
        if (f->kind == DE_FK_FIXED_ARRAY) {
            const de_field_info *el = &l->extra[i].element;
            for (u32 e = 0; e < f->attrs.array_count; ++e)
                enc_value(s, el->kind, el->size, el->attrs.nested_type,
                          obj + f->offset + (size_t)e * el->size, depth);
        } else {
            enc_value(s, f->kind, f->size, f->attrs.nested_type, obj + f->offset, depth);
        }
    }
}

/* ---- Schema block writing ------------------------------------------------------------------- */

/* Most distinct struct types one object's schema block lists (the object's type included). */
#define BIN_MAX_SCHEMA_TYPES 256u

typedef struct type_list {
    const de__type_entry *items[BIN_MAX_SCHEMA_TYPES];
    u32 count;
    bool overflow;
} type_list;

static bool list_has(const type_list *t, const de__type_entry *e) {
    for (u32 i = 0; i < t->count; ++i) {
        if (t->items[i] == e)
            return true;
    }
    return false;
}

/* Post-order DFS over the struct types reachable through present fields. Types are acyclic
 * (validated at registration); the depth limit is a guard. */
static void collect_types(const de_type_registry *r, const de__type_entry *e, u32 req,
                          type_list *out, u32 depth) {
    if (depth >= DE__TYPE_MAX_DEPTH || list_has(out, e))
        return;
    const de__type_layout *l = e->cur;
    for (u32 i = 0; i < l->desc.field_count; ++i) {
        const de_field_info *f = &l->fields[i];
        if (!selected(f->flags, req))
            continue;
        const char *nested = NULL;
        if (f->kind == DE_FK_STRUCT)
            nested = f->attrs.nested_type;
        else if (f->kind == DE_FK_FIXED_ARRAY && l->extra[i].element.kind == DE_FK_STRUCT)
            nested = l->extra[i].element.attrs.nested_type;
        const de__type_entry *ne = nested ? de__reflect_find_entry(r, nested) : NULL;
        if (ne && ne->kind == DE__ENTRY_STRUCT)
            collect_types(r, ne, req, out, depth + 1);
    }
    if (list_has(out, e))
        return;
    if (out->count == BIN_MAX_SCHEMA_TYPES)
        out->overflow = true;
    else
        out->items[out->count++] = e;
}

static void write_type_record(bin_out *o, const de_type_registry *r, const de__type_entry *e,
                              u32 req) {
    const de__type_layout *l = e->cur;
    u32 present = 0;
    for (u32 i = 0; i < l->desc.field_count; ++i)
        present += selected(l->fields[i].flags, req) ? 1u : 0u;
    out_u64(o, e->hash);
    out_u64(o, e->name_hash);
    out_u32(o, l->desc.version);
    out_u32(o, l->desc.size);
    out_u32(o, present);
    out_str(o, l->desc.name);
    for (u32 i = 0; i < l->desc.field_count; ++i) {
        const de_field_info *f = &l->fields[i];
        if (!selected(f->flags, req))
            continue;
        bool arr = f->kind == DE_FK_FIXED_ARRAY;
        out_u64(o, de_hash64_str(f->name));
        out_u64(o, field_nested_hash(r, f));
        out_u32(o, f->size);
        out_u32(o, f->offset);
        out_u32(o, f->flags);
        out_u32(o, arr ? f->attrs.array_count : 0u);
        out_u8(o, (u8)f->kind);
        out_u8(o, arr ? (u8)l->extra[i].element.kind : 0u);
        out_str(o, f->name);
        out_str(o, f->attrs.nested_type);
    }
}

/* Writes the schema block (block size first) for type `e`. Allocates nothing, so a writer that
 * allocates (even from thread scratch) is safe to call. */
static de_result write_schema(bin_out *o, const de_type_registry *r, const de__type_entry *e,
                              u32 req) {
    type_list tl;
    tl.count = 0;
    tl.overflow = false;
    collect_types(r, e, req, &tl, 0);
    if (tl.overflow)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "type '%s': nests more than %u struct types", e->name,
                       BIN_MAX_SCHEMA_TYPES);

    /* Reverse post-order: e first, and every type before the types it nests. */
    bin_out m;
    out_init(&m, NULL);
    out_u32(&m, tl.count);
    for (u32 i = tl.count; i-- > 0;)
        write_type_record(&m, r, tl.items[i], req);
    if (m.bytes > DE__BIN_MAX_SCHEMA_BYTES)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "type '%s': schema block of %llu bytes is too large",
                       e->name, (unsigned long long)m.bytes);
    out_u32(o, (u32)m.bytes);
    out_u32(o, tl.count);
    for (u32 i = tl.count; i-- > 0;)
        write_type_record(o, r, tl.items[i], req);
    return DE_OK;
}

/* ---- Serialize ------------------------------------------------------------------------------ */

static const de__type_entry *struct_entry(const de_type_registry *r, de_type_id type) {
    const de__type_entry *e = r ? de__reflect_entry(r, type) : NULL;
    return (e && e->kind == DE__ENTRY_STRUCT) ? e : NULL;
}

de_result de_serialize_binary(const de_type_registry *r, de_type_id type, const void *obj,
                              const de_ser_options *opt, const de_ser_binary_options *bopt,
                              de_writer *w) {
    const de__type_entry *e = struct_entry(r, type);
    if (!e || !obj || !w || !w->write)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_serialize_binary: NULL argument or not a struct type id");
    u32 req = opt ? opt->required_flags : 0u;
    if (req > 0xFFu)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_serialize_binary: type '%s': required_flags 0x%x above 0xFF", e->name,
                       req);
    const de_ser_context *ctx = opt ? opt->ctx : NULL;
    de_ser_binary_stats *stats = bopt ? bopt->stats : NULL;
    bool self = DE__BIN_DEFAULT_SELF_DESCRIBING;
    if (bopt && bopt->schema == DE_SER_BINARY_SELF_DESCRIBING)
        self = true;
    else if (bopt && bopt->schema == DE_SER_BINARY_COMPACT)
        self = false;

    const de_type_desc *d = &e->cur->desc;
    u32 sum = e->bin_summary;
    bool fast = !(bopt && bopt->disable_fast_path) && d->is_pod && (sum & DE__BIN_SUM_PACKED) &&
                all_selected(sum, req) &&
                !((sum & DE__BIN_SUM_HAS_ENTITY) && ctx && ctx->entity_to_scene_id);

    enc_state s;
    s.r = r;
    s.ctx = ctx;
    s.type_name = e->name;
    s.req = req;
    s.err = DE_OK;

    u64 payload = d->size;
    if (!fast) {
        bin_out m; /* measuring pass: strings make the payload size vary */
        out_init(&m, NULL);
        s.o = &m;
        enc_struct(&s, e->cur, (const u8 *)obj, 0);
        if (s.err != DE_OK)
            return s.err;
        payload = m.bytes;
    }
    if (payload > 0xFFFFFFFFull)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "type '%s': payload of %llu bytes is too large",
                       e->name, (unsigned long long)payload);

    bin_out o;
    out_init(&o, w);
    u8 h[DE__BIN_HEADER_SIZE];
    h[0] = DE__BIN_MAGIC0;
    h[1] = DE__BIN_MAGIC1;
    h[2] = DE__BIN_MAGIC2;
    h[3] = DE__BIN_MAGIC3;
    put_u16(h + 4, (u16)DE__BIN_FORMAT_VERSION);
    put_u16(h + 6,
            (u16)((self ? DE__BIN_FLAG_SELF_DESCRIBING : 0u) | (req << DE__BIN_FLAGS_REQ_SHIFT)));
    put_u64(h + 8, e->name_hash);
    put_u32(h + 16, d->version);
    put_u64(h + 20, e->hash);
    put_u32(h + 28, (u32)payload);
    out_bytes(&o, h, sizeof h);
    if (self)
        DE_TRY(write_schema(&o, r, e, req));
    if (fast) {
        out_bytes(&o, obj, d->size);
    } else {
        s.o = &o;
        enc_struct(&s, e->cur, (const u8 *)obj, 0);
        if (s.err != DE_OK)
            return s.err;
    }
    out_flush(&o);
    if (o.err != DE_OK)
        return DE_FAIL(o.err, "type '%s': the writer failed (%s)", e->name, de_result_str(o.err));
    if (stats) {
        stats->objects_written++;
        if (fast)
            stats->fast_path++;
        else
            stats->field_wise++;
    }
    return DE_OK;
}

/* ---- Input: exact reads and a bounded, buffered region -------------------------------------- */

/* Reads exactly `n` bytes; the end of the data is DE_ERR_PARSE. */
static de_result read_exact(de_reader *rd, void *dst, size_t n, const char *what) {
    u8 *p = (u8 *)dst;
    while (n > 0) {
        size_t got = 0;
        de_result res = rd->read(rd->user, p, n, &got);
        if (res != DE_OK)
            return DE_FAIL(res, "binary object: reading %s failed (%s)", what, de_result_str(res));
        if (got == 0)
            return DE_FAIL(DE_ERR_PARSE, "binary object: truncated %s", what);
        if (got > n)
            return DE_FAIL(DE_ERR_INTERNAL, "binary object: the reader returned too many bytes");
        p += got;
        n -= got;
    }
    return DE_OK;
}

static de_result skip_exact(de_reader *rd, u64 n, const char *what) {
    u8 tmp[256];
    while (n > 0) {
        size_t step = n < sizeof tmp ? (size_t)n : sizeof tmp;
        DE_TRY(read_exact(rd, tmp, step, what));
        n -= step;
    }
    return DE_OK;
}

typedef struct bin_in {
    de_reader *rd;
    u64 left; /* region bytes not yet consumed (buffered ones included) */
    u32 pos, len;
    u8 buf[BIN_BUF];
} bin_in;

static void in_init(bin_in *in, de_reader *rd, u64 size) {
    in->rd = rd;
    in->left = size;
    in->pos = in->len = 0;
}

static de_result in_short(void) {
    return DE_FAIL(DE_ERR_PARSE, "binary object: a value runs past the end of the payload");
}

static de_result in_bytes_slow(bin_in *in, u8 *dst, size_t n) {
    if (n > in->left)
        return in_short();
    size_t have = in->len - in->pos;
    if (have > 0) { /* have < n here */
        memcpy(dst, in->buf + in->pos, have);
        dst += have;
        n -= have;
        in->left -= have;
        in->pos = in->len = 0;
    }
    if (n >= BIN_BUF) {
        DE_TRY(read_exact(in->rd, dst, n, "payload"));
        in->left -= n;
        return DE_OK;
    }
    u32 fill = (u32)(in->left < BIN_BUF ? in->left : BIN_BUF);
    DE_TRY(read_exact(in->rd, in->buf, fill, "payload"));
    in->len = fill;
    memcpy(dst, in->buf, n);
    in->pos = (u32)n;
    in->left -= n;
    return DE_OK;
}

/* `dst` NULL skips. */
static inline de_result in_bytes(bin_in *in, void *dst, size_t n) {
    if (in->len - in->pos >= n) {
        if (dst)
            memcpy(dst, in->buf + in->pos, n);
        in->pos += (u32)n;
        in->left -= n;
        return DE_OK;
    }
    if (dst)
        return in_bytes_slow(in, (u8 *)dst, n);
    u8 tmp[256];
    while (n > 0) {
        size_t step = n < sizeof tmp ? n : sizeof tmp;
        DE_TRY(in_bytes(in, tmp, step));
        n -= step;
    }
    return DE_OK;
}

static de_result in_u32(bin_in *in, u32 *v) {
    u8 b[4];
    DE_TRY(in_bytes(in, b, 4));
    *v = get_u32(b);
    return DE_OK;
}

static de_result in_u64(bin_in *in, u64 *v) {
    u8 b[8];
    DE_TRY(in_bytes(in, b, 8));
    *v = get_u64(b);
    return DE_OK;
}

/* One non-STRUCT value into `dst` (NULL: consume only). Entities and assets are mapped back
 * through `ctx`; `asset_type` is passed to id_to_asset. */
static de_result dec_leaf(bin_in *in, const de_ser_context *ctx, de_field_kind k, u32 size,
                          const char *asset_type, u8 *dst) {
    switch (k) {
    case DE_FK_BOOL: {
        u8 b;
        DE_TRY(in_bytes(in, &b, 1));
        if (b > 1)
            return DE_FAIL(DE_ERR_PARSE, "binary object: bool value %u", (unsigned)b);
        if (dst)
            dst[0] = b;
        return DE_OK;
    }
    case DE_FK_STRING: {
        u32 n;
        DE_TRY(in_u32(in, &n));
        if (n >= size)
            return DE_FAIL(DE_ERR_PARSE, "binary object: string of %u bytes for a char[%u]", n,
                           size);
        DE_TRY(in_bytes(in, dst, n));
        if (dst)
            memset(dst + n, 0, size - n);
        return DE_OK;
    }
    case DE_FK_ENTITY: {
        u64 v;
        DE_TRY(in_u64(in, &v));
        if (dst) {
            if (ctx && ctx->scene_id_to_entity)
                v = ctx->scene_id_to_entity(v, ctx->user);
            memcpy(dst, &v, 8);
        }
        return DE_OK;
    }
    case DE_FK_ASSET: {
        u64 v;
        DE_TRY(in_u64(in, &v));
        if (dst) {
            u32 h;
            if (ctx && ctx->id_to_asset) {
                h = ctx->id_to_asset(v, asset_type, ctx->user);
            } else {
                if (v > 0xFFFFFFFFull)
                    return DE_FAIL(DE_ERR_PARSE,
                                   "binary object: asset value 0x%016llx is no handle (written "
                                   "with asset_to_id? read with id_to_asset)",
                                   (unsigned long long)v);
                h = (u32)v;
            }
            memcpy(dst, &h, 4);
        }
        return DE_OK;
    }
    default:
        return in_bytes(in, dst, size);
    }
}

/* ---- Path 1: decoding with the current layout ----------------------------------------------- */

typedef struct dec_state {
    const de_type_registry *r;
    const de_ser_context *ctx;
    const char *type_name;
    u32 req;        /* the reader's filter */
    u32 stored_req; /* the writer's filter: which fields are present */
    bin_in *in;
} dec_state;

static de_result dec_struct(dec_state *s, const de__type_layout *l, u8 *obj, u32 depth);

static de_result dec_value(dec_state *s, de_field_kind k, u32 size, const char *nested, u8 *dst,
                           u32 depth) {
    if (k != DE_FK_STRUCT)
        return dec_leaf(s->in, s->ctx, k, size, nested, dst);
    const de__type_layout *nl = nested_layout(s->r, nested, size);
    if (!nl || depth >= DE__TYPE_MAX_DEPTH)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "type '%s': nested type '%s' is missing or no longer %u bytes", s->type_name,
                       nested ? nested : "(null)", size);
    return dec_struct(s, nl, dst, depth + 1);
}

/* `obj` NULL consumes the present fields without storing them. */
static de_result dec_struct(dec_state *s, const de__type_layout *l, u8 *obj, u32 depth) {
    for (u32 i = 0; i < l->desc.field_count; ++i) {
        const de_field_info *f = &l->fields[i];
        if (!selected(f->flags, s->stored_req))
            continue;
        u8 *fd = (obj && selected(f->flags, s->req)) ? obj + f->offset : NULL;
        if (f->kind == DE_FK_FIXED_ARRAY) {
            const de_field_info *el = &l->extra[i].element;
            for (u32 e = 0; e < f->attrs.array_count; ++e)
                DE_TRY(dec_value(s, el->kind, el->size, el->attrs.nested_type,
                                 fd ? fd + (size_t)e * el->size : NULL, depth));
        } else {
            DE_TRY(dec_value(s, f->kind, f->size, f->attrs.nested_type, fd, depth));
        }
    }
    return DE_OK;
}

/* ---- Path 2: the stored schema -------------------------------------------------------------- */

typedef struct sb_field {
    u64 name_hash, nested_hash;
    de_field_kind elem_kind;
    u32 elem_size;
    u32 nested; /* table index of the nested struct (STRUCT, FIXED_ARRAY of STRUCT) */
    bool noted; /* already counted as not loaded */
} sb_field;

typedef struct sb_type {
    u64 schema_hash, name_hash;
    const char *name;
    u32 version, size, field_count;
    sb_field *f;
    de_field_info *info; /* old_fields for migrate: offsets into this type's old image */
} sb_type;

typedef struct sb_table {
    sb_type *t;
    u32 count;
} sb_table;

/* A cursor over the schema block in memory. */
typedef struct mcur {
    const u8 *p;
    size_t left;
} mcur;

static bool m_take(mcur *c, size_t n, const u8 **out) {
    if (n > c->left)
        return false;
    *out = c->p;
    c->p += n;
    c->left -= n;
    return true;
}

static bool m_u8(mcur *c, u8 *v) {
    const u8 *p;
    if (!m_take(c, 1, &p))
        return false;
    *v = p[0];
    return true;
}

static bool m_u32(mcur *c, u32 *v) {
    const u8 *p;
    if (!m_take(c, 4, &p))
        return false;
    *v = get_u32(p);
    return true;
}

static bool m_u64(mcur *c, u64 *v) {
    const u8 *p;
    if (!m_take(c, 8, &p))
        return false;
    *v = get_u64(p);
    return true;
}

/* A string copied NUL-terminated into the arena; empty gives NULL when `empty_null`. */
static de_result m_str(mcur *c, de_arena *a, bool empty_null, const char **out) {
    u32 n;
    const u8 *p;
    if (!m_u32(c, &n) || !m_take(c, n, &p))
        return DE_FAIL(DE_ERR_PARSE, "binary schema: truncated string");
    if (n == 0 && empty_null) {
        *out = NULL;
        return DE_OK;
    }
    char *s = (char *)de_arena_push(a, (size_t)n + 1, 1);
    if (!s)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "binary schema: string");
    memcpy(s, p, n);
    s[n] = 0;
    *out = s;
    return DE_OK;
}

static de_result schema_corrupt(const char *what, u32 ti, u32 fi) {
    return DE_FAIL(DE_ERR_PARSE, "binary schema: type #%u field #%u: %s", ti, fi, what);
}

static bool is_int_size(u32 n) {
    return n == 1 || n == 2 || n == 4 || n == 8;
}

/* The table index after `ti` of the struct with `hash` and `size` bytes, or 0 (never valid:
 * entry 0 is the object's own type, which nothing may nest). */
static u32 find_nested(const sb_table *t, u32 ti, u64 hash, u32 size) {
    for (u32 j = ti + 1; j < t->count; ++j) {
        if (t->t[j].schema_hash == hash)
            return t->t[j].size == size ? j : 0;
    }
    return 0;
}

/* Kind/size rule of one stored value (a field, or an array element). */
static bool value_valid(const sb_table *t, u32 ti, de_field_kind k, u32 size, u64 nested_hash,
                        u32 *out_nested) {
    *out_nested = 0;
    if ((u32)k >= DE_FIELD_KIND_COUNT || k == DE_FK_FIXED_ARRAY || size == 0)
        return false;
    u32 fixed = de__reflect_kind_fixed_size(k);
    if (fixed != 0)
        return size == fixed;
    switch (k) {
    case DE_FK_STRING:
        return true;
    case DE_FK_ENUM:
    case DE_FK_FLAGS:
        return is_int_size(size);
    case DE_FK_STRUCT:
        *out_nested = find_nested(t, ti, nested_hash, size);
        return *out_nested != 0;
    default:
        return false;
    }
}

static de_result parse_schema(const u8 *block, u32 block_size, de_arena *a, sb_table *out) {
    mcur c = {block, block_size};
    u32 count;
    if (!m_u32(&c, &count) || count == 0 || count > block_size / DE__BIN_TYPE_RECORD_FIXED)
        return DE_FAIL(DE_ERR_PARSE, "binary schema: bad type count");
    sb_type *types = (sb_type *)de_arena_push_zero(a, sizeof(sb_type) * count, DE_ALIGNOF(sb_type));
    if (!types)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "binary schema: type table");
    out->t = types;
    out->count = count;

    for (u32 ti = 0; ti < count; ++ti) {
        sb_type *st = &types[ti];
        if (!m_u64(&c, &st->schema_hash) || !m_u64(&c, &st->name_hash) ||
            !m_u32(&c, &st->version) || !m_u32(&c, &st->size) || !m_u32(&c, &st->field_count))
            return schema_corrupt("truncated type record", ti, 0);
        DE_TRY(m_str(&c, a, false, &st->name));
        if (st->size == 0 || st->size > DE__BIN_MAX_TYPE_SIZE)
            return schema_corrupt("bad type size", ti, 0);
        if (st->field_count > c.left / DE__BIN_FIELD_RECORD_FIXED)
            return schema_corrupt("bad field count", ti, 0);
        if (st->field_count > 0) {
            st->f = (sb_field *)de_arena_push_zero(a, sizeof(sb_field) * st->field_count,
                                                   DE_ALIGNOF(sb_field));
            st->info = (de_field_info *)de_arena_push_zero(
                a, sizeof(de_field_info) * st->field_count, DE_ALIGNOF(de_field_info));
            if (!st->f || !st->info)
                return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "binary schema: field table");
        }
        for (u32 fi = 0; fi < st->field_count; ++fi) {
            sb_field *sf = &st->f[fi];
            de_field_info *inf = &st->info[fi];
            u32 size, offset, flags, count_a;
            u8 kind, ekind;
            if (!m_u64(&c, &sf->name_hash) || !m_u64(&c, &sf->nested_hash) || !m_u32(&c, &size) ||
                !m_u32(&c, &offset) || !m_u32(&c, &flags) || !m_u32(&c, &count_a) ||
                !m_u8(&c, &kind) || !m_u8(&c, &ekind))
                return schema_corrupt("truncated field record", ti, fi);
            DE_TRY(m_str(&c, a, false, &inf->name));
            DE_TRY(m_str(&c, a, true, &inf->attrs.nested_type));
            inf->offset = offset;
            inf->size = size;
            inf->kind = (de_field_kind)kind;
            inf->flags = flags;
            inf->attrs.array_count = kind == DE_FK_FIXED_ARRAY ? count_a : 0;
            sf->elem_kind = (de_field_kind)ekind;
        }
    }
    if (c.left != 0)
        return DE_FAIL(DE_ERR_PARSE, "binary schema: %zu trailing bytes", c.left);

    /* Second pass: every reference resolves forward, every field fits its type. */
    for (u32 ti = 0; ti < count; ++ti) {
        sb_type *st = &types[ti];
        for (u32 fi = 0; fi < st->field_count; ++fi) {
            sb_field *sf = &st->f[fi];
            const de_field_info *inf = &st->info[fi];
            if ((u64)inf->offset + inf->size > st->size)
                return schema_corrupt("field outside its type", ti, fi);
            if (inf->kind == DE_FK_FIXED_ARRAY) {
                u32 n = inf->attrs.array_count;
                if (n == 0 || inf->size % n != 0)
                    return schema_corrupt("bad array count", ti, fi);
                sf->elem_size = inf->size / n;
                if (!value_valid(out, ti, sf->elem_kind, sf->elem_size, sf->nested_hash,
                                 &sf->nested))
                    return schema_corrupt("bad array element", ti, fi);
            } else if (!value_valid(out, ti, inf->kind, inf->size, sf->nested_hash, &sf->nested)) {
                return schema_corrupt("bad kind, size or nested type", ti, fi);
            }
        }
    }
    return DE_OK;
}

typedef struct old_state {
    bin_in *in;
    const de_ser_context *ctx;
    const sb_table *tab;
    u64 budget; /* values left to decode (guards against overlapping-field blowups) */
} old_state;

static de_result dec_old_struct(old_state *s, u32 ti, u8 *dst, u32 depth);

static de_result dec_old_value(old_state *s, de_field_kind k, u32 size, u32 nested,
                               const char *asset_type, u8 *dst, u32 depth) {
    if (s->budget == 0)
        return DE_FAIL(DE_ERR_PARSE, "binary object: the stored schema describes too many values");
    s->budget--;
    if (k == DE_FK_STRUCT)
        return dec_old_struct(s, nested, dst, depth + 1);
    return dec_leaf(s->in, s->ctx, k, size, asset_type, dst);
}

static de_result dec_old_struct(old_state *s, u32 ti, u8 *dst, u32 depth) {
    if (depth >= DE__TYPE_MAX_DEPTH)
        return DE_FAIL(DE_ERR_PARSE, "binary object: stored types nest too deeply");
    const sb_type *st = &s->tab->t[ti];
    for (u32 fi = 0; fi < st->field_count; ++fi) {
        const sb_field *sf = &st->f[fi];
        const de_field_info *inf = &st->info[fi];
        u8 *fd = dst + inf->offset;
        if (inf->kind == DE_FK_FIXED_ARRAY) {
            for (u32 e = 0; e < inf->attrs.array_count; ++e)
                DE_TRY(dec_old_value(s, sf->elem_kind, sf->elem_size, sf->nested, NULL,
                                     fd + (size_t)e * sf->elem_size, depth));
        } else {
            DE_TRY(dec_old_value(s, inf->kind, inf->size, sf->nested, inf->attrs.nested_type, fd,
                                 depth));
        }
    }
    return DE_OK;
}

/* Copying the old image into the current object. */
typedef struct conv_state {
    const de_type_registry *r;
    sb_table *tab;
    de_arena *arena;
    u32 req;
    u32 skipped;
    u32 migrations;
    bool no_hook; /* an older stored version met a type without a migrate hook */
    bool newer;   /* a nested stored version is newer than the current type */
    size_t note_len;
    char note[400]; /* "Type.field, ..." of the fields not loaded */
} conv_state;

static void note_skip(conv_state *cs, u32 ti, u32 fi, const char *why) {
    sb_field *sf = &cs->tab->t[ti].f[fi];
    if (sf->noted)
        return;
    sf->noted = true;
    cs->skipped++;
    if (cs->note_len + 4 >= sizeof cs->note)
        return;
    int n =
        snprintf(cs->note + cs->note_len, sizeof cs->note - cs->note_len, "%s%s.%s%s",
                 cs->note_len ? ", " : "", cs->tab->t[ti].name, cs->tab->t[ti].info[fi].name, why);
    if (n < 0 || (size_t)n >= sizeof cs->note - cs->note_len) {
        cs->note_len = sizeof cs->note - 4;
        memcpy(cs->note + cs->note_len, "...", 4);
    } else {
        cs->note_len += (size_t)n;
    }
}

static de_result conv_struct(conv_state *cs, u32 ti, const u8 *old, const de__type_layout *cl,
                             u8 *obj, u32 depth);

/* Copies one value; false when the kinds or sizes do not fit (nothing written then). */
static bool conv_value(conv_state *cs, de_field_kind sk, u32 ssize, u32 snested, const u8 *old,
                       de_field_kind ck, u32 csize, const char *cnested, u8 *dst, u32 depth,
                       de_result *err) {
    if (sk != ck)
        return false;
    switch (ck) {
    case DE_FK_STRING: {
        const u8 *nul = (const u8 *)memchr(old, 0, ssize);
        size_t n = nul ? (size_t)(nul - old) : ssize;
        if (n > (size_t)csize - 1)
            n = (size_t)csize - 1;
        memcpy(dst, old, n);
        memset(dst + n, 0, csize - n);
        return true;
    }
    case DE_FK_STRUCT: {
        const de__type_layout *nl = nested_layout(cs->r, cnested, csize);
        if (!nl || de_hash64_str(nl->desc.name) != cs->tab->t[snested].name_hash)
            return false;
        *err = conv_struct(cs, snested, old, nl, dst, depth + 1);
        return true;
    }
    default:
        if (ssize != csize)
            return false;
        memcpy(dst, old, csize);
        return true;
    }
}

static de_result conv_struct(conv_state *cs, u32 ti, const u8 *old, const de__type_layout *cl,
                             u8 *obj, u32 depth) {
    if (depth >= DE__TYPE_MAX_DEPTH)
        return DE_FAIL(DE_ERR_PARSE, "binary object: stored types nest too deeply");
    const sb_type *st = &cs->tab->t[ti];
    u32 n = cl->desc.field_count;
    u64 *hashes = NULL;
    if (n > 0) {
        hashes = (u64 *)de_arena_push(cs->arena, sizeof(u64) * n, DE_ALIGNOF(u64));
        if (!hashes)
            return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "binary object: field matching");
        for (u32 ci = 0; ci < n; ++ci)
            hashes[ci] = de_hash64_str(cl->fields[ci].name);
    }
    for (u32 fi = 0; fi < st->field_count; ++fi) {
        const sb_field *sf = &st->f[fi];
        const de_field_info *sinf = &st->info[fi];
        u32 ci = 0;
        while (ci < n && hashes[ci] != sf->name_hash)
            ++ci;
        if (ci == n) {
            note_skip(cs, ti, fi, "");
            continue;
        }
        const de_field_info *cf = &cl->fields[ci];
        if (!selected(cf->flags, cs->req))
            continue;
        const u8 *src = old + sinf->offset;
        u8 *dst = obj + cf->offset;
        de_result err = DE_OK;
        bool ok;
        if (sinf->kind == DE_FK_FIXED_ARRAY && cf->kind == DE_FK_FIXED_ARRAY) {
            const de_field_info *el = &cl->extra[ci].element;
            u32 cnt = sinf->attrs.array_count < cf->attrs.array_count ? sinf->attrs.array_count
                                                                      : cf->attrs.array_count;
            ok = true;
            for (u32 e = 0; e < cnt && ok && err == DE_OK; ++e)
                ok = conv_value(cs, sf->elem_kind, sf->elem_size, sf->nested,
                                src + (size_t)e * sf->elem_size, el->kind, el->size,
                                el->attrs.nested_type, dst + (size_t)e * el->size, depth, &err);
        } else {
            ok = sinf->kind != DE_FK_FIXED_ARRAY &&
                 conv_value(cs, sinf->kind, sinf->size, sf->nested, src, cf->kind, cf->size,
                            cf->attrs.nested_type, dst, depth, &err);
        }
        if (err != DE_OK)
            return err;
        if (!ok)
            note_skip(cs, ti, fi, " (kind or size changed)");
    }

    const de_type_desc *cd = &cl->desc;
    if (st->version < cd->version) {
        if (cd->hooks.migrate) {
            cs->migrations++;
            if (!cd->hooks.migrate(obj, st->version, old, st->info, st->field_count))
                return DE_FAIL(DE_ERR_VERSION_MISMATCH,
                               "type '%s': migrate from version %u dropped the object", cd->name,
                               st->version);
        } else {
            cs->no_hook = true;
        }
    } else if (st->version > cd->version) {
        cs->newer = true;
    }
    return DE_OK;
}

/* ---- Deserialize ---------------------------------------------------------------------------- */

typedef struct bin_header {
    u16 format, flags;
    u64 type_hash;
    u32 version;
    u64 schema_hash;
    u32 payload;
} bin_header;

static de_result parse_header(const u8 *h, bin_header *out) {
    if (h[0] != DE__BIN_MAGIC0 || h[1] != DE__BIN_MAGIC1 || h[2] != DE__BIN_MAGIC2 ||
        h[3] != DE__BIN_MAGIC3)
        return DE_FAIL(DE_ERR_PARSE, "binary object: bad magic");
    out->format = get_u16(h + 4);
    out->flags = get_u16(h + 6);
    out->type_hash = get_u64(h + 8);
    out->version = get_u32(h + 16);
    out->schema_hash = get_u64(h + 20);
    out->payload = get_u32(h + 28);
    if (out->format == 0)
        return DE_FAIL(DE_ERR_PARSE, "binary object: format version 0");
    if (out->format > DE__BIN_FORMAT_VERSION)
        return DE_FAIL(DE_ERR_VERSION_MISMATCH, "binary object: format version %u is newer than %u",
                       (unsigned)out->format, DE__BIN_FORMAT_VERSION);
    if (out->flags & ~DE__BIN_FLAGS_KNOWN)
        return DE_FAIL(DE_ERR_PARSE, "binary object: unknown flags 0x%04x", (unsigned)out->flags);
    return DE_OK;
}

/* Skips what follows the header (schema block, payload) after a version mismatch, so a stream of
 * objects can continue, then returns the mismatch error that is already set. */
static de_result skip_rest(de_reader *rd, const bin_header *h, bool block_read, u32 block_size,
                           de_result mismatch) {
    if (h->flags & DE__BIN_FLAG_SELF_DESCRIBING) {
        if (!block_read) {
            u8 b[4];
            DE_TRY(read_exact(rd, b, 4, "schema block size"));
            block_size = get_u32(b);
        }
        DE_TRY(skip_exact(rd, block_size, "schema block"));
    }
    DE_TRY(skip_exact(rd, h->payload, "payload"));
    return mismatch;
}

static de_result read_stored(const de_type_registry *r, const de__type_entry *e, u8 *obj, u32 req,
                             const de_ser_context *ctx, de_ser_binary_stats *stats, de_reader *rd,
                             const bin_header *h, u32 block_size) {
    de_scratch sc = de_scratch_begin(NULL, 0);
    de_result res = DE_OK;
    u8 *block = (u8 *)de_arena_push(sc.arena, block_size ? block_size : 1, 8);
    sb_table tab = {NULL, 0};
    u8 *image = NULL;
    bin_in *in = NULL;
    old_state os;
    conv_state cs;
    memset(&cs, 0, sizeof cs);
    if (!block) {
        res = DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "type '%s': schema block", e->name);
        goto done;
    }
    res = read_exact(rd, block, block_size, "schema block");
    if (res == DE_OK)
        res = parse_schema(block, block_size, sc.arena, &tab);
    if (res != DE_OK)
        goto done;
    if (tab.t[0].schema_hash != h->schema_hash || tab.t[0].name_hash != h->type_hash ||
        tab.t[0].version != h->version) {
        res =
            DE_FAIL(DE_ERR_PARSE, "type '%s': the schema block does not match the header", e->name);
        goto done;
    }

    /* Decode the payload into an image of the stored layout. */
    image = (u8 *)de_arena_push_zero(sc.arena, tab.t[0].size, 16);
    in = (bin_in *)de_arena_push(sc.arena, sizeof *in, DE_ALIGNOF(bin_in));
    if (!image || !in) {
        res = DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "type '%s': old image", e->name);
        goto done;
    }
    in_init(in, rd, h->payload);
    os.in = in;
    os.ctx = ctx;
    os.tab = &tab;
    os.budget = (u64)tab.t[0].size * (DE__TYPE_MAX_DEPTH + 1u) + 1024u;
    res = dec_old_struct(&os, 0, image, 0);
    if (res == DE_OK && in->left != 0)
        res = DE_FAIL(DE_ERR_PARSE, "type '%s': %llu trailing payload bytes", e->name,
                      (unsigned long long)in->left);
    if (res != DE_OK)
        goto done;

    /* Copy what still fits into the object, then migrate. */
    cs.r = r;
    cs.tab = &tab;
    cs.arena = sc.arena;
    cs.req = req;
    res = conv_struct(&cs, 0, image, e->cur, obj, 0);
    if (stats) {
        stats->migrations += cs.migrations;
        stats->skipped_fields += cs.skipped;
    }
    if (res != DE_OK)
        goto done;
    if (cs.skipped > 0 || cs.no_hook || cs.newer) {
        DE_WARN(log_cat(),
                "type '%s': loaded from stored version %u (schema 0x%016llx; current version "
                "%u)%s%s%s%s",
                e->name, h->version, (unsigned long long)h->schema_hash, e->cur->desc.version,
                cs.skipped ? "; not loaded: " : "", cs.skipped ? cs.note : "",
                cs.no_hook ? "; no migrate hook for an older version, matched fields by name" : "",
                cs.newer ? "; a nested type was stored by a newer version" : "");
        if (stats)
            stats->warnings++;
    }
    if (stats) {
        stats->objects_read++;
        stats->schema_matched++;
    }
done:
    de_scratch_end(sc);
    return res;
}

de_result de_deserialize_binary(const de_type_registry *r, de_type_id type, void *obj,
                                const de_ser_options *opt, const de_ser_binary_options *bopt,
                                de_reader *rd) {
    const de__type_entry *e = struct_entry(r, type);
    if (!e || !obj || !rd || !rd->read)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_deserialize_binary: NULL argument or not a struct type id");
    u32 req = opt ? opt->required_flags : 0u;
    const de_ser_context *ctx = opt ? opt->ctx : NULL;
    de_ser_binary_stats *stats = bopt ? bopt->stats : NULL;
    const de_type_desc *d = &e->cur->desc;

    u8 hb[DE__BIN_HEADER_SIZE];
    DE_TRY(read_exact(rd, hb, sizeof hb, "header"));
    bin_header h;
    DE_TRY(parse_header(hb, &h));
    if (h.type_hash != e->name_hash)
        return DE_FAIL(DE_ERR_PARSE, "binary object: not an object of type '%s'", e->name);
    if (h.version > d->version)
        return skip_rest(rd, &h, false, 0,
                         DE_FAIL(DE_ERR_VERSION_MISMATCH,
                                 "type '%s': stored version %u is newer than %u", e->name,
                                 h.version, d->version));
    bool self = (h.flags & DE__BIN_FLAG_SELF_DESCRIBING) != 0;
    u32 stored_req = (u32)h.flags >> DE__BIN_FLAGS_REQ_SHIFT;
    u32 block_size = 0;
    if (self) {
        u8 b[4];
        DE_TRY(read_exact(rd, b, 4, "schema block size"));
        block_size = get_u32(b);
        if (block_size > DE__BIN_MAX_SCHEMA_BYTES)
            return DE_FAIL(DE_ERR_PARSE, "type '%s': schema block of %u bytes", e->name,
                           block_size);
    }

    if (h.schema_hash != e->hash) {
        if (!self)
            return skip_rest(rd, &h, true, block_size,
                             DE_FAIL(DE_ERR_VERSION_MISMATCH,
                                     "type '%s': stored schema 0x%016llx differs from 0x%016llx "
                                     "and the object has no schema block",
                                     e->name, (unsigned long long)h.schema_hash,
                                     (unsigned long long)e->hash));
        return read_stored(r, e, (u8 *)obj, req, ctx, stats, rd, &h, block_size);
    }

    /* Path 1: same schema. The schema block is not needed. */
    if (self)
        DE_TRY(skip_exact(rd, block_size, "schema block"));
    u32 sum = e->bin_summary;
    bool fast = !(bopt && bopt->disable_fast_path) && d->is_pod && (sum & DE__BIN_SUM_PACKED) &&
                all_selected(sum, stored_req) && all_selected(sum, req) &&
                !((sum & DE__BIN_SUM_HAS_ENTITY) && ctx && ctx->scene_id_to_entity);
    if (fast) {
        if (h.payload != d->size)
            return DE_FAIL(DE_ERR_PARSE, "type '%s': payload of %u bytes, expected %u", e->name,
                           h.payload, d->size);
        DE_TRY(read_exact(rd, obj, d->size, "payload"));
    } else {
        bin_in in;
        in_init(&in, rd, h.payload);
        dec_state s;
        s.r = r;
        s.ctx = ctx;
        s.type_name = e->name;
        s.req = req;
        s.stored_req = stored_req;
        s.in = &in;
        DE_TRY(dec_struct(&s, e->cur, (u8 *)obj, 0));
        if (in.left != 0)
            return DE_FAIL(DE_ERR_PARSE, "type '%s': %llu trailing payload bytes", e->name,
                           (unsigned long long)in.left);
    }
    if (stats) {
        stats->objects_read++;
        if (fast)
            stats->fast_path++;
        else
            stats->field_wise++;
    }
    return DE_OK;
}

/* ---- Memory writer and reader --------------------------------------------------------------- */

static de_result mem_write(void *user, const void *data, size_t bytes) {
    de_mem_writer *mw = (de_mem_writer *)user;
    if (bytes == 0)
        return DE_OK;
    if (bytes > (size_t)-1 - mw->size)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_mem_writer: size overflow");
    size_t need = mw->size + bytes;
    if (need > mw->capacity) {
        if (!mw->a)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                           "de_mem_writer: fixed buffer of %zu bytes cannot take %zu more",
                           mw->capacity, bytes);
        size_t cap = mw->capacity ? mw->capacity : 256;
        while (cap < need)
            cap = cap > (size_t)-1 / 2 ? need : cap * 2;
        u8 *p = (u8 *)DE_REALLOC(mw->a, mw->data, mw->capacity, cap, 16, DE_MEM_GENERAL);
        if (!p)
            return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_mem_writer: growing to %zu bytes", cap);
        mw->data = p;
        mw->capacity = cap;
    }
    memcpy(mw->data + mw->size, data, bytes);
    mw->size = need;
    return DE_OK;
}

void de_mem_writer_init(de_mem_writer *mw, const de_allocator *a) {
    if (!mw)
        return;
    memset(mw, 0, sizeof *mw);
    mw->a = a ? a : de_heap_allocator();
}

void de_mem_writer_init_fixed(de_mem_writer *mw, void *buf, size_t capacity) {
    if (!mw)
        return;
    memset(mw, 0, sizeof *mw);
    mw->data = (u8 *)buf;
    mw->capacity = buf ? capacity : 0;
}

void de_mem_writer_reset(de_mem_writer *mw) {
    if (mw)
        mw->size = 0;
}

void de_mem_writer_free(de_mem_writer *mw) {
    if (!mw)
        return;
    if (mw->a && mw->data)
        DE_FREE(mw->a, mw->data, mw->capacity);
    memset(mw, 0, sizeof *mw);
}

de_writer de_writer_from_mem(de_mem_writer *mw) {
    de_writer w;
    w.write = mem_write;
    w.user = mw;
    return w;
}

static de_result mem_read(void *user, void *data, size_t bytes, size_t *read) {
    de_mem_reader *mr = (de_mem_reader *)user;
    size_t avail = mr->pos < mr->size ? mr->size - mr->pos : 0;
    size_t n = bytes < avail ? bytes : avail;
    if (n)
        memcpy(data, mr->data + mr->pos, n);
    mr->pos += n;
    if (read)
        *read = n;
    return DE_OK;
}

void de_mem_reader_init(de_mem_reader *mr, const void *data, size_t size) {
    if (!mr)
        return;
    mr->data = (const u8 *)data;
    mr->size = data ? size : 0;
    mr->pos = 0;
}

de_reader de_reader_from_mem(de_mem_reader *mr) {
    de_reader rd;
    rd.read = mem_read;
    rd.user = mr;
    return rd;
}
