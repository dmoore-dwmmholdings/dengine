/*
 * fuzz_dpak_file.c: fuzz target of the .dpak file reader (SPEC §19.1, §21.3; DENG-395).
 *
 * fuzz_dpak_reader covers dpak_open_memory. A mounted pack is opened with dpak_open instead, which
 * has its own code: it reads the header, then the sections, then the dictionaries from the file
 * in three steps (read_meta), and reads asset data through de_file_read. This target covers that
 * path. Each input is written to a scratch file under a writable VFS root, then opened with
 * dpak_open and, as the reference, with dpak_open_memory on the same bytes. The two readers must
 * agree:
 *
 *   - both opens give the same result, for each of the three settings fuzz_dpak_reader uses
 *     (any platform; check_chunks; verify with the host platform);
 *   - when both open: the same header, and the same TOC, chunk, dependency, dictionary, name and
 *     bundle records; dpak_check_dict gives the same result for every dictionary;
 *   - dpak_read_entry gives the same result and the same bytes for every entry of at most
 *     FZ_ENTRY_READ_MAX raw bytes (the file reader reads each chunk with de_file_read first), and
 *     refuses a buffer one byte too small.
 *
 * Scratch file: the native directory DE_FUZZ_DPAK_FILE_DIR (the build directory of this target;
 * the environment variable of the same name overrides it) is mounted once as `fzdpak:/`. The file
 * name holds the process ID, so libFuzzer's -fork, -jobs and -merge workers do not share a file.
 * It is written for each input and deleted before returning (a crash leaves it behind). The reader
 * under test opens and closes its own de_file each time, and everything is closed and freed
 * before returning.
 *
 * Seed corpus: corpus/dpak_reader/, shared with fuzz_dpak_reader (packs of the DENG-211 writer).
 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L /* getpid */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dengine/de_base.h>
#include <dengine/de_platform.h>

#include <zstd.h>

#include "asset/dpak/dpak_format.h"
#include "asset/dpak/dpak_reader.h"
#include "de_fuzz.h"

#if defined(_WIN32)
#include <process.h> /* _getpid */
#define fz_getpid _getpid
#else
#include <unistd.h>
#define fz_getpid getpid
#endif

#ifndef DE_FUZZ_DPAK_FILE_DIR
#error "DE_FUZZ_DPAK_FILE_DIR must name the scratch directory (tests/fuzz/asset/CMakeLists.txt)"
#endif

/** Entries up to this raw size are read whole with dpak_read_entry, from both readers. */
#define FZ_ENTRY_READ_MAX (1u << 20)
/** Most raw bytes read per input and per setting, over all entries. */
#define FZ_READ_BUDGET (16u << 20)

/** The scratch file's virtual path, set on the first call. */
static char g_path[64];

/* Mounts the scratch root and names the file, once per process. */
static void fz_setup(void) {
    if (g_path[0] != '\0')
        return;
    const char *dir = getenv("DE_FUZZ_DPAK_FILE_DIR");
    if (dir == NULL || dir[0] == '\0')
        dir = DE_FUZZ_DPAK_FILE_DIR;
    if (de_fs_mount("fzdpak", dir, DE_MOUNT_WRITABLE, 0) != DE_OK) {
        fprintf(stderr, "fuzz_dpak_file: cannot mount the scratch directory %s\n", dir);
        abort();
    }
    snprintf(g_path, sizeof g_path, "fzdpak:/fuzz_dpak_file.%lu.dpak", (unsigned long)fz_getpid());
}

/* Replaces the scratch file with the input. */
static void fz_write(const uint8_t *data, size_t size) {
    de_file *f = NULL;
    if (de_file_open(g_path, DE_FILE_WRITE, &f) != DE_OK)
        abort();
    if (size != 0 && de_file_write(f, data, size) != DE_OK)
        abort();
    de_file_close(f);
}

/* The two readers hand out the same records. */
static void fz_compare_meta(const dpak_reader *rf, const dpak_reader *rm) {
    const dpak_header *hf = dpak_get_header(rf);
    if (memcmp(hf, dpak_get_header(rm), sizeof *hf) != 0)
        abort();
    if (dpak_file(rf) == NULL || dpak_memory(rf) != NULL)
        abort(); /* a file reader reads through its de_file */
    if (dpak_has_hashes(rf) != dpak_has_hashes(rm) ||
        dpak_checks_chunks(rf) != dpak_checks_chunks(rm))
        abort();

    const u32 n = dpak_entry_count(rf);
    if (n != dpak_entry_count(rm))
        abort();
    for (u32 i = 0; i < n; ++i) {
        dpak_toc_entry ef, em;
        dpak_entry(rf, i, &ef);
        dpak_entry(rm, i, &em);
        if (memcmp(&ef, &em, sizeof ef) != 0 || dpak_find(rf, ef.id) != i)
            abort();
        for (u32 k = 0; k < ef.chunk_count; ++k) {
            dpak_chunk_entry cf, cm;
            dpak_chunk(rf, ef.first_chunk + k, &cf);
            dpak_chunk(rm, ef.first_chunk + k, &cm);
            if (memcmp(&cf, &cm, sizeof cf) != 0)
                abort();
        }
        for (u32 k = 0; k < ef.dep_count; ++k)
            if (dpak_dep(rf, ef.dep_first + k) != dpak_dep(rm, ef.dep_first + k))
                abort();
        u32 lf = 0, lm = 0;
        const char *nf = dpak_name(rf, i, &lf);
        const char *nm = dpak_name(rm, i, &lm);
        if ((nf == NULL) != (nm == NULL))
            abort();
        if (nf != NULL && (lf != lm || memcmp(nf, nm, (size_t)lf + 1u) != 0))
            abort(); /* the same bytes and the NUL */
    }

    const u32 nd = dpak_dict_count(rf);
    if (nd != dpak_dict_count(rm))
        abort();
    for (u32 i = 0; i < nd; ++i) {
        dpak_dict_entry df, dm;
        dpak_dict(rf, i, &df);
        dpak_dict(rm, i, &dm);
        if (memcmp(&df, &dm, sizeof df) != 0)
            abort();
        /* The file reader checks the dictionary bytes it read into its metadata block. */
        if (dpak_check_dict(rf, i) != dpak_check_dict(rm, i))
            abort();
    }

    const u32 nb = dpak_bundle_count(rf);
    if (nb != dpak_bundle_count(rm))
        abort();
    for (u32 i = 0; i < nb; ++i) {
        dpak_bundle_entry bf, bm;
        dpak_bundle(rf, i, &bf);
        dpak_bundle(rm, i, &bm);
        if (memcmp(&bf, &bm, sizeof bf) != 0)
            abort();
        if (memcmp(dpak_bundle_name(rf, &bf), dpak_bundle_name(rm, &bm),
                   (size_t)bf.name_len + 1u) != 0)
            abort(); /* the same bytes and the NUL */
        for (u32 k = 0; k < bf.member_count; ++k)
            if (dpak_bundle_id(rf, bf.member_first + k) != dpak_bundle_id(rm, bf.member_first + k))
                abort();
        for (u32 k = 0; k < bf.depend_count; ++k)
            if (dpak_bundle_id(rf, bf.depend_first + k) != dpak_bundle_id(rm, bf.depend_first + k))
                abort();
    }
}

/* Reads every small entry through both readers: same result, same bytes. */
static void fz_compare_reads(const dpak_reader *rf, const dpak_reader *rm, ZSTD_DCtx *dctx) {
    u64 budget = FZ_READ_BUDGET;
    const u32 n = dpak_entry_count(rf);
    for (u32 i = 0; i < n; ++i) {
        dpak_toc_entry e;
        dpak_entry(rf, i, &e);
        if (e.raw_size > FZ_ENTRY_READ_MAX || e.raw_size > budget)
            continue;
        budget -= e.raw_size;
        const size_t n_raw = (size_t)e.raw_size;
        u8 *a = (u8 *)malloc(n_raw != 0 ? n_raw : 1u);
        u8 *b = (u8 *)malloc(n_raw != 0 ? n_raw : 1u);
        if (a == NULL || b == NULL)
            abort();
        const de_result res_f = dpak_read_entry(rf, i, dctx, a, e.raw_size);
        const de_result res_m = dpak_read_entry(rm, i, dctx, b, e.raw_size);
        if (res_f != res_m)
            abort();
        if (res_f == DE_OK && memcmp(a, b, n_raw) != 0)
            abort();
        /* A buffer that is too small is refused before any read. */
        if (e.raw_size != 0 && dpak_read_entry(rf, i, dctx, a, e.raw_size - 1u) == DE_OK)
            abort();
        free(a);
        free(b);
    }
    if (n > 0 && dpak_read_entry(rf, n, dctx, NULL, 0) == DE_OK)
        abort(); /* index out of range */
}

/* Opens the scratch file and the input bytes with `desc`; they must agree. */
static void fz_run(const uint8_t *data, size_t size, const dpak_open_desc *desc, ZSTD_DCtx *dctx) {
    dpak_reader *rf = NULL;
    dpak_reader *rm = NULL;
    const de_result res_f = dpak_open(g_path, desc, &rf);
    const de_result res_m = dpak_open_memory(data, size, desc, &rm);
    if (res_f != res_m) {
        fprintf(stderr, "fuzz_dpak_file: dpak_open gave %s, dpak_open_memory %s\n",
                de_result_str(res_f), de_result_str(res_m));
        abort();
    }
    if ((res_f == DE_OK) != (rf != NULL) || (res_m == DE_OK) != (rm != NULL))
        abort(); /* a failed open leaves *out NULL */
    if (res_f == DE_OK) {
        fz_compare_meta(rf, rm);
        fz_compare_reads(rf, rm, dctx);
    }
    dpak_close(rf);
    dpak_close(rm);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fz_setup();
    fz_write(data, size);
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    if (dctx == NULL)
        abort();
    fz_run(data, size, &(dpak_open_desc){.platform = DPAK_PLATFORM_ANY}, dctx);
    fz_run(data, size, &(dpak_open_desc){.platform = DPAK_PLATFORM_ANY, .check_chunks = true},
           dctx);
    fz_run(data, size, &(dpak_open_desc){.platform = dpak_platform_host(), .verify = true}, dctx);
    ZSTD_freeDCtx(dctx);
    /* libFuzzer leaves through _Exit on most paths (timeouts, crashes, -merge children), so an
     * atexit handler would not run: delete the file after each input instead. */
    if (de_file_delete(g_path) != DE_OK)
        abort();
    return 0;
}
