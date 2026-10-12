/*
 * dpak_reader.c: the runtime .dpak reader (SPEC §11.7; DENG-212). See dpak_reader.h.
 *
 * Open checks the whole metadata of the pack before anything else uses it (docs/formats/dpak.md,
 * "Reader checks"). Every size is computed in u64 and compared against the file size first, so an
 * overflow cannot pass a check: a crafted or truncated pack fails with DE_ERR_PARSE and never
 * makes the reader read outside its buffer (DENG-216 fuzzes dpak_open_memory).
 */

#include "asset/dpak/dpak_reader.h"

#include "asset/dpak/dpak_format.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#define ZSTD_STATIC_LINKING_ONLY /* ZSTD_customMem, ZSTD_createDDict_advanced */
#include <zstd.h>
#include <xxhash.h>

#include <string.h>

struct dpak_reader {
    const de_allocator *alloc;
    dpak_header h;
    de_file *file;  /**< file reader: the pack, open until close */
    const u8 *mem;  /**< memory reader: the caller's bytes */
    u8 *meta;       /**< file reader: bytes [0, meta_size) of the file (owned) */
    const u8 *base; /**< the metadata: `meta` or `mem` */
    u64 meta_size;  /**< bytes at `base` (the whole pack for a memory reader) */
    ZSTD_DDict **ddicts;
    u32 ddict_count;
    char path[DE_FS_PATH_MAX]; /**< for messages: the vpath, or "(memory)" */
};

/* ---- zstd memory through the reader's allocator ---------------------------------------------- */

#define ZS_PREFIX 16u

static void *zs_alloc(void *opaque, size_t size) {
    const de_allocator *a = (const de_allocator *)opaque;
    u8 *p = (u8 *)DE_ALLOC(a, size + ZS_PREFIX, 16, DE_MEM_ASSET);
    if (p == NULL)
        return NULL;
    memcpy(p, &size, sizeof size);
    return p + ZS_PREFIX;
}

static void zs_free(void *opaque, void *address) {
    if (address == NULL)
        return;
    const de_allocator *a = (const de_allocator *)opaque;
    u8 *p = (u8 *)address - ZS_PREFIX;
    size_t size = 0;
    memcpy(&size, p, sizeof size);
    DE_FREE(a, p, size + ZS_PREFIX);
}

static ZSTD_customMem zs_mem(const de_allocator *a) {
    ZSTD_customMem m = {zs_alloc, zs_free, (void *)(uintptr_t)a};
    return m;
}

/* ---- Small helpers --------------------------------------------------------------------------- */

u32 dpak_platform_host(void) {
#if defined(_WIN32)
    return DPAK_PLATFORM_WINDOWS;
#elif defined(__APPLE__)
    return DPAK_PLATFORM_MACOS;
#elif defined(__linux__)
    return DPAK_PLATFORM_LINUX;
#else
    return DPAK_PLATFORM_ANY;
#endif
}

static const char *platform_name(u32 p) {
    switch (p) {
    case DPAK_PLATFORM_ANY:
        return "any";
    case DPAK_PLATFORM_WINDOWS:
        return "windows";
    case DPAK_PLATFORM_LINUX:
        return "linux";
    case DPAK_PLATFORM_MACOS:
        return "macos";
    default:
        return "unknown";
    }
}

/* True if [off, off + size) lies inside [0, limit). Overflow-safe. */
static bool range_ok(u64 off, u64 size, u64 limit) {
    return off <= limit && size <= limit - off;
}

static bool is_pow2(u32 v) {
    return v != 0 && (v & (v - 1u)) == 0;
}

static const u8 *section_ptr(const dpak_reader *r, dpak_section s) {
    return r->base + r->h.sections[s].offset;
}

static u32 section_count(const dpak_reader *r, dpak_section s) {
    return r->h.sections[s].count;
}

/* The string at [off, off + len) of STRINGS, which must be followed by a NUL. */
static bool string_ok(const dpak_reader *r, u32 off, u32 len) {
    u64 n = r->h.sections[DPAK_SECTION_STRINGS].size;
    if (!range_ok(off, (u64)len + 1u, n))
        return false;
    return section_ptr(r, DPAK_SECTION_STRINGS)[(u64)off + len] == 0;
}

static const char *section_name(dpak_section s) {
    static const char *const k_names[DPAK_SECTION_COUNT] = {
        "TOC", "CHUNKS", "DEPS", "BUNDLES", "BUNDLE_IDS", "DICTS", "NAMES", "STRINGS"};
    return (u32)s < (u32)DPAK_SECTION_COUNT ? k_names[s] : "?";
}

/* ---- Checks ---------------------------------------------------------------------------------- */

/* The fixed header fields. `actual_size` is the real size of the file or buffer. */
static de_result check_header(dpak_reader *r, u64 actual_size, const dpak_open_desc *d) {
    const dpak_header *h = &r->h;
    if (h->version != DPAK_VERSION)
        return DE_FAIL(DE_ERR_VERSION_MISMATCH,
                       "pack \"%s\": format version %u, this build reads version %u", r->path,
                       h->version, DPAK_VERSION);
    if (h->header_size != DPAK_HEADER_SIZE)
        return DE_FAIL(DE_ERR_VERSION_MISMATCH, "pack \"%s\": header size %u, expected %u", r->path,
                       h->header_size, DPAK_HEADER_SIZE);
    if (h->file_size != actual_size)
        return DE_FAIL(DE_ERR_PARSE,
                       "pack \"%s\": the header says %llu bytes, the file has %llu (truncated or "
                       "damaged)",
                       r->path, (unsigned long long)h->file_size, (unsigned long long)actual_size);
    if (h->platform >= (u32)DPAK_PLATFORM_COUNT_)
        return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": unknown platform %u", r->path, h->platform);
    if (d->platform != DPAK_PLATFORM_ANY && h->platform != DPAK_PLATFORM_ANY &&
        h->platform != d->platform)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "pack \"%s\" was cooked for %s, this build runs on %s",
                       r->path, platform_name(h->platform), platform_name(d->platform));
    if (h->chunk_size < DPAK_MIN_CHUNK_SIZE || h->chunk_size > DPAK_MAX_CHUNK_SIZE)
        return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": chunk size %u out of range", r->path,
                       h->chunk_size);
    if (!is_pow2(h->alignment) || h->alignment < DPAK_MIN_ALIGNMENT ||
        h->alignment > DPAK_MAX_ALIGNMENT)
        return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": alignment %u is not valid", r->path,
                       h->alignment);
    for (u32 i = 0; i < (u32)DPAK_SECTION_COUNT; ++i) {
        const dpak_section_desc *s = &h->sections[i];
        u64 rec = dpak_section_record_size((dpak_section)i);
        if (s->count == 0) {
            if (s->size != 0)
                return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": empty section %s has %llu bytes",
                               r->path, section_name((dpak_section)i), (unsigned long long)s->size);
            continue;
        }
        if (s->size != (u64)s->count * rec || s->offset < DPAK_HEADER_SIZE ||
            s->offset % DPAK_SECTION_ALIGNMENT != 0 || !range_ok(s->offset, s->size, h->file_size))
            return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": section %s is out of range", r->path,
                           section_name((dpak_section)i));
    }
    bool names = (h->flags & DPAK_HEADER_HAS_NAMES) != 0;
    u32 name_count = h->sections[DPAK_SECTION_NAMES].count;
    if (names ? name_count != h->sections[DPAK_SECTION_TOC].count : name_count != 0)
        return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": the NAMES section does not match the TOC",
                       r->path);
    if (h->sections[DPAK_SECTION_DICTS].count > DPAK_MAX_DICTS)
        return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": too many dictionaries", r->path);
    return DE_OK;
}

/* Where the sections end (the metadata a reader needs before the dictionaries). */
static u64 sections_end(const dpak_header *h) {
    u64 end = DPAK_HEADER_SIZE;
    for (u32 i = 0; i < (u32)DPAK_SECTION_COUNT; ++i) {
        const dpak_section_desc *s = &h->sections[i];
        if (s->count != 0 && s->offset + s->size > end)
            end = s->offset + s->size;
    }
    return end;
}

/* The dictionary table (sections must be in memory): sorted, inside the file. Writes where the
 * last dictionary ends to `*end`. */
static de_result check_dicts(dpak_reader *r, u64 *end) {
    u32 n = section_count(r, DPAK_SECTION_DICTS);
    const u8 *p = section_ptr(r, DPAK_SECTION_DICTS);
    *end = 0;
    u64 prev = 0;
    for (u32 i = 0; i < n; ++i) {
        dpak_dict_entry d;
        dpak_dict_entry_load(p + (u64)i * DPAK_DICT_ENTRY_SIZE, &d);
        if ((i != 0 && d.type_hash <= prev) || d.size == 0 ||
            !range_ok(d.file_offset, d.size, r->h.file_size))
            return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": dictionary %u is out of range or order",
                           r->path, i);
        prev = d.type_hash;
        if (d.file_offset + d.size > *end)
            *end = d.file_offset + d.size;
    }
    return DE_OK;
}

/* TOC, chunks, dependencies and names. Everything is in memory. */
static de_result check_entries(dpak_reader *r) {
    const dpak_header *h = &r->h;
    u32 n = section_count(r, DPAK_SECTION_TOC);
    u32 chunk_total = section_count(r, DPAK_SECTION_CHUNKS);
    u32 dep_total = section_count(r, DPAK_SECTION_DEPS);
    u32 dict_total = section_count(r, DPAK_SECTION_DICTS);
    const u8 *dicts = section_ptr(r, DPAK_SECTION_DICTS);
    u64 cs = h->chunk_size;
    u64 next_chunk = 0;
    u64 prev_id = 0;
    for (u32 i = 0; i < n; ++i) {
        dpak_toc_entry e;
        dpak_entry(r, i, &e);
        if (e.id == 0 || (i != 0 && e.id <= prev_id))
            return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": TOC entry %u: IDs are not sorted and unique",
                           r->path, i);
        prev_id = e.id;
        /* Chunks are stored in TOC order, each asset's back to back. */
        if (e.first_chunk != next_chunk || !range_ok(e.first_chunk, e.chunk_count, chunk_total) ||
            !range_ok(e.dep_first, e.dep_count, dep_total))
            return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": TOC entry %u: chunk or dependency range",
                           r->path, i);
        next_chunk += e.chunk_count;
        u64 want_chunks = e.raw_size / cs + (e.raw_size % cs != 0 ? 1u : 0u);
        if (want_chunks != e.chunk_count)
            return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": TOC entry %u: %u chunks for %llu bytes",
                           r->path, i, e.chunk_count, (unsigned long long)e.raw_size);
        if (e.chunk_count == 0) {
            if (e.data_offset != 0 || e.stored_size != 0)
                return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": TOC entry %u: empty asset with data",
                               r->path, i);
            continue;
        }
        if (e.data_offset % h->alignment != 0 ||
            !range_ok(e.data_offset, e.stored_size, h->file_size))
            return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": TOC entry %u: data out of range", r->path,
                           i);
        u64 at = e.data_offset, stored = 0;
        for (u32 k = 0; k < e.chunk_count; ++k) {
            dpak_chunk_entry c;
            dpak_chunk(r, e.first_chunk + k, &c);
            u64 want_raw = k + 1u < e.chunk_count ? cs : e.raw_size - (u64)k * cs;
            bool raw = (c.flags & DPAK_CHUNK_RAW) != 0;
            if (c.file_offset != at || c.stored_size == 0 || c.raw_size != want_raw ||
                !range_ok(c.file_offset, c.stored_size, h->file_size) ||
                (raw && c.stored_size != c.raw_size) ||
                ((e.flags & DPAK_ENTRY_UNCOMPRESSED) != 0 && !raw))
                return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": TOC entry %u: chunk %u is not valid",
                               r->path, i, k);
            if (c.dict_index != DPAK_DICT_NONE) {
                dpak_dict_entry d;
                if (raw || c.dict_index >= dict_total)
                    return DE_FAIL(DE_ERR_PARSE,
                                   "pack \"%s\": TOC entry %u: chunk %u names dictionary %u",
                                   r->path, i, k, c.dict_index);
                dpak_dict_entry_load(dicts + (u64)c.dict_index * DPAK_DICT_ENTRY_SIZE, &d);
                if (d.type_hash != e.type_hash)
                    return DE_FAIL(DE_ERR_PARSE,
                                   "pack \"%s\": TOC entry %u: dictionary of another type", r->path,
                                   i);
            }
            at += c.stored_size;
            stored += c.stored_size;
        }
        if (stored != e.stored_size)
            return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": TOC entry %u: stored size %llu, chunks %llu",
                           r->path, i, (unsigned long long)e.stored_size,
                           (unsigned long long)stored);
    }
    if (next_chunk != chunk_total)
        return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": %u chunks, the TOC uses %llu", r->path,
                       chunk_total, (unsigned long long)next_chunk);
    u32 names = section_count(r, DPAK_SECTION_NAMES);
    const u8 *np = section_ptr(r, DPAK_SECTION_NAMES);
    for (u32 i = 0; i < names; ++i) {
        dpak_name_entry ne;
        dpak_name_entry_load(np + (u64)i * DPAK_NAME_ENTRY_SIZE, &ne);
        if (ne.len != 0 && !string_ok(r, ne.offset, ne.len))
            return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": name %u is out of range", r->path, i);
    }
    return DE_OK;
}

static de_result check_bundles(dpak_reader *r) {
    u32 n = section_count(r, DPAK_SECTION_BUNDLES);
    u32 ids = section_count(r, DPAK_SECTION_BUNDLE_IDS);
    u64 prev = 0;
    for (u32 i = 0; i < n; ++i) {
        dpak_bundle_entry b;
        dpak_bundle(r, i, &b);
        if ((i != 0 && b.name_hash <= prev) || !string_ok(r, b.name_offset, b.name_len) ||
            !range_ok(b.member_first, b.member_count, ids) ||
            !range_ok(b.depend_first, b.depend_count, ids) ||
            b.priority >= (u32)DPAK_PRIORITY_COUNT_)
            return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": bundle %u is not valid", r->path, i);
        prev = b.name_hash;
    }
    return DE_OK;
}

/* One ZSTD_DDict per dictionary, referencing the metadata bytes (they live as long as r). */
static de_result make_ddicts(dpak_reader *r) {
    u32 n = section_count(r, DPAK_SECTION_DICTS);
    if (n == 0)
        return DE_OK;
    r->ddicts = (ZSTD_DDict **)DE_ALLOC(r->alloc, sizeof(ZSTD_DDict *) * n,
                                        DE_ALIGNOF(ZSTD_DDict *), DE_MEM_ASSET);
    if (r->ddicts == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "pack \"%s\": out of memory", r->path);
    memset(r->ddicts, 0, sizeof(ZSTD_DDict *) * n);
    r->ddict_count = n;
    const u8 *p = section_ptr(r, DPAK_SECTION_DICTS);
    for (u32 i = 0; i < n; ++i) {
        dpak_dict_entry d;
        dpak_dict_entry_load(p + (u64)i * DPAK_DICT_ENTRY_SIZE, &d);
        r->ddicts[i] = ZSTD_createDDict_advanced(r->base + d.file_offset, d.size, ZSTD_dlm_byRef,
                                                 ZSTD_dct_auto, zs_mem(r->alloc));
        if (r->ddicts[i] == NULL)
            return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": dictionary %u is not a zstd dictionary",
                           r->path, i);
        if (ZSTD_getDictID_fromDDict(r->ddicts[i]) != d.zstd_id)
            return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": dictionary %u has ID %u, the table says %u",
                           r->path, i, ZSTD_getDictID_fromDDict(r->ddicts[i]), d.zstd_id);
    }
    return DE_OK;
}

/* Content hash of a file reader: streamed in 1 MiB blocks. */
static de_result verify_file(dpak_reader *r) {
    const size_t block = (size_t)1 << 20;
    u8 *buf = (u8 *)DE_ALLOC(r->alloc, block, 16, DE_MEM_ASSET);
    XXH3_state_t *st = XXH3_createState();
    de_result res = DE_OK;
    if (buf == NULL || st == NULL || XXH3_64bits_reset_withSeed(st, 0) != XXH_OK) {
        res = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "pack \"%s\": out of memory", r->path);
    } else {
        for (u64 at = DPAK_HEADER_SIZE; at < r->h.file_size;) {
            u64 want = r->h.file_size - at < block ? r->h.file_size - at : block;
            u64 got = 0;
            if (de_file_read(r->file, buf, at, want, &got) != DE_OK || got != want) {
                res = DE_FAIL(DE_ERR_IO, "pack \"%s\": read failed at %llu", r->path,
                              (unsigned long long)at);
                break;
            }
            (void)XXH3_64bits_update(st, buf, (size_t)want);
            at += want;
        }
        if (res == DE_OK && (u64)XXH3_64bits_digest(st) != r->h.content_hash)
            res =
                DE_FAIL(DE_ERR_PARSE, "pack \"%s\": content hash mismatch (damaged file)", r->path);
    }
    if (st != NULL)
        XXH3_freeState(st);
    if (buf != NULL)
        DE_FREE(r->alloc, buf, block);
    return res;
}

/* Everything after the header is at `base` (meta_size bytes, at least up to the dictionaries). */
static de_result check_all(dpak_reader *r) {
    de_result res = check_entries(r);
    if (res == DE_OK)
        res = check_bundles(r);
    if (res == DE_OK)
        res = make_ddicts(r);
    return res;
}

/* ---- Open and close -------------------------------------------------------------------------- */

static dpak_reader *reader_new(const dpak_open_desc *desc, const char *path) {
    const de_allocator *a =
        desc != NULL && desc->allocator != NULL ? desc->allocator : de_heap_allocator();
    dpak_reader *r = DE_NEW(a, dpak_reader, DE_MEM_ASSET);
    if (r == NULL)
        return NULL;
    memset(r, 0, sizeof *r);
    r->alloc = a;
    size_t n = strlen(path);
    if (n >= sizeof r->path)
        n = sizeof r->path - 1u;
    memcpy(r->path, path, n);
    r->path[n] = '\0';
    return r;
}

void dpak_close(dpak_reader *r) {
    if (r == NULL)
        return;
    for (u32 i = 0; i < r->ddict_count; ++i)
        if (r->ddicts[i] != NULL)
            ZSTD_freeDDict(r->ddicts[i]);
    if (r->ddicts != NULL)
        DE_FREE(r->alloc, r->ddicts, sizeof(ZSTD_DDict *) * r->ddict_count);
    if (r->meta != NULL)
        DE_FREE(r->alloc, r->meta, (size_t)r->meta_size);
    if (r->file != NULL)
        de_file_close(r->file);
    DE_FREE(r->alloc, r, sizeof *r);
}

static const dpak_open_desc k_default_desc = {0};

de_result dpak_open_memory(const void *data, u64 size, const dpak_open_desc *desc,
                           dpak_reader **out) {
    if (out != NULL)
        *out = NULL;
    if (out == NULL || (data == NULL && size != 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_open_memory: NULL argument");
    const dpak_open_desc *d = desc != NULL ? desc : &k_default_desc;
    dpak_reader *r = reader_new(d, "(memory)");
    if (r == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "dpak_open_memory: out of memory");
    const u8 *p = (const u8 *)data;
    de_result res = DE_OK;
    if (size < DPAK_HEADER_SIZE || !dpak_magic_ok(p)) {
        res = DE_FAIL(DE_ERR_PARSE, "pack \"%s\": not a .dpak (no DPAK header)", r->path);
    } else {
        dpak_header_load(p, &r->h);
        r->mem = p;
        r->base = p;
        r->meta_size = size;
        u64 dict_end = 0;
        res = check_header(r, size, d);
        if (res == DE_OK)
            res = check_dicts(r, &dict_end);
        if (res == DE_OK && d->verify &&
            de_hash64(p + DPAK_HEADER_SIZE, (size_t)(size - DPAK_HEADER_SIZE), 0) !=
                r->h.content_hash)
            res =
                DE_FAIL(DE_ERR_PARSE, "pack \"%s\": content hash mismatch (damaged data)", r->path);
        if (res == DE_OK)
            res = check_all(r);
    }
    if (res != DE_OK) {
        dpak_close(r);
        return res;
    }
    *out = r;
    return DE_OK;
}

/* Reads [from, to) of the file into r->meta (grown to `to` bytes). */
static de_result read_meta(dpak_reader *r, u64 from, u64 to) {
    if (to > (u64)SIZE_MAX / 2u)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "pack \"%s\": metadata too large", r->path);
    void *p = DE_REALLOC(r->alloc, r->meta, (size_t)r->meta_size, (size_t)to, 16, DE_MEM_ASSET);
    if (p == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "pack \"%s\": out of memory for %llu bytes", r->path,
                       (unsigned long long)to);
    r->meta = (u8 *)p;
    r->meta_size = to;
    r->base = r->meta;
    u64 got = 0;
    de_result res = de_file_read(r->file, r->meta + from, from, to - from, &got);
    if (res != DE_OK || got != to - from)
        return DE_FAIL(res != DE_OK ? res : DE_ERR_PARSE,
                       "pack \"%s\": cannot read bytes %llu..%llu", r->path,
                       (unsigned long long)from, (unsigned long long)to);
    return DE_OK;
}

de_result dpak_open(const char *vpath, const dpak_open_desc *desc, dpak_reader **out) {
    if (out != NULL)
        *out = NULL;
    if (out == NULL || vpath == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_open: NULL argument");
    const dpak_open_desc *d = desc != NULL ? desc : &k_default_desc;
    dpak_reader *r = reader_new(d, vpath);
    if (r == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "dpak_open: out of memory");
    de_result res = de_file_open(vpath, DE_FILE_READ, &r->file);
    if (res != DE_OK) {
        const de_error *e = de_error_last();
        res = DE_FAIL(res, "cannot open pack \"%s\": %s", vpath,
                      e != NULL && e->result == res ? e->message : de_result_str(res));
        dpak_close(r);
        return res;
    }
    u64 size = de_file_size(r->file);
    if (size < DPAK_HEADER_SIZE) {
        res = DE_FAIL(DE_ERR_PARSE, "pack \"%s\": not a .dpak (%llu bytes)", vpath,
                      (unsigned long long)size);
    } else {
        res = read_meta(r, 0, DPAK_HEADER_SIZE);
        if (res == DE_OK && !dpak_magic_ok(r->meta))
            res = DE_FAIL(DE_ERR_PARSE, "pack \"%s\": not a .dpak (no DPAK header)", vpath);
        if (res == DE_OK) {
            dpak_header_load(r->meta, &r->h);
            res = check_header(r, size, d);
        }
        u64 dict_end = 0;
        if (res == DE_OK)
            res = read_meta(r, DPAK_HEADER_SIZE, sections_end(&r->h));
        if (res == DE_OK)
            res = check_dicts(r, &dict_end);
        if (res == DE_OK && dict_end > r->meta_size)
            res = read_meta(r, r->meta_size, dict_end);
        if (res == DE_OK && d->verify)
            res = verify_file(r);
        if (res == DE_OK)
            res = check_all(r);
    }
    if (res != DE_OK) {
        dpak_close(r);
        return res;
    }
    *out = r;
    return DE_OK;
}

/* ---- Accessors ------------------------------------------------------------------------------- */

const dpak_header *dpak_get_header(const dpak_reader *r) {
    return &r->h;
}

de_file *dpak_file(const dpak_reader *r) {
    return r->file;
}

const u8 *dpak_memory(const dpak_reader *r) {
    return r->mem;
}

u32 dpak_entry_count(const dpak_reader *r) {
    return section_count(r, DPAK_SECTION_TOC);
}

void dpak_entry(const dpak_reader *r, u32 index, dpak_toc_entry *out) {
    dpak_toc_entry_load(section_ptr(r, DPAK_SECTION_TOC) + (u64)index * DPAK_TOC_ENTRY_SIZE, out);
}

u32 dpak_find(const dpak_reader *r, u64 id) {
    const u8 *toc = section_ptr(r, DPAK_SECTION_TOC);
    u32 lo = 0, hi = section_count(r, DPAK_SECTION_TOC);
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        u64 v = dpak_load_u64(toc + (u64)mid * DPAK_TOC_ENTRY_SIZE); /* id is the first field */
        if (v == id)
            return mid;
        if (v < id)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return DPAK_NOT_FOUND;
}

void dpak_chunk(const dpak_reader *r, u32 index, dpak_chunk_entry *out) {
    dpak_chunk_entry_load(section_ptr(r, DPAK_SECTION_CHUNKS) + (u64)index * DPAK_CHUNK_ENTRY_SIZE,
                          out);
}

u64 dpak_dep(const dpak_reader *r, u32 index) {
    return dpak_load_u64(section_ptr(r, DPAK_SECTION_DEPS) + (u64)index * DPAK_ID_SIZE);
}

const char *dpak_name(const dpak_reader *r, u32 index, u32 *len) {
    if (len != NULL)
        *len = 0;
    if (index >= section_count(r, DPAK_SECTION_NAMES))
        return NULL;
    dpak_name_entry ne;
    dpak_name_entry_load(section_ptr(r, DPAK_SECTION_NAMES) + (u64)index * DPAK_NAME_ENTRY_SIZE,
                         &ne);
    if (ne.len == 0)
        return NULL;
    if (len != NULL)
        *len = ne.len;
    return (const char *)section_ptr(r, DPAK_SECTION_STRINGS) + ne.offset;
}

u32 dpak_bundle_count(const dpak_reader *r) {
    return section_count(r, DPAK_SECTION_BUNDLES);
}

void dpak_bundle(const dpak_reader *r, u32 index, dpak_bundle_entry *out) {
    dpak_bundle_entry_load(
        section_ptr(r, DPAK_SECTION_BUNDLES) + (u64)index * DPAK_BUNDLE_ENTRY_SIZE, out);
}

u32 dpak_bundle_find(const dpak_reader *r, u64 name_hash) {
    const u8 *b = section_ptr(r, DPAK_SECTION_BUNDLES);
    u32 lo = 0, hi = section_count(r, DPAK_SECTION_BUNDLES);
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        u64 v = dpak_load_u64(b + (u64)mid * DPAK_BUNDLE_ENTRY_SIZE); /* name_hash first */
        if (v == name_hash)
            return mid;
        if (v < name_hash)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return DPAK_NOT_FOUND;
}

const char *dpak_bundle_name(const dpak_reader *r, const dpak_bundle_entry *b) {
    return (const char *)section_ptr(r, DPAK_SECTION_STRINGS) + b->name_offset;
}

u64 dpak_bundle_id(const dpak_reader *r, u32 index) {
    return dpak_load_u64(section_ptr(r, DPAK_SECTION_BUNDLE_IDS) + (u64)index * DPAK_ID_SIZE);
}

/* ---- Data ------------------------------------------------------------------------------------ */

de_result dpak_decode_chunk(const dpak_reader *r, void *dctx, const dpak_chunk_entry *c,
                            const void *src, void *dst) {
    if ((c->flags & DPAK_CHUNK_RAW) != 0) {
        memcpy(dst, src, c->raw_size);
        return DE_OK;
    }
    ZSTD_DCtx *d = (ZSTD_DCtx *)dctx;
    size_t got;
    if (c->dict_index != DPAK_DICT_NONE)
        got = ZSTD_decompress_usingDDict(d, dst, c->raw_size, src, c->stored_size,
                                         r->ddicts[c->dict_index]);
    else
        got = ZSTD_decompressDCtx(d, dst, c->raw_size, src, c->stored_size);
    if (ZSTD_isError(got))
        return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": corrupt chunk at %llu: %s", r->path,
                       (unsigned long long)c->file_offset, ZSTD_getErrorName(got));
    if (got != c->raw_size)
        return DE_FAIL(DE_ERR_PARSE, "pack \"%s\": chunk at %llu decodes to %zu bytes, not %u",
                       r->path, (unsigned long long)c->file_offset, got, c->raw_size);
    return DE_OK;
}

de_result dpak_read_entry(const dpak_reader *r, u32 index, void *dctx, void *dst, u64 dst_size) {
    if (r == NULL || index >= dpak_entry_count(r) || (dst == NULL && dst_size != 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_read_entry: bad argument");
    dpak_toc_entry e;
    dpak_entry(r, index, &e);
    if (dst_size < e.raw_size)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "dpak_read_entry: %llu bytes, entry needs %llu",
                       (unsigned long long)dst_size, (unsigned long long)e.raw_size);
    if (e.chunk_count == 0)
        return DE_OK;
    ZSTD_DCtx *own = NULL;
    if (dctx == NULL) {
        own = ZSTD_createDCtx_advanced(zs_mem(r->alloc));
        if (own == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "pack \"%s\": out of memory", r->path);
        dctx = own;
    }
    u8 *tmp = NULL;
    size_t tmp_size = 0;
    if (r->mem == NULL) { /* a stored chunk is never larger than the format allows a u32 */
        tmp_size = DPAK_MAX_CHUNK_SIZE;
        for (u32 k = 0; k < e.chunk_count; ++k) {
            dpak_chunk_entry c;
            dpak_chunk(r, e.first_chunk + k, &c);
            if (c.stored_size > tmp_size)
                tmp_size = c.stored_size;
        }
        tmp = (u8 *)DE_ALLOC(r->alloc, tmp_size, 16, DE_MEM_ASSET);
    }
    de_result res = DE_OK;
    if (r->mem == NULL && tmp == NULL)
        res = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "pack \"%s\": out of memory", r->path);
    u8 *out = (u8 *)dst;
    for (u32 k = 0; res == DE_OK && k < e.chunk_count; ++k) {
        dpak_chunk_entry c;
        dpak_chunk(r, e.first_chunk + k, &c);
        const u8 *src = NULL;
        if (r->mem != NULL) {
            src = r->mem + c.file_offset;
        } else {
            u64 got = 0;
            res = de_file_read(r->file, tmp, c.file_offset, c.stored_size, &got);
            if (res != DE_OK || got != c.stored_size) {
                res = DE_FAIL(res != DE_OK ? res : DE_ERR_IO, "pack \"%s\": read failed at %llu",
                              r->path, (unsigned long long)c.file_offset);
                break;
            }
            src = tmp;
        }
        res = dpak_decode_chunk(r, dctx, &c, src, out + (u64)k * r->h.chunk_size);
    }
    if (tmp != NULL)
        DE_FREE(r->alloc, tmp, tmp_size);
    if (own != NULL)
        ZSTD_freeDCtx(own);
    return res;
}
