/*
 * fuzz_dpak_reader.c: fuzz target of the .dpak pack reader (SPEC §19.1, §21.3; DENG-216).
 *
 * A pack is untrusted input once mods exist. The input is the whole pack file. Each input is opened
 * with dpak_open_memory (no content-hash check, any platform), so a mutation is not stopped by the
 * hash and reaches every metadata check. If the open succeeds, the target walks everything the
 * reader hands out and checks what open promises:
 *
 *   - the header, then every TOC entry: dpak_find(id) gives back the same index (IDs are sorted
 *     and unique), the name ends in a NUL at its length, every dependency ID is readable;
 *   - every chunk is checked against its hash (format v2; dpak_check_chunk) and decoded with
 *     dpak_decode_chunk into a buffer of the chunk size. Open has checked that no chunk decodes
 *     to more than that; a zstd frame that claims more must fail;
 *   - an entry of at most FZ_ENTRY_READ_MAX raw bytes is also read with dpak_read_entry, and when
 *     both reads succeed the bytes must be the same;
 *   - every bundle: dpak_bundle_find(name_hash) gives back the same index, the name ends in a NUL
 *     at its length, every member and depend ID is readable.
 *
 * Then the input is opened again with `check_chunks` (DENG-392): every dictionary is checked at
 * open, and a chunk whose hash does not match must not decode. Then once more with `verify` and
 * the host platform, for the content-hash path and the platform check. Both format versions are
 * fuzzed: the seeds have version 2 packs and version 1 packs (v1_*.dpak). FZ_DECODE_BUDGET caps the
 * bytes decoded per input, so an input with many chunks cannot time out. Nothing touches the file
 * system, and everything is freed before returning (LeakSanitizer runs under libFuzzer).
 *
 * Seed corpus: corpus/dpak_reader/, packs written by the DENG-211 writer. make_dpak_corpus.c
 * makes them and checks that they still open (CTest dpak_corpus.*).
 */
#include <stdlib.h>
#include <string.h>

#include <dengine/de_base.h>

#include <zstd.h>

#include "asset/dpak/dpak_format.h"
#include "asset/dpak/dpak_reader.h"
#include "de_fuzz.h"

/** Entries up to this raw size are also read whole with dpak_read_entry. */
#define FZ_ENTRY_READ_MAX (1u << 20)
/** Most raw bytes decoded per input, over all chunks. */
#define FZ_DECODE_BUDGET (64u << 20)

typedef struct fz_state {
    const dpak_reader *r;
    ZSTD_DCtx *dctx;
    u8 chunk[DPAK_MAX_CHUNK_SIZE];
    u64 budget;
    volatile u64 sink;
} fz_state;

static void fz_touch(fz_state *st, const void *p, size_t n) {
    const u8 *b = (const u8 *)p;
    u64 h = 0;
    for (size_t i = 0; i < n; ++i)
        h = h * 31u + b[i];
    st->sink += h;
}

/* Decodes the chunks of TOC entry `e`. When `whole` is not NULL, chunk k lands at
 * whole + k * chunk_size; otherwise each chunk goes through st->chunk. Returns true when every
 * chunk decoded. */
static bool fz_decode_entry(fz_state *st, const dpak_toc_entry *e, u8 *whole) {
    const dpak_header *h = dpak_get_header(st->r);
    const u8 *base = dpak_memory(st->r);
    bool ok = true;
    for (u32 k = 0; k < e->chunk_count; ++k) {
        dpak_chunk_entry c;
        dpak_chunk(st->r, e->first_chunk + k, &c);
        if (c.raw_size > h->chunk_size || c.raw_size > DPAK_MAX_CHUNK_SIZE)
            abort(); /* open promised raw_size <= chunk_size */
        if (st->budget < c.raw_size)
            return false;
        st->budget -= c.raw_size;
        u8 *dst = whole != NULL ? whole + (u64)k * h->chunk_size : st->chunk;
        const bool hash_ok = dpak_check_chunk(st->r, &c, base + c.file_offset) == DE_OK;
        if (!dpak_has_hashes(st->r) && !hash_ok)
            abort(); /* version 1 has nothing to check */
        de_result dec = dpak_decode_chunk(st->r, st->dctx, &c, base + c.file_offset, dst);
        if (dpak_checks_chunks(st->r) && !hash_ok && dec == DE_OK)
            abort(); /* check_chunks: a damaged chunk never decodes */
        if (dec != DE_OK) {
            ok = false;
            continue;
        }
        fz_touch(st, dst, c.raw_size);
    }
    return ok;
}

static void fz_walk_entries(fz_state *st) {
    const u32 n = dpak_entry_count(st->r);
    for (u32 i = 0; i < n; ++i) {
        dpak_toc_entry e;
        dpak_entry(st->r, i, &e);
        if (dpak_find(st->r, e.id) != i)
            abort(); /* open promised sorted, unique IDs */
        u32 len = 0;
        const char *name = dpak_name(st->r, i, &len);
        if (name != NULL) {
            if (name[len] != '\0')
                abort();
            fz_touch(st, name, len);
        }
        for (u32 k = 0; k < e.dep_count; ++k)
            st->sink += dpak_dep(st->r, e.dep_first + k);

        if (e.raw_size > FZ_ENTRY_READ_MAX) {
            (void)fz_decode_entry(st, &e, NULL);
            continue;
        }
        /* Chunk by chunk, then the whole entry at once: the same bytes when both succeed. */
        size_t n_raw = (size_t)e.raw_size;
        u8 *a = (u8 *)malloc(n_raw != 0 ? n_raw : 1u);
        u8 *b = (u8 *)malloc(n_raw != 0 ? n_raw : 1u);
        if (a == NULL || b == NULL)
            abort();
        bool ok_a = fz_decode_entry(st, &e, a);
        if (ok_a && st->budget >= e.raw_size) {
            st->budget -= e.raw_size;
            if (dpak_read_entry(st->r, i, st->dctx, b, e.raw_size) == DE_OK &&
                memcmp(a, b, n_raw) != 0)
                abort();
        }
        /* A buffer that is too small is refused, never written past. */
        if (e.raw_size != 0 && dpak_read_entry(st->r, i, st->dctx, b, e.raw_size - 1u) == DE_OK)
            abort();
        free(a);
        free(b);
    }
    if (n > 0 && dpak_read_entry(st->r, n, st->dctx, st->chunk, sizeof st->chunk) == DE_OK)
        abort(); /* index out of range */
}

static void fz_walk_dicts(fz_state *st) {
    const u32 n = dpak_dict_count(st->r);
    if (n != dpak_get_header(st->r)->sections[DPAK_SECTION_DICTS].count)
        abort();
    for (u32 i = 0; i < n; ++i) {
        dpak_dict_entry d;
        dpak_dict(st->r, i, &d);
        de_result res = dpak_check_dict(st->r, i);
        if (dpak_checks_chunks(st->r) && res != DE_OK)
            abort(); /* open with check_chunks refused damaged dictionaries */
        st->sink += d.hash + d.type_hash + (u64)res;
    }
    if (dpak_check_dict(st->r, n) != DE_OK)
        abort(); /* out of range: nothing to check */
}

static void fz_walk_bundles(fz_state *st) {
    const u32 n = dpak_bundle_count(st->r);
    for (u32 i = 0; i < n; ++i) {
        dpak_bundle_entry b;
        dpak_bundle(st->r, i, &b);
        if (dpak_bundle_find(st->r, b.name_hash) != i)
            abort(); /* open promised sorted, unique name hashes */
        const char *name = dpak_bundle_name(st->r, &b);
        if (name[b.name_len] != '\0')
            abort();
        fz_touch(st, name, b.name_len);
        for (u32 k = 0; k < b.member_count; ++k)
            st->sink += dpak_bundle_id(st->r, b.member_first + k);
        for (u32 k = 0; k < b.depend_count; ++k)
            st->sink += dpak_bundle_id(st->r, b.depend_first + k);
    }
}

/* Opens the input with `desc` and walks everything. Returns false when it does not open. */
static bool fz_run(const uint8_t *data, size_t size, const dpak_open_desc *desc) {
    dpak_reader *r = NULL;
    if (dpak_open_memory(data, size, desc, &r) != DE_OK) {
        if (r != NULL)
            abort(); /* a failed open leaves *out NULL */
        return false;
    }
    const dpak_header *h = dpak_get_header(r);
    if (h->file_size != size || dpak_memory(r) != data || dpak_file(r) != NULL)
        abort();
    if (dpak_has_hashes(r) != (h->version >= DPAK_VERSION_HASHES))
        abort();
    if (dpak_checks_chunks(r) != (desc->check_chunks && dpak_has_hashes(r)))
        abort();

    fz_state *st = (fz_state *)malloc(sizeof *st);
    if (st == NULL)
        abort();
    memset(st, 0, sizeof *st);
    st->r = r;
    st->budget = FZ_DECODE_BUDGET;
    st->dctx = ZSTD_createDCtx();
    if (st->dctx == NULL)
        abort();
    st->sink = h->content_hash + h->flags + h->platform;
    fz_walk_dicts(st);
    fz_walk_entries(st);
    fz_walk_bundles(st);
    ZSTD_freeDCtx(st->dctx);
    free(st);
    dpak_close(r);
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    dpak_reader *r = NULL;
    if (!fz_run(data, size, &(dpak_open_desc){.platform = DPAK_PLATFORM_ANY}))
        return 0;
    /* Hash checks on (format v2): damaged dictionaries are refused at open, damaged chunks at
     * decode. */
    (void)fz_run(data, size,
                 &(dpak_open_desc){.platform = DPAK_PLATFORM_ANY, .check_chunks = true});

    /* The content hash and the host platform check. */
    r = NULL;
    if (dpak_open_memory(data, size,
                         &(dpak_open_desc){.platform = dpak_platform_host(), .verify = true},
                         &r) == DE_OK)
        dpak_close(r);
    return 0;
}
