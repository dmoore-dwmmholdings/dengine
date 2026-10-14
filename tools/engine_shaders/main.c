/*
 * main.c: `de-engine-shaders`, the build-time cook of the renderer's engine shaders (DENG-421).
 * The build runs it once per line of shaders/engine_shaders.cmake (src/render/CMakeLists.txt):
 *
 *   de-engine-shaders --root <shaders dir> --cache <cache dir> --name <name> --source <rel path>
 *                     --out <file.dshd> [--permutation <A=1,B>]...
 *
 * It cooks <root>/<rel path> through the cooker library's built-in shader importer for this host
 * platform's default targets (macOS: metallib + SPIR-V; Linux: SPIR-V; Windows: DXIL + SPIR-V, DXIL
 * only when DXC is found) and writes the cooked `shader` blob (DSHD,
 * src/gpu/shader/shader_format.h) to <out>. The asset ID is "engine/shaders/<name>", the source is
 * mounted as `deengshd:/` and the `.decache` as `deengcache:/`, exactly as the renderer's
 * in-process cook in Debug and Dev builds does (src/render/shaders/engine_shaders.c), so both share
 * the cache entries.
 *
 * It links only the cooker library and what it needs, never the engine (de-cook links the whole
 * engine, de_render included, whose sources this tool generates).
 *
 * Exit: 0 written; 1 the cook failed (message on stderr); 2 bad arguments.
 */
#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_platform.h>

#include "asset/manifest_internal.h"
#include "cook/lib/de_cooker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PERMUTATIONS 32

static int usage(void) {
    fprintf(stderr, "usage: de-engine-shaders --root <dir> --cache <dir> --name <name> --source "
                    "<rel path> --out <file> [--permutation <A=1,B>]...\n");
    return 2;
}

static int fail(const char *what) {
    const de_error *e = de_error_last();
    fprintf(stderr, "de-engine-shaders: %s: %s\n", what, e != NULL ? e->message : "?");
    return 1;
}

/* Writes `size` bytes to the native path `path` atomically (de_file_write_all_atomic through a
 * mount of its directory), so the build never sees a partial blob. */
static bool write_file(const char *path, const void *data, u64 size) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    char *bslash = strrchr(dir, '\\');
    if (bslash != NULL && (slash == NULL || bslash > slash))
        slash = bslash;
    if (slash == NULL)
        return false;
    *slash = '\0';
    if (de_fs_mount("deengout", dir, DE_MOUNT_WRITABLE, 0) != DE_OK)
        return false;
    char vpath[1100];
    snprintf(vpath, sizeof vpath, "deengout:/%s", slash + 1);
    const bool ok = de_file_write_all_atomic(vpath, data, (size_t)size) == DE_OK;
    (void)de_fs_unmount("deengout", dir);
    return ok;
}

int main(int argc, char **argv) {
    const char *root = NULL, *cache = NULL, *name = NULL, *source = NULL, *out = NULL;
    char perms[MAX_PERMUTATIONS][256];
    const char *perm_items[MAX_PERMUTATIONS];
    u32 perm_count = 0;
    for (int i = 1; i < argc; i += 2) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (v == NULL)
            return usage();
        if (strcmp(a, "--root") == 0)
            root = v;
        else if (strcmp(a, "--cache") == 0)
            cache = v;
        else if (strcmp(a, "--name") == 0)
            name = v;
        else if (strcmp(a, "--source") == 0)
            source = v;
        else if (strcmp(a, "--out") == 0)
            out = v;
        else if (strcmp(a, "--permutation") == 0 && perm_count < MAX_PERMUTATIONS) {
            /* "A=1,B" -> "A=1;B" (CMake lists cannot carry ';'). */
            snprintf(perms[perm_count], sizeof perms[0], "%s", v);
            for (char *c = perms[perm_count]; *c != '\0'; ++c)
                if (*c == ',')
                    *c = ';';
            perm_items[perm_count] = perms[perm_count];
            ++perm_count;
        } else
            return usage();
    }
    if (root == NULL || cache == NULL || name == NULL || source == NULL || out == NULL)
        return usage();

    de_asset_type_desc t;
    if (!de_cooker_builtin_importer("shader", &t)) {
        fprintf(stderr, "de-engine-shaders: this build has no shader importer (no Slang)\n");
        return 1;
    }
    if (de_fs_mount("deengshd", root, 0, 0) != DE_OK)
        return fail("mounting the shader directory");
    if (de_fs_mount("deengcache", cache, DE_MOUNT_WRITABLE, 0) != DE_OK)
        return fail("mounting the cache directory");

    de_cooker *ck = NULL;
    if (de_cooker_create(&(de_cooker_desc){.cache_dir = "deengcache:/",
                                           .verify_determinism = DE_COOK_VERIFY_OFF},
                         &ck) != DE_OK ||
        de_cooker_register_type(ck, &t) != DE_OK) {
        de_cooker_destroy(ck);
        return fail("creating the cooker");
    }
    char id[160];
    snprintf(id, sizeof id, "engine/shaders/%s", name);
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = "permutations";
    kv.value.type = DE__MANIFEST_STRING_ARRAY;
    kv.value.u.arr.items = perm_items;
    kv.value.u.arr.count = perm_count;
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = id;
    a.type = "shader";
    a.source = source;
    a.file = "deengshd:/engine_shaders.cmake";
    a.settings = perm_count > 0 ? &kv : NULL;
    a.setting_count = perm_count > 0 ? 1u : 0u;
    de_cook_result res;
    memset(&res, 0, sizeof res);
    const de_cook_input in = {.asset = &a, .asset_root = "deengshd:/", .source = NULL};
    int rc = 0;
    if (de_cooker_cook(ck, &in, &res) != DE_OK) {
        rc = fail(source);
    } else {
        if (!write_file(out, res.data, res.size)) {
            fprintf(stderr, "de-engine-shaders: cannot write %s\n", out);
            rc = 1;
        }
        de_cook_result_free(&res);
    }
    de_cooker_destroy(ck);
    return rc;
}
