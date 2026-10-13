/*
 * dpak_writer.c: .dpak pack writer (SPEC §11.7, DENG-211). See dpak_writer.h for the contract
 * and src/asset/dpak/dpak_format.h / docs/formats/dpak.md for the bytes.
 *
 * finish() works in five steps, all on the calling thread:
 *   1. sort the entries by ID and the bundles by name hash; reject duplicates;
 *   2. cut every entry into chunks of chunk_size raw bytes (in TOC order);
 *   3. compress each chunk on its own without a dictionary; a chunk that does not shrink is
 *      stored raw;
 *   4. with use_dictionaries, train one dictionary per type (in type-hash order) from that type's
 *      chunks, recompress them with it, and keep it only if it pays for itself;
 *   5. lay out header, sections, dictionaries and data, encode everything little-endian into one
 *      zeroed block (in format version 2, with the XXH3-64 of every chunk's stored bytes and of
 *      every dictionary in their table entries), and hash it.
 * Nothing depends on the add order (step 1), on addresses or on time, and zstd runs
 * single-threaded with fixed parameters, so the same inputs give the same bytes.
 */
#include "dpak_writer.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include <zdict.h>
#include <zstd.h>

#include <dengine/de_platform.h>

#include "asset/dpak/dpak_format.h"
#include "core/de_array.h"
#include "core/de_sort.h"

/* Training reads at most this many sample bytes per type (the first chunks in TOC order). */
#define DPAK__DICT_SAMPLE_LIMIT (64u * 1024u * 1024u)
/* The dictionary capacity is a tenth of the sample bytes, and at least this much, or the type
 * gets no dictionary: smaller training sets do not give zstd enough to work with. */
#define DPAK__DICT_MIN_CAPACITY 1024u
#define DPAK__DICT_SAMPLE_RATIO 10u

/* True if a u64 byte count fits in size_t. Compiled out where size_t is 64-bit, so GCC's
 * -Wtype-limits never sees an always-false comparison. */
#if SIZE_MAX < UINT64_MAX
#define DPAK__FITS_SIZE(v) ((v) <= (u64)SIZE_MAX)
#else
#define DPAK__FITS_SIZE(v) ((void)(v), true)
#endif

typedef struct dpak__entry {
    u64 id;
    u64 type_hash;
    u8 *data; /* owned copy of `size` bytes; NULL when size is 0 */
    u64 size;
    u32 dep_first; /* into dpak_writer.deps */
    u32 dep_count;
    u32 name_offset; /* into dpak_writer.strings; valid when has_name */
    u32 name_len;
    u32 flags; /* dpak_entry_flags known at add time (SUB_ASSET) */
    u32 has_name;
} dpak__entry;

typedef struct dpak__bundle {
    u64 name_hash;
    u32 name_offset; /* into dpak_writer.strings */
    u32 name_len;
    u32 member_first; /* into dpak_writer.ids */
    u32 member_count;
    u32 depend_first; /* into dpak_writer.ids */
    u32 depend_count;
    u32 flags;
    u32 priority;
} dpak__bundle;

struct dpak_writer {
    const de_allocator *a;
    u32 version; /* format version written: DPAK_VERSION, or DPAK_VERSION_MIN for compat tests */
    u32 platform;
    u8 build_id[16];
    u32 chunk_size;
    i32 level;
    bool use_dicts;
    bool include_names;
    u32 alignment;
    u64 chunk_total; /* chunks of all entries so far */
    DE_ARRAY(dpak__entry) entries;
    DE_ARRAY(dpak__bundle) bundles;
    DE_ARRAY(u64) deps;     /* entry dependencies, per entry sorted and unique, in add order */
    DE_ARRAY(u64) ids;      /* bundle members and depends, per bundle sorted and unique */
    DE_ARRAY(char) strings; /* entry and bundle names, each followed by a NUL, in add order */
};

/* ---- Small helpers --------------------------------------------------------------------- */

static bool dpak__is_pow2(u32 v) {
    return v != 0u && (v & (v - 1u)) == 0u;
}

static u64 dpak__align_up(u64 v, u64 align) {
    return (v + align - 1u) & ~(align - 1u);
}

static int dpak__cmp_u64(const void *a, const void *b, void *user) {
    (void)user;
    const u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Sorts `n` u64 in place and removes duplicates; returns the new count. */
static de_result dpak__sort_unique(const de_allocator *a, u64 *v, u32 n, u32 *out_n) {
    DE_TRY(de_sort_stable(v, n, sizeof(u64), dpak__cmp_u64, NULL, a));
    u32 m = 0;
    for (u32 i = 0; i < n; ++i)
        if (m == 0 || v[m - 1] != v[i])
            v[m++] = v[i];
    *out_n = m;
    return DE_OK;
}

/* Appends `s` and a NUL to w->strings; returns its offset and length. */
static de_result dpak__push_string(dpak_writer *w, const char *s, u32 *offset, u32 *len) {
    const size_t n = strlen(s);
    if ((u64)w->strings.count + n + 1u > (u64)UINT32_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak: string section exceeds 4 GiB");
    const u32 at = w->strings.count;
    DE_TRY(de_array_push_n(&w->strings, s, (u32)n));
    de_result r = de_array_push(&w->strings, NULL); /* NUL */
    if (r != DE_OK) {
        (void)de_array_resize(&w->strings, at);
        return r;
    }
    *offset = at;
    *len = (u32)n;
    return DE_OK;
}

static const char *dpak__entry_name(const dpak_writer *w, const dpak__entry *e) {
    return e->has_name ? (const char *)w->strings.data + e->name_offset : "(unnamed)";
}

/* ---- Create / destroy ------------------------------------------------------------------- */

de_result dpak_writer_create(const dpak_writer_desc *desc, dpak_writer **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_writer_create: NULL out");
    *out = NULL;
    const dpak_writer_desc d = desc ? *desc : (dpak_writer_desc){0};
    const u32 chunk_size = d.chunk_size ? d.chunk_size : DPAK_MAX_CHUNK_SIZE;
    const u32 alignment = d.alignment ? d.alignment : DPAK_DEFAULT_ALIGNMENT;
    const i32 level = d.zstd_level ? d.zstd_level : DPAK_WRITER_DEFAULT_ZSTD_LEVEL;
    if (chunk_size < DPAK_MIN_CHUNK_SIZE || chunk_size > DPAK_MAX_CHUNK_SIZE)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak: chunk_size %u not in [%u, %u]", chunk_size,
                       DPAK_MIN_CHUNK_SIZE, DPAK_MAX_CHUNK_SIZE);
    if (!dpak__is_pow2(alignment) || alignment < DPAK_MIN_ALIGNMENT ||
        alignment > DPAK_MAX_ALIGNMENT)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "dpak: alignment %u is not a power of two in [%u, %u]", alignment,
                       DPAK_MIN_ALIGNMENT, DPAK_MAX_ALIGNMENT);
    if (level < ZSTD_minCLevel() || level > ZSTD_maxCLevel())
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak: zstd level %d not in [%d, %d]", (int)level,
                       ZSTD_minCLevel(), ZSTD_maxCLevel());
    if (d.platform >= (u32)DPAK_PLATFORM_COUNT_)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak: unknown platform %u", d.platform);
    const u32 version = d.format_version ? d.format_version : DPAK_VERSION;
    if (version < DPAK_VERSION_MIN || version > DPAK_VERSION)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak: format version %u not in [%u, %u]", version,
                       DPAK_VERSION_MIN, DPAK_VERSION);

    const de_allocator *a = d.allocator ? d.allocator : de_heap_allocator();
    dpak_writer *w = DE_NEW(a, dpak_writer, DE_MEM_ASSET);
    if (w == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "dpak: writer allocation failed");
    memset(w, 0, sizeof *w);
    w->a = a;
    w->version = version;
    w->platform = d.platform;
    memcpy(w->build_id, d.build_id, sizeof w->build_id);
    w->chunk_size = chunk_size;
    w->level = level;
    w->use_dicts = d.use_dictionaries;
    w->include_names = d.include_names;
    w->alignment = alignment;
    DE_ARRAY_INIT_TAG(&w->entries, dpak__entry, a, DE_MEM_ASSET);
    DE_ARRAY_INIT_TAG(&w->bundles, dpak__bundle, a, DE_MEM_ASSET);
    DE_ARRAY_INIT_TAG(&w->deps, u64, a, DE_MEM_ASSET);
    DE_ARRAY_INIT_TAG(&w->ids, u64, a, DE_MEM_ASSET);
    DE_ARRAY_INIT_TAG(&w->strings, char, a, DE_MEM_ASSET);
    *out = w;
    return DE_OK;
}

void dpak_writer_destroy(dpak_writer *w) {
    if (w == NULL)
        return;
    for (u32 i = 0; i < w->entries.count; ++i) {
        dpak__entry *e = &DE_ARRAY_AT(&w->entries, dpak__entry, i);
        if (e->data != NULL)
            DE_FREE(w->a, e->data, (size_t)e->size);
    }
    de_array_free(&w->entries);
    de_array_free(&w->bundles);
    de_array_free(&w->deps);
    de_array_free(&w->ids);
    de_array_free(&w->strings);
    DE_FREE(w->a, w, sizeof *w);
}

/* ---- Add -------------------------------------------------------------------------------- */

de_result dpak_writer_add(dpak_writer *w, u64 id, const char *type_name, const void *data, u64 size,
                          const u64 *deps, u32 dep_count, const char *name) {
    if (w == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_writer_add: NULL writer");
    if (type_name == NULL || type_name[0] == '\0')
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_writer_add: asset 0x%016llx has no type",
                       (unsigned long long)id);
    if (data == NULL && size != 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_writer_add: NULL data, size %llu",
                       (unsigned long long)size);
    if (deps == NULL && dep_count != 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_writer_add: NULL deps, count %u", dep_count);
    if (!DPAK__FITS_SIZE(size))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_writer_add: size does not fit in memory");
    const u64 chunks = (size + w->chunk_size - 1u) / w->chunk_size;
    if (w->chunk_total + chunks > (u64)UINT32_MAX || w->entries.count == UINT32_MAX ||
        (u64)w->deps.count + dep_count > (u64)UINT32_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_writer_add: pack exceeds a format limit");
    for (u32 i = 0; i < dep_count; ++i)
        if (deps[i] == id)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "dpak_writer_add: asset 0x%016llx ('%s') depends on itself",
                           (unsigned long long)id, name ? name : "(unnamed)");

    /* Roll-back points: on any failure below the writer is left as it was. */
    const u32 deps_at = w->deps.count;
    const u32 strings_at = w->strings.count;
    dpak__entry e = {
        .id = id,
        .type_hash = de_hash64_str(type_name),
        .size = size,
        .dep_first = deps_at,
    };
    de_result r = DE_OK;
    if (dep_count > 0) {
        r = de_array_push_n(&w->deps, deps, dep_count);
        if (r != DE_OK)
            goto fail;
        r = dpak__sort_unique(w->a, DE_ARRAY_DATA(&w->deps, u64) + deps_at, dep_count,
                              &e.dep_count);
        if (r != DE_OK)
            goto fail;
        (void)de_array_resize(&w->deps, deps_at + e.dep_count); /* shrinks: cannot fail */
    }
    if (name != NULL) {
        r = dpak__push_string(w, name, &e.name_offset, &e.name_len);
        if (r != DE_OK)
            goto fail;
        e.has_name = 1;
        if (strchr(name, '#') != NULL)
            e.flags |= DPAK_ENTRY_SUB_ASSET;
    }
    if (size > 0) {
        e.data = DE_ALLOC(w->a, (size_t)size, 16, DE_MEM_ASSET);
        if (e.data == NULL) {
            r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "dpak_writer_add: %llu bytes for '%s'",
                        (unsigned long long)size, name ? name : "(unnamed)");
            goto fail;
        }
        memcpy(e.data, data, (size_t)size);
    }
    r = de_array_push(&w->entries, &e);
    if (r != DE_OK) {
        if (e.data != NULL)
            DE_FREE(w->a, e.data, (size_t)size);
        goto fail;
    }
    w->chunk_total += chunks;
    return DE_OK;

fail:
    (void)de_array_resize(&w->deps, deps_at);
    (void)de_array_resize(&w->strings, strings_at);
    return r;
}

de_result dpak_writer_add_bundle(dpak_writer *w, const char *name, const u64 *ids, u32 count,
                                 const char *const *depends, u32 depend_count, bool resident,
                                 u32 priority) {
    if (w == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_writer_add_bundle: NULL writer");
    if (name == NULL || name[0] == '\0')
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_writer_add_bundle: bundle has no name");
    if ((ids == NULL && count != 0) || (depends == NULL && depend_count != 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_writer_add_bundle: '%s': NULL list", name);
    if (priority >= (u32)DPAK_PRIORITY_COUNT_)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_writer_add_bundle: '%s': priority %u", name,
                       priority);
    if ((u64)w->ids.count + count + depend_count > (u64)UINT32_MAX ||
        w->bundles.count == UINT32_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_writer_add_bundle: format limit");
    const u64 name_hash = de_hash64_str(name);
    for (u32 i = 0; i < depend_count; ++i) {
        if (depends[i] == NULL || depends[i][0] == '\0')
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "dpak_writer_add_bundle: '%s': empty depend name", name);
        if (de_hash64_str(depends[i]) == name_hash)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "dpak_writer_add_bundle: '%s' depends on itself", name);
    }

    const u32 ids_at = w->ids.count;
    const u32 strings_at = w->strings.count;
    dpak__bundle b = {
        .name_hash = name_hash,
        .flags = resident ? DPAK_BUNDLE_RESIDENT : 0u,
        .priority = priority,
    };
    de_result r = dpak__push_string(w, name, &b.name_offset, &b.name_len);
    if (r != DE_OK)
        goto fail;
    if (count > 0) {
        r = de_array_push_n(&w->ids, ids, count);
        if (r != DE_OK)
            goto fail;
        b.member_first = ids_at;
        r = dpak__sort_unique(w->a, DE_ARRAY_DATA(&w->ids, u64) + ids_at, count, &b.member_count);
        if (r != DE_OK)
            goto fail;
        (void)de_array_resize(&w->ids, ids_at + b.member_count);
    }
    b.depend_first = w->ids.count;
    if (depend_count > 0) {
        const u32 dep_at = w->ids.count;
        r = de_array_resize(&w->ids, dep_at + depend_count);
        if (r != DE_OK)
            goto fail;
        u64 *h = DE_ARRAY_DATA(&w->ids, u64) + dep_at;
        for (u32 i = 0; i < depend_count; ++i)
            h[i] = de_hash64_str(depends[i]);
        r = dpak__sort_unique(w->a, h, depend_count, &b.depend_count);
        if (r != DE_OK)
            goto fail;
        (void)de_array_resize(&w->ids, dep_at + b.depend_count);
    }
    r = de_array_push(&w->bundles, &b);
    if (r != DE_OK)
        goto fail;
    return DE_OK;

fail:
    (void)de_array_resize(&w->ids, ids_at);
    (void)de_array_resize(&w->strings, strings_at);
    return r;
}

/* ---- Finish: state ---------------------------------------------------------------------- */

typedef struct dpak__chunk {
    const u8 *raw;
    u8 *stored; /* == raw when DPAK_CHUNK_RAW, else an owned block of stored_size bytes */
    u32 raw_size;
    u32 stored_size;
    u32 type_slot; /* index into dpak__build.types */
    u16 dict;
    u16 flags;
} dpak__chunk;

typedef struct dpak__dict {
    u64 type_hash;
    u8 *data; /* owned, `cap` bytes allocated */
    u32 size;
    u32 cap;
    u32 zstd_id;
    u64 file_offset; /* set by the layout */
} dpak__dict;

typedef struct dpak__sort_key {
    u64 key;
    u32 index;
    u32 pad;
} dpak__sort_key;

typedef struct dpak__build {
    dpak_writer *w;
    u32 *order;        /* entry indices sorted by id */
    u32 *bundle_order; /* bundle indices sorted by name hash */
    u32 *entry_chunk;  /* first chunk of each sorted entry */
    dpak__chunk *chunks;
    u32 chunk_count;
    u64 *types; /* distinct type hashes, ascending */
    u32 type_count;
    dpak__dict *dicts;
    u32 dict_count;
    ZSTD_CCtx *cctx;
    u8 *scratch; /* ZSTD_compressBound(chunk_size) bytes */
    size_t scratch_cap;
} dpak__build;

static int dpak__cmp_key(const void *a, const void *b, void *user) {
    (void)user;
    const dpak__sort_key *x = a, *y = b;
    return x->key < y->key ? -1 : (x->key > y->key ? 1 : 0);
}

static void *dpak__alloc_zero(const de_allocator *a, size_t size) {
    if (size == 0)
        size = 1;
    void *p = DE_ALLOC(a, size, 16, DE_MEM_ASSET);
    if (p != NULL)
        memset(p, 0, size);
    return p;
}

static void dpak__build_free(dpak__build *b) {
    const de_allocator *a = b->w->a;
    const u32 n = b->w->entries.count;
    if (b->chunks != NULL) {
        for (u32 i = 0; i < b->chunk_count; ++i)
            if (!(b->chunks[i].flags & DPAK_CHUNK_RAW) && b->chunks[i].stored != NULL)
                DE_FREE(a, b->chunks[i].stored, b->chunks[i].stored_size);
        DE_FREE(a, b->chunks, (size_t)(b->chunk_count ? b->chunk_count : 1u) * sizeof *b->chunks);
    }
    if (b->dicts != NULL) {
        for (u32 i = 0; i < b->dict_count; ++i)
            DE_FREE(a, b->dicts[i].data, b->dicts[i].cap);
        DE_FREE(a, b->dicts, (size_t)(b->type_count ? b->type_count : 1u) * sizeof *b->dicts);
    }
    if (b->order != NULL)
        DE_FREE(a, b->order, (size_t)(n ? n : 1u) * sizeof *b->order);
    if (b->entry_chunk != NULL)
        DE_FREE(a, b->entry_chunk, (size_t)(n ? n : 1u) * sizeof *b->entry_chunk);
    if (b->bundle_order != NULL)
        DE_FREE(a, b->bundle_order,
                (size_t)(b->w->bundles.count ? b->w->bundles.count : 1u) * sizeof(u32));
    if (b->types != NULL)
        DE_FREE(a, b->types, (size_t)(n ? n : 1u) * sizeof *b->types);
    if (b->scratch != NULL)
        DE_FREE(a, b->scratch, b->scratch_cap);
    ZSTD_freeCCtx(b->cctx);
}

/* ---- Finish: step 1, order ------------------------------------------------------------- */

/* Writes into `out` the indices 0..n-1 sorted by key(i) (stable). */
static de_result dpak__sorted_indices(const de_allocator *a, u32 n, const void *base, size_t stride,
                                      u32 *out) {
    if (n == 0)
        return DE_OK;
    dpak__sort_key *keys = DE_ALLOC(a, (size_t)n * sizeof *keys, 8, DE_MEM_ASSET);
    if (keys == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "dpak: sort keys");
    for (u32 i = 0; i < n; ++i) {
        /* Both record types start with their u64 sort key. */
        u64 k;
        memcpy(&k, (const u8 *)base + (size_t)i * stride, sizeof k);
        keys[i] = (dpak__sort_key){.key = k, .index = i};
    }
    de_result r = de_sort_stable(keys, n, sizeof *keys, dpak__cmp_key, NULL, a);
    if (r == DE_OK)
        for (u32 i = 0; i < n; ++i)
            out[i] = keys[i].index;
    DE_FREE(a, keys, (size_t)n * sizeof *keys);
    return r;
}

static de_result dpak__order(dpak__build *b) {
    dpak_writer *w = b->w;
    const de_allocator *a = w->a;
    const u32 n = w->entries.count;
    const u32 nb = w->bundles.count;
    b->order = dpak__alloc_zero(a, (size_t)(n ? n : 1u) * sizeof(u32));
    b->entry_chunk = dpak__alloc_zero(a, (size_t)(n ? n : 1u) * sizeof(u32));
    b->bundle_order = dpak__alloc_zero(a, (size_t)(nb ? nb : 1u) * sizeof(u32));
    b->types = dpak__alloc_zero(a, (size_t)(n ? n : 1u) * sizeof(u64));
    if (!b->order || !b->entry_chunk || !b->bundle_order || !b->types)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "dpak: finish tables");

    DE_STATIC_ASSERT(offsetof(dpak__entry, id) == 0, "entry sort key first");
    DE_STATIC_ASSERT(offsetof(dpak__bundle, name_hash) == 0, "bundle sort key first");
    DE_TRY(dpak__sorted_indices(a, n, w->entries.data, sizeof(dpak__entry), b->order));
    DE_TRY(dpak__sorted_indices(a, nb, w->bundles.data, sizeof(dpak__bundle), b->bundle_order));

    const dpak__entry *ents = DE_ARRAY_DATA(&w->entries, dpak__entry);
    for (u32 i = 1; i < n; ++i) {
        const dpak__entry *p = &ents[b->order[i - 1]], *e = &ents[b->order[i]];
        if (p->id == e->id)
            return DE_FAIL(
                DE_ERR_ALREADY_EXISTS, "dpak: duplicate asset id 0x%016llx: '%s' and '%s'",
                (unsigned long long)e->id, dpak__entry_name(w, p), dpak__entry_name(w, e));
    }
    const dpak__bundle *bun = DE_ARRAY_DATA(&w->bundles, dpak__bundle);
    const char *strs = DE_ARRAY_DATA(&w->strings, char);
    for (u32 i = 1; i < nb; ++i) {
        const dpak__bundle *p = &bun[b->bundle_order[i - 1]], *e = &bun[b->bundle_order[i]];
        if (p->name_hash == e->name_hash)
            return DE_FAIL(DE_ERR_ALREADY_EXISTS, "dpak: bundles '%s' and '%s' have one name hash",
                           strs + p->name_offset, strs + e->name_offset);
    }

    /* Distinct type hashes, ascending. */
    for (u32 i = 0; i < n; ++i)
        b->types[i] = ents[i].type_hash;
    return dpak__sort_unique(a, b->types, n, &b->type_count);
}

static u32 dpak__type_slot(const dpak__build *b, u64 type_hash) {
    u32 lo = 0, hi = b->type_count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (b->types[mid] < type_hash)
            lo = mid + 1u;
        else
            hi = mid;
    }
    DE_ASSERT(lo < b->type_count && b->types[lo] == type_hash);
    return lo;
}

/* ---- Finish: steps 2-4, chunks and compression ---------------------------------------- */

/* Compresses `n` bytes into b->scratch with the fixed frame parameters: content size on, no
 * checksum (the pack has its own content hash), no dictionary ID (the chunk table names the
 * dictionary). */
static de_result dpak__compress(dpak__build *b, const ZSTD_CDict *cdict, const u8 *src, u32 n,
                                size_t *out) {
    ZSTD_CCtx *cc = b->cctx;
    size_t z = ZSTD_CCtx_reset(cc, ZSTD_reset_session_and_parameters);
    if (!ZSTD_isError(z))
        z = cdict ? ZSTD_CCtx_refCDict(cc, cdict)
                  : ZSTD_CCtx_setParameter(cc, ZSTD_c_compressionLevel, (int)b->w->level);
    if (!ZSTD_isError(z))
        z = ZSTD_CCtx_setParameter(cc, ZSTD_c_contentSizeFlag, 1);
    if (!ZSTD_isError(z))
        z = ZSTD_CCtx_setParameter(cc, ZSTD_c_checksumFlag, 0);
    if (!ZSTD_isError(z))
        z = ZSTD_CCtx_setParameter(cc, ZSTD_c_dictIDFlag, 0);
    if (!ZSTD_isError(z))
        z = ZSTD_compress2(cc, b->scratch, b->scratch_cap, src, n);
    if (ZSTD_isError(z))
        return DE_FAIL(DE_ERR_INTERNAL, "dpak: zstd: %s", ZSTD_getErrorName(z));
    *out = z;
    return DE_OK;
}

static u8 *dpak__copy_scratch(dpak__build *b, size_t n) {
    u8 *p = DE_ALLOC(b->w->a, n, 8, DE_MEM_ASSET);
    if (p != NULL)
        memcpy(p, b->scratch, n);
    return p;
}

static de_result dpak__make_chunks(dpak__build *b) {
    dpak_writer *w = b->w;
    const u32 n = w->entries.count;
    b->chunk_count = (u32)w->chunk_total;
    b->chunks =
        dpak__alloc_zero(w->a, (size_t)(b->chunk_count ? b->chunk_count : 1u) * sizeof *b->chunks);
    if (b->chunks == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "dpak: chunk table");
    u32 c = 0;
    for (u32 i = 0; i < n; ++i) {
        const dpak__entry *e = &DE_ARRAY_AT(&w->entries, dpak__entry, b->order[i]);
        const u32 slot = dpak__type_slot(b, e->type_hash);
        b->entry_chunk[i] = c;
        for (u64 off = 0; off < e->size; off += w->chunk_size) {
            const u64 left = e->size - off;
            dpak__chunk *ch = &b->chunks[c++];
            ch->raw = e->data + off;
            ch->raw_size = (u32)(left < w->chunk_size ? left : w->chunk_size);
            ch->type_slot = slot;
            ch->dict = (u16)DPAK_DICT_NONE;
        }
    }
    DE_ASSERT(c == b->chunk_count);

    for (u32 i = 0; i < b->chunk_count; ++i) {
        dpak__chunk *ch = &b->chunks[i];
        size_t z;
        DE_TRY(dpak__compress(b, NULL, ch->raw, ch->raw_size, &z));
        if (z < ch->raw_size) {
            ch->stored = dpak__copy_scratch(b, z);
            if (ch->stored == NULL)
                return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "dpak: compressed chunk");
            ch->stored_size = (u32)z;
        } else {
            /* Did not shrink: store raw. */
            ch->stored = (u8 *)(uintptr_t)ch->raw;
            ch->stored_size = ch->raw_size;
            ch->flags = DPAK_CHUNK_RAW;
        }
    }
    return DE_OK;
}

/* Trains a dictionary for type `slot` and switches its chunks to it when it pays off. */
static de_result dpak__try_dict(dpak__build *b, u32 slot) {
    const de_allocator *a = b->w->a;
    u32 nsamples = 0;
    u64 total = 0;
    for (u32 i = 0; i < b->chunk_count; ++i) {
        const dpak__chunk *ch = &b->chunks[i];
        if (ch->type_slot != slot)
            continue;
        if (total + ch->raw_size > DPAK__DICT_SAMPLE_LIMIT)
            break;
        total += ch->raw_size;
        ++nsamples;
    }
    if (nsamples < DPAK_DICT_MIN_SAMPLES)
        return DE_OK;
    u64 cap64 = total / DPAK__DICT_SAMPLE_RATIO;
    if (cap64 > DPAK_DICT_MAX_SIZE)
        cap64 = DPAK_DICT_MAX_SIZE;
    if (cap64 < DPAK__DICT_MIN_CAPACITY)
        return DE_OK;
    const u32 cap = (u32)cap64;

    de_result r = DE_OK;
    u8 *samples = DE_ALLOC(a, (size_t)total, 16, DE_MEM_ASSET);
    size_t *sizes = DE_ALLOC(a, (size_t)nsamples * sizeof(size_t), 8, DE_MEM_ASSET);
    u8 *dict = DE_ALLOC(a, cap, 16, DE_MEM_ASSET);
    u8 **cand = dpak__alloc_zero(a, (size_t)b->chunk_count * sizeof(u8 *));
    u32 *cand_size = dpak__alloc_zero(a, (size_t)b->chunk_count * sizeof(u32));
    ZSTD_CDict *cdict = NULL;
    bool keep = false;
    if (!samples || !sizes || !dict || !cand || !cand_size) {
        r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "dpak: dictionary training buffers");
        goto done;
    }
    {
        u64 at = 0;
        u32 k = 0;
        for (u32 i = 0; i < b->chunk_count && k < nsamples; ++i) {
            const dpak__chunk *ch = &b->chunks[i];
            if (ch->type_slot != slot)
                continue;
            memcpy(samples + at, ch->raw, ch->raw_size);
            at += ch->raw_size;
            sizes[k++] = ch->raw_size;
        }
    }
    /* Training failures (too little or too uniform data) just mean no dictionary. */
    const size_t dict_size = ZDICT_trainFromBuffer(dict, cap, samples, sizes, nsamples);
    if (ZDICT_isError(dict_size) || dict_size == 0)
        goto done;
    cdict = ZSTD_createCDict(dict, dict_size, (int)b->w->level);
    if (cdict == NULL) {
        r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "dpak: zstd dictionary");
        goto done;
    }

    /* Each chunk of the type takes the dictionary only if that makes it smaller. */
    u64 gain = 0;
    for (u32 i = 0; i < b->chunk_count; ++i) {
        dpak__chunk *ch = &b->chunks[i];
        if (ch->type_slot != slot)
            continue;
        size_t z;
        r = dpak__compress(b, cdict, ch->raw, ch->raw_size, &z);
        if (r != DE_OK)
            goto done;
        if (z < ch->stored_size) {
            cand[i] = dpak__copy_scratch(b, z);
            if (cand[i] == NULL) {
                r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "dpak: compressed chunk");
                goto done;
            }
            cand_size[i] = (u32)z;
            gain += ch->stored_size - z;
        }
    }
    keep = gain > dict_size;
    if (keep) {
        const u32 index = b->dict_count++;
        b->dicts[index] = (dpak__dict){
            .type_hash = b->types[slot],
            .data = dict,
            .size = (u32)dict_size,
            .cap = cap,
            .zstd_id = ZDICT_getDictID(dict, dict_size),
        };
        dict = NULL; /* owned by b->dicts now */
        for (u32 i = 0; i < b->chunk_count; ++i) {
            dpak__chunk *ch = &b->chunks[i];
            if (cand[i] == NULL)
                continue;
            if (!(ch->flags & DPAK_CHUNK_RAW))
                DE_FREE(a, ch->stored, ch->stored_size);
            ch->stored = cand[i];
            ch->stored_size = cand_size[i];
            ch->flags = 0;
            ch->dict = (u16)index;
            cand[i] = NULL;
        }
    }

done:
    if (cand != NULL) {
        for (u32 i = 0; i < b->chunk_count; ++i)
            if (cand[i] != NULL)
                DE_FREE(a, cand[i], cand_size[i]);
        DE_FREE(a, cand, (size_t)b->chunk_count * sizeof(u8 *));
    }
    if (cand_size != NULL)
        DE_FREE(a, cand_size, (size_t)b->chunk_count * sizeof(u32));
    ZSTD_freeCDict(cdict);
    if (dict != NULL)
        DE_FREE(a, dict, cap);
    if (sizes != NULL)
        DE_FREE(a, sizes, (size_t)nsamples * sizeof(size_t));
    if (samples != NULL)
        DE_FREE(a, samples, (size_t)total);
    return r;
}

static de_result dpak__make_dicts(dpak__build *b) {
    if (!b->w->use_dicts || b->type_count == 0)
        return DE_OK;
    b->dicts = dpak__alloc_zero(b->w->a, (size_t)b->type_count * sizeof *b->dicts);
    if (b->dicts == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "dpak: dictionary table");
    /* Types in ascending hash order, so DICTS comes out sorted by type hash. */
    for (u32 slot = 0; slot < b->type_count && b->dict_count < DPAK_MAX_DICTS; ++slot)
        DE_TRY(dpak__try_dict(b, slot));
    return DE_OK;
}

/* ---- Finish: step 5, layout and encode ------------------------------------------------- */

static de_result dpak__encode(dpak__build *b, const de_allocator *out_a, void **out_data,
                              u64 *out_size) {
    dpak_writer *w = b->w;
    const u32 n = w->entries.count;
    const u32 nb = w->bundles.count;
    const dpak__entry *ents = DE_ARRAY_DATA(&w->entries, dpak__entry);
    const dpak__bundle *buns = DE_ARRAY_DATA(&w->bundles, dpak__bundle);
    const char *strs = DE_ARRAY_DATA(&w->strings, char);

    /* Sizes of the variable sections. */
    u64 dep_total = 0, id_total = 0, string_total = 0;
    for (u32 i = 0; i < n; ++i) {
        dep_total += ents[i].dep_count;
        if (w->include_names && ents[i].has_name)
            string_total += (u64)ents[i].name_len + 1u;
    }
    for (u32 i = 0; i < nb; ++i) {
        id_total += (u64)buns[i].member_count + buns[i].depend_count;
        string_total += (u64)buns[i].name_len + 1u;
    }
    if (string_total > (u64)UINT32_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak: string section exceeds 4 GiB");

    dpak_header h = {
        .magic = {(u8)DPAK_MAGIC_0, (u8)DPAK_MAGIC_1, (u8)DPAK_MAGIC_2, (u8)DPAK_MAGIC_3},
        .version = w->version,
        .header_size = DPAK_HEADER_SIZE,
        .platform = w->platform,
        .flags = w->include_names ? DPAK_HEADER_HAS_NAMES : 0u,
        .chunk_size = w->chunk_size,
        .alignment = w->alignment,
    };
    memcpy(h.build_id, w->build_id, sizeof h.build_id);
    const u32 counts[DPAK_SECTION_COUNT] = {
        [DPAK_SECTION_TOC] = n,
        [DPAK_SECTION_CHUNKS] = b->chunk_count,
        [DPAK_SECTION_DEPS] = (u32)dep_total,
        [DPAK_SECTION_BUNDLES] = nb,
        [DPAK_SECTION_BUNDLE_IDS] = (u32)id_total,
        [DPAK_SECTION_DICTS] = b->dict_count,
        [DPAK_SECTION_NAMES] = w->include_names ? n : 0u,
        [DPAK_SECTION_STRINGS] = (u32)string_total,
    };
    u64 off = DPAK_HEADER_SIZE;
    for (u32 s = 0; s < (u32)DPAK_SECTION_COUNT; ++s) {
        const u64 size = (u64)counts[s] * dpak_section_record_size(w->version, (dpak_section)s);
        if (size == 0)
            continue; /* empty: offset, size and count stay 0 */
        off = dpak__align_up(off, DPAK_SECTION_ALIGNMENT);
        h.sections[s] = (dpak_section_desc){.offset = off, .size = size, .count = counts[s]};
        off += size;
    }
    for (u32 i = 0; i < b->dict_count; ++i) {
        off = dpak__align_up(off, DPAK_SECTION_ALIGNMENT);
        b->dicts[i].file_offset = off;
        off += b->dicts[i].size;
    }
    const u64 data_start = off;
    for (u32 i = 0; i < n; ++i) {
        const dpak__entry *e = &ents[b->order[i]];
        const u32 first = b->entry_chunk[i];
        const u32 nc = (u32)((e->size + w->chunk_size - 1u) / w->chunk_size);
        if (nc == 0)
            continue; /* an empty asset has no data and data_offset 0 */
        off = dpak__align_up(off, w->alignment);
        for (u32 c = 0; c < nc; ++c)
            off += b->chunks[first + c].stored_size;
    }
    h.file_size = off;
    if (!DPAK__FITS_SIZE(h.file_size))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "dpak: pack does not fit in memory");

    u8 *buf = dpak__alloc_zero(out_a, (size_t)h.file_size);
    if (buf == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "dpak: %llu-byte pack",
                       (unsigned long long)h.file_size);

    /* Dictionaries. */
    u8 *dict_table = buf + h.sections[DPAK_SECTION_DICTS].offset;
    for (u32 i = 0; i < b->dict_count; ++i) {
        const dpak_dict_entry de = {
            .type_hash = b->dicts[i].type_hash,
            .file_offset = b->dicts[i].file_offset,
            .size = b->dicts[i].size,
            .zstd_id = b->dicts[i].zstd_id,
            .hash = de_hash64(b->dicts[i].data, b->dicts[i].size, 0),
        };
        dpak_dict_entry_store(dict_table + (size_t)i * dpak_dict_entry_size(w->version), w->version,
                              &de);
        memcpy(buf + de.file_offset, b->dicts[i].data, b->dicts[i].size);
    }
    off = data_start;

    /* TOC, chunks, deps, names, asset data. */
    u8 *toc = buf + h.sections[DPAK_SECTION_TOC].offset;
    u8 *chunk_table = buf + h.sections[DPAK_SECTION_CHUNKS].offset;
    u8 *dep_table = buf + h.sections[DPAK_SECTION_DEPS].offset;
    u8 *name_table = buf + h.sections[DPAK_SECTION_NAMES].offset;
    u8 *string_bytes = buf + h.sections[DPAK_SECTION_STRINGS].offset;
    const u64 *deps = DE_ARRAY_DATA(&w->deps, u64);
    u32 dep_at = 0, string_at = 0;
    for (u32 i = 0; i < n; ++i) {
        const dpak__entry *e = &ents[b->order[i]];
        const u32 first = b->entry_chunk[i];
        const u32 nc = (u32)((e->size + w->chunk_size - 1u) / w->chunk_size);
        dpak_toc_entry t = {
            .id = e->id,
            .type_hash = e->type_hash,
            .raw_size = e->size,
            .first_chunk = first,
            .chunk_count = nc,
            .dep_first = dep_at,
            .dep_count = e->dep_count,
            .flags = e->flags,
        };
        if (nc > 0) {
            off = dpak__align_up(off, w->alignment);
            t.data_offset = off;
        }
        bool all_raw = nc > 0;
        for (u32 c = 0; c < nc; ++c) {
            const dpak__chunk *ch = &b->chunks[first + c];
            const dpak_chunk_entry ce = {
                .file_offset = off,
                .stored_size = ch->stored_size,
                .raw_size = ch->raw_size,
                .dict_index = ch->dict,
                .flags = ch->flags,
                .hash = de_hash64(ch->stored, ch->stored_size, 0),
            };
            dpak_chunk_entry_store(chunk_table +
                                       (size_t)(first + c) * dpak_chunk_entry_size(w->version),
                                   w->version, &ce);
            memcpy(buf + off, ch->stored, ch->stored_size);
            off += ch->stored_size;
            t.stored_size += ch->stored_size;
            all_raw = all_raw && (ch->flags & DPAK_CHUNK_RAW);
        }
        if (all_raw)
            t.flags |= DPAK_ENTRY_UNCOMPRESSED;
        dpak_toc_entry_store(toc + (size_t)i * DPAK_TOC_ENTRY_SIZE, &t);

        for (u32 d = 0; d < e->dep_count; ++d)
            dpak_store_u64(dep_table + (size_t)(dep_at + d) * DPAK_ID_SIZE, deps[e->dep_first + d]);
        dep_at += e->dep_count;

        if (w->include_names) {
            dpak_name_entry ne = {0};
            if (e->has_name) {
                ne = (dpak_name_entry){.offset = string_at, .len = e->name_len};
                memcpy(string_bytes + string_at, strs + e->name_offset, e->name_len);
                string_at += e->name_len + 1u; /* the NUL is already zero */
            }
            dpak_name_entry_store(name_table + (size_t)i * DPAK_NAME_ENTRY_SIZE, &ne);
        }
    }
    DE_ASSERT(off == h.file_size);

    /* Bundles and their id lists. */
    u8 *bundle_table = buf + h.sections[DPAK_SECTION_BUNDLES].offset;
    u8 *id_table = buf + h.sections[DPAK_SECTION_BUNDLE_IDS].offset;
    const u64 *ids = DE_ARRAY_DATA(&w->ids, u64);
    u32 id_at = 0;
    for (u32 i = 0; i < nb; ++i) {
        const dpak__bundle *s = &buns[b->bundle_order[i]];
        dpak_bundle_entry be = {
            .name_hash = s->name_hash,
            .name_offset = string_at,
            .name_len = s->name_len,
            .member_first = id_at,
            .member_count = s->member_count,
            .depend_first = id_at + s->member_count,
            .depend_count = s->depend_count,
            .flags = s->flags,
            .priority = s->priority,
        };
        memcpy(string_bytes + string_at, strs + s->name_offset, s->name_len);
        string_at += s->name_len + 1u;
        for (u32 k = 0; k < s->member_count; ++k)
            dpak_store_u64(id_table + (size_t)(id_at++) * DPAK_ID_SIZE, ids[s->member_first + k]);
        for (u32 k = 0; k < s->depend_count; ++k)
            dpak_store_u64(id_table + (size_t)(id_at++) * DPAK_ID_SIZE, ids[s->depend_first + k]);
        dpak_bundle_entry_store(bundle_table + (size_t)i * DPAK_BUNDLE_ENTRY_SIZE, &be);
    }
    DE_ASSERT(string_at == string_total && id_at == id_total && dep_at == dep_total);

    h.content_hash = de_hash64(buf + DPAK_HEADER_SIZE, (size_t)(h.file_size - DPAK_HEADER_SIZE), 0);
    dpak_header_store(buf, &h);
    *out_data = buf;
    *out_size = h.file_size;
    return DE_OK;
}

/* ---- Finish ----------------------------------------------------------------------------- */

de_result dpak_writer_finish_memory(dpak_writer *w, const de_allocator *a, void **out_data,
                                    u64 *out_size) {
    if (w == NULL || out_data == NULL || out_size == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_writer_finish_memory: NULL argument");
    *out_data = NULL;
    *out_size = 0;
    if (a == NULL)
        a = de_heap_allocator();

    dpak__build b = {.w = w};
    de_result r = dpak__order(&b);
    if (r == DE_OK) {
        b.cctx = ZSTD_createCCtx();
        b.scratch_cap = ZSTD_compressBound(w->chunk_size);
        b.scratch = DE_ALLOC(w->a, b.scratch_cap, 16, DE_MEM_ASSET);
        if (b.cctx == NULL || b.scratch == NULL)
            r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "dpak: zstd context");
    }
    if (r == DE_OK)
        r = dpak__make_chunks(&b);
    if (r == DE_OK)
        r = dpak__make_dicts(&b);
    if (r == DE_OK)
        r = dpak__encode(&b, a, out_data, out_size);
    dpak__build_free(&b);
    return r;
}

de_result dpak_writer_finish(dpak_writer *w, const char *vpath) {
    if (w == NULL || vpath == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_writer_finish: NULL argument");
    void *data;
    u64 size;
    DE_TRY(dpak_writer_finish_memory(w, w->a, &data, &size));
    de_result r = de_file_write_all_atomic(vpath, data, size);
    DE_FREE(w->a, data, (size_t)size);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "dpak: writing %s", vpath);
    return DE_OK;
}
