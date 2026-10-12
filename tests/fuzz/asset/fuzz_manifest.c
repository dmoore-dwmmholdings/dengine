/*
 * fuzz_manifest.c: fuzz target of the asset manifest (assets.toml; SPEC §11.2, §19.1; DENG-145).
 *
 * Input: the bytes up to the first line "#%include" are the root manifest; the bytes after that
 * line are what every include returns, whatever path it names. Without the marker line the root is
 * the whole input and includes read an empty file. The marker is a TOML comment, so a seed file
 * is a valid manifest as a whole too.
 *
 * Each input runs what `de-cook --check` runs: de__manifest_parse, de__manifest_resolve,
 * de__manifest_validate (known types from a fixed list, source existence from a hash of the path,
 * so no disk access), then reads every result through the lookup functions.
 *
 * Nothing touches the file system, and all memory comes from the heap allocator and is released
 * before returning (LeakSanitizer checks it under libFuzzer).
 */
#include <stdlib.h>
#include <string.h>

#include <dengine/de_memory.h>

#include "asset/manifest_internal.h"
#include "asset/resolve_internal.h"
#include "de_fuzz.h"

#define FZ_ROOT "fz:/assets.toml"

static const char fz_marker[] = "\n#%include\n";

typedef struct fz_input {
    const uint8_t *root;
    size_t root_size;
    const uint8_t *include;
    size_t include_size;
} fz_input;

static de_result fz_copy(const uint8_t *src, size_t n, const de_allocator *a, void **data,
                         u64 *size) {
    /* The reader contract: a block of n + 1 bytes that the parser frees. */
    u8 *buf = (u8 *)DE_ALLOC(a, n + 1, 1, DE_MEM_ASSET);
    if (buf == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    if (n > 0)
        memcpy(buf, src, n);
    buf[n] = 0;
    *data = buf;
    *size = n;
    return DE_OK;
}

static de_result fz_read(void *user, const char *vpath, const de_allocator *a, void **data,
                         u64 *size) {
    const fz_input *in = (const fz_input *)user;
    if (strcmp(vpath, FZ_ROOT) == 0)
        return fz_copy(in->root, in->root_size, a, data, size);
    return fz_copy(in->include, in->include_size, a, data, size);
}

/* Deterministic stand-in for de_path_exists: about half of all paths exist. */
static bool fz_exists(void *user, const char *vpath) {
    (void)user;
    u32 h = 2166136261u; /* FNV-1a */
    for (const char *p = vpath; *p != '\0'; ++p)
        h = (h ^ (u8)*p) * 16777619u;
    return (h & 1u) != 0;
}

static void fz_split(const uint8_t *data, size_t size, fz_input *in) {
    const size_t ml = sizeof fz_marker - 1;
    in->root = data;
    in->root_size = size;
    in->include = data + size;
    in->include_size = 0;
    for (size_t i = 0; i + ml <= size; ++i) {
        if (data[i] == '\n' && memcmp(data + i, fz_marker, ml) == 0) {
            in->root_size = i + 1; /* keep the newline */
            in->include = data + i + ml;
            in->include_size = size - i - ml;
            return;
        }
    }
}

/* Reads every pointer the results hand out, so a bad one shows under ASan. */
static void fz_walk(const de__resolved *r) {
    const de__manifest *m = r->manifest;
    volatile size_t sink = 0;
    for (u32 i = 0; i < r->issue_count; ++i) {
        const de__manifest_issue *is = &r->issues[i];
        sink += strlen(is->file) + strlen(is->key_path) + strlen(is->message) + is->line;
    }
    for (u32 i = 0; i < m->asset_count; ++i) {
        const de__manifest_asset *a = &m->assets[i];
        const char *src = de__manifest_source_for(m, a, "pt-BR");
        sink += src != NULL ? strlen(src) : 0;
        if (r->asset_ids[i].hash != 0) {
            const char *path = de__resolved_path(r, r->asset_ids[i]);
            if (path == NULL || de__resolved_asset(r, r->asset_ids[i]) == NULL)
                abort();
            sink += strlen(path);
        }
    }
    for (u32 i = 0; i < r->bundle_count; ++i) {
        const de__resolved_bundle *b = &r->bundles[i];
        if (de__resolved_find_bundle(r, b->bundle->name) == NULL)
            abort();
        for (u32 k = 0; k < b->member_count; ++k)
            if (de__resolved_find(r, b->members[k]) == NULL)
                abort();
        for (u32 k = 0; k < b->depends_count; ++k)
            if (b->depends[k] >= r->bundle_count)
                abort();
    }
    (void)sink;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fz_input in;
    fz_split(data, size, &in);

    de__manifest *m = NULL;
    de__manifest_parse_desc pd = {.path = FZ_ROOT, .read = fz_read, .read_user = &in};
    if (de__manifest_parse(&pd, &m) != DE_OK)
        return 0; /* out of memory only */

    de__resolved *r = NULL;
    if (de__manifest_resolve(m, NULL, &r) == DE_OK) {
        static const char *const known[] = {"texture", "model", "sound", "font", "blob"};
        de__validate_desc vd = {
            .known_types = known,
            .known_type_count = (u32)(sizeof known / sizeof known[0]),
            .exists = fz_exists,
        };
        (void)de__manifest_validate(r, &vd);
        fz_walk(r);
        de__resolved_destroy(r);
    }
    de__manifest_destroy(m);
    return 0;
}
