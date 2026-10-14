/* main.c — de-golden, the golden-image test executable (DENG-329, SPEC §19.2).
 *
 *   de-golden [--backend metal|vulkan|d3d12|default] [--warp] [--adapter N]
 *             [--scene <glob>] [--tier base|rt] [--out <dir>] [--update-refs]
 *             [--refs <dir>] [--config <scenes.toml>] [--list]
 *
 * For each registered scene whose name matches --scene (default `*`) and whose scenes.toml table
 * lists the backend: for each tier of the scene that the device has, render it (golden.c), read it
 * back and compare it with <refs>/<backend>/<tier>/<reference>.png by LDR-FLIP (flip/flip.h). It
 * passes when the mean error is <= threshold_mean and the 99th percentile <= threshold_p99.
 *
 *   --warp         D3D12: run on WARP, the software adapter (the last adapter of the backend's
 *                  list; the D3D12 CTests use the vendored one, DENG-405).
 *   --out <dir>    writes <dir>/<backend>/<tier>/<scene>.png (the image), and for a failure also
 *                  <scene>.ref.png (the reference) and <scene>.flip.png (the FLIP heat map). The
 *                  layout matches refs/, so a reviewed image can be copied there as it is.
 *   --update-refs  writes the image as the new reference instead of comparing. Refused when the
 *                  environment variable CI is set: references come from a person's review.
 *
 * scenes.toml keys per [scene.<name>]: threshold_mean, threshold_p99 (required); backends
 * (default all), tiers (default ["base"]), requires (cap names, default none), size (default 512,
 * or [w, h]), format ("rgba8" default, or "rgba16f"), reference (another scene's reference
 * images; default the scene's own), expect_fail (the self test: it passes only when the
 * comparison fails and the heat map was written), description.
 *
 * Any engine log record of level ERROR or above during a scene (validation messages included)
 * fails it. Exit status: 0 every selected scene passed (or skipped, with one passing at least),
 * 1 a failure, 2 usage or configuration error, 77 everything was skipped (CTest: skipped).
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#endif
#include "golden.h"

#include "flip.h"

#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include <stb_image.h>
#include <stb_image_write.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc17.h>

#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

#if defined(_WIN32) && defined(DE_GOLDEN_D3D12_AGILITY)
#include "gpu/d3d12/d3d12_agility_exports.h" /* D3D12SDKVersion / D3D12SDKPath (DENG-285) */
#endif

#define EXIT_SKIP  77
#define MAX_TIERS  2u
#define MAX_CAPS   8u
#define MAX_CONFIG 256u

#if defined(DE_DEBUG) || defined(DE_DEV)
#define GOLDEN_VALIDATION true /* API validation: Vulkan layer, D3D12 debug layer */
#else
#define GOLDEN_VALIDATION false
#endif

/* ---------- Options ---------- */

typedef struct options {
    de_gpu_backend backend;
    bool warp, update_refs, list;
    u32 adapter;
    const char *scene_glob, *tier, *out, *refs, *config;
} options;

static int usage(const char *bad) {
    fprintf(stderr,
            "de-golden: bad argument '%s'\n"
            "usage: de-golden [--backend metal|vulkan|d3d12|default] [--warp] [--adapter N]\n"
            "                 [--scene <glob>] [--tier base|rt] [--out <dir>] [--update-refs]\n"
            "                 [--refs <dir>] [--config <scenes.toml>] [--list]\n",
            bad);
    return 2;
}

static int parse(int argc, char **argv, options *o) {
    o->scene_glob = "*";
    o->refs = DE_GOLDEN_REFS_DIR;
    o->config = DE_GOLDEN_SCENES_TOML;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--warp") == 0) {
            o->warp = true;
            continue;
        }
        if (strcmp(a, "--update-refs") == 0) {
            o->update_refs = true;
            continue;
        }
        if (strcmp(a, "--list") == 0) {
            o->list = true;
            continue;
        }
        if (v == NULL)
            return usage(a);
        ++i; /* every other option takes a value */
        if (strcmp(a, "--backend") == 0) {
            static const char *const names[] = {"default", "d3d12", "vulkan", "metal"};
            o->backend = DE_GPU_BACKEND_COUNT_;
            for (u32 b = 0; b < 4u; ++b)
                if (strcmp(v, names[b]) == 0)
                    o->backend = (de_gpu_backend)b; /* the order of de_gpu_backend */
            if (o->backend == DE_GPU_BACKEND_COUNT_)
                return usage(v);
        } else if (strcmp(a, "--adapter") == 0) {
            o->adapter = (u32)strtoul(v, NULL, 10);
        } else if (strcmp(a, "--scene") == 0) {
            o->scene_glob = v;
        } else if (strcmp(a, "--tier") == 0) {
            if (strcmp(v, "base") != 0 && strcmp(v, "rt") != 0)
                return usage(v);
            o->tier = v;
        } else if (strcmp(a, "--out") == 0) {
            o->out = v;
        } else if (strcmp(a, "--refs") == 0) {
            o->refs = v;
        } else if (strcmp(a, "--config") == 0) {
            o->config = v;
        } else {
            return usage(a);
        }
    }
    return 0;
}

/* `*` and `?` glob. */
static bool glob_match(const char *p, const char *s) {
    for (; *p; ++p, ++s) {
        if (*p == '*') {
            for (const char *t = s;; ++t) {
                if (glob_match(p + 1, t))
                    return true;
                if (*t == '\0')
                    return false;
            }
        }
        if (*s == '\0' || (*p != '?' && *p != *s))
            return false;
    }
    return *s == '\0';
}

/* ---------- scenes.toml ---------- */

typedef struct scene_config {
    char name[64];
    char reference[64];
    double threshold_mean, threshold_p99;
    bool backends[DE_GPU_BACKEND_COUNT_];
    char tiers[MAX_TIERS][8];
    u32 tier_count;
    char requires[MAX_CAPS][32];
    u32 require_count;
    u32 width, height;
    de_gpu_format format;
    bool expect_fail;
} scene_config;

static scene_config g_config[MAX_CONFIG];
static u32 g_config_count;

static const char *const k_cap_names[] = {"mesh_shaders", "ray_query", "rt_pipelines",
                                          "hardware_rt", "async_compute"};

static bool cap_present(const de_gpu_caps *c, const char *name) {
    if (strcmp(name, "mesh_shaders") == 0)
        return c->mesh_shaders;
    if (strcmp(name, "ray_query") == 0)
        return c->ray_query;
    if (strcmp(name, "rt_pipelines") == 0)
        return c->rt_pipelines;
    if (strcmp(name, "hardware_rt") == 0)
        return c->raytracing == DE_RT_HARDWARE;
    if (strcmp(name, "async_compute") == 0)
        return c->async_compute;
    return false;
}

static bool known_cap(const char *name) {
    for (size_t i = 0; i < sizeof k_cap_names / sizeof k_cap_names[0]; ++i)
        if (strcmp(name, k_cap_names[i]) == 0)
            return true;
    return false;
}

static bool config_error(const char *file, int line, const char *scene, const char *what) {
    fprintf(stderr, "de-golden: %s:%d: [scene.%s]: %s\n", file, line, scene, what);
    return false;
}

/* Copies a string array into `out` (cap entries of `len` bytes). */
static bool string_array(toml_datum_t v, char *out, u32 cap, size_t len, u32 *count) {
    if (v.type != TOML_ARRAY || v.u.arr.size < 0 || (u32)v.u.arr.size > cap)
        return false;
    *count = 0;
    for (i32 i = 0; i < v.u.arr.size; ++i) {
        const toml_datum_t e = v.u.arr.elem[i];
        if (e.type != TOML_STRING || (size_t)e.u.str.len >= len)
            return false;
        snprintf(out + (size_t)*count * len, len, "%s", e.u.s);
        ++*count;
    }
    return true;
}

static bool number(toml_datum_t v, double *out) {
    if (v.type == TOML_FP64)
        *out = v.u.fp64;
    else if (v.type == TOML_INT64)
        *out = (double)v.u.int64;
    else
        return false;
    return true;
}

static bool parse_scene(const char *file, const char *name, toml_datum_t t, scene_config *sc) {
    memset(sc, 0, sizeof *sc);
    if (strlen(name) >= sizeof sc->name)
        return config_error(file, t.lineno, name, "name too long");
    snprintf(sc->name, sizeof sc->name, "%s", name);
    snprintf(sc->reference, sizeof sc->reference, "%s", name);
    sc->width = sc->height = 512;
    sc->format = DE_FMT_RGBA8_UNORM;
    sc->threshold_mean = sc->threshold_p99 = -1.0;
    sc->backends[DE_GPU_D3D12] = sc->backends[DE_GPU_VULKAN] = sc->backends[DE_GPU_METAL] = true;
    snprintf(sc->tiers[0], sizeof sc->tiers[0], "base");
    sc->tier_count = 1;
    if (t.type != TOML_TABLE)
        return config_error(file, t.lineno, name, "not a table");
    for (i32 k = 0; k < t.u.tab.size; ++k) {
        const char *key = t.u.tab.key[k];
        const toml_datum_t v = t.u.tab.value[k];
        if (strcmp(key, "threshold_mean") == 0 || strcmp(key, "threshold_p99") == 0) {
            double *d = key[10] == 'm' ? &sc->threshold_mean : &sc->threshold_p99;
            if (!number(v, d) || *d < 0.0 || *d > 1.0)
                return config_error(file, v.lineno, name, "thresholds are numbers in [0, 1]");
        } else if (strcmp(key, "backends") == 0) {
            char b[4][16];
            u32 n = 0;
            if (!string_array(v, &b[0][0], 4, sizeof b[0], &n))
                return config_error(file, v.lineno, name, "backends: an array of names");
            memset(sc->backends, 0, sizeof sc->backends);
            for (u32 i = 0; i < n; ++i) {
                if (strcmp(b[i], "metal") == 0)
                    sc->backends[DE_GPU_METAL] = true;
                else if (strcmp(b[i], "vulkan") == 0)
                    sc->backends[DE_GPU_VULKAN] = true;
                else if (strcmp(b[i], "d3d12") == 0)
                    sc->backends[DE_GPU_D3D12] = true;
                else
                    return config_error(file, v.lineno, name, "backends: metal, vulkan or d3d12");
            }
        } else if (strcmp(key, "tiers") == 0) {
            if (!string_array(v, &sc->tiers[0][0], MAX_TIERS, sizeof sc->tiers[0],
                              &sc->tier_count) ||
                sc->tier_count == 0)
                return config_error(file, v.lineno, name, "tiers: an array of base and rt");
            for (u32 i = 0; i < sc->tier_count; ++i)
                if (strcmp(sc->tiers[i], "base") != 0 && strcmp(sc->tiers[i], "rt") != 0)
                    return config_error(file, v.lineno, name, "tiers: base or rt");
        } else if (strcmp(key, "requires") == 0) {
            if (!string_array(v, &sc->requires[0][0], MAX_CAPS, sizeof sc->requires[0],
                              &sc->require_count))
                return config_error(file, v.lineno, name, "requires: an array of cap names");
            for (u32 i = 0; i < sc->require_count; ++i)
                if (!known_cap(sc->requires[i]))
                    return config_error(file, v.lineno, name,
                                        "requires: mesh_shaders, ray_query, rt_pipelines, "
                                        "hardware_rt or async_compute");
        } else if (strcmp(key, "size") == 0) {
            if (v.type == TOML_INT64) {
                sc->width = sc->height = (u32)v.u.int64;
            } else if (v.type == TOML_ARRAY && v.u.arr.size == 2 &&
                       v.u.arr.elem[0].type == TOML_INT64 && v.u.arr.elem[1].type == TOML_INT64) {
                sc->width = (u32)v.u.arr.elem[0].u.int64;
                sc->height = (u32)v.u.arr.elem[1].u.int64;
            } else {
                return config_error(file, v.lineno, name, "size: N or [width, height]");
            }
            if (sc->width < 8 || sc->height < 8 || sc->width > 4096 || sc->height > 4096)
                return config_error(file, v.lineno, name, "size: 8 to 4096 pixels");
        } else if (strcmp(key, "format") == 0) {
            if (v.type != TOML_STRING)
                return config_error(file, v.lineno, name, "format: rgba8 or rgba16f");
            if (strcmp(v.u.s, "rgba8") == 0)
                sc->format = DE_FMT_RGBA8_UNORM;
            else if (strcmp(v.u.s, "rgba16f") == 0)
                sc->format = DE_FMT_RGBA16_FLOAT;
            else
                return config_error(file, v.lineno, name, "format: rgba8 or rgba16f");
        } else if (strcmp(key, "reference") == 0) {
            if (v.type != TOML_STRING || (size_t)v.u.str.len >= sizeof sc->reference)
                return config_error(file, v.lineno, name, "reference: a scene name");
            snprintf(sc->reference, sizeof sc->reference, "%s", v.u.s);
        } else if (strcmp(key, "expect_fail") == 0) {
            if (v.type != TOML_BOOLEAN)
                return config_error(file, v.lineno, name, "expect_fail: true or false");
            sc->expect_fail = v.u.boolean;
        } else if (strcmp(key, "description") == 0) {
            if (v.type != TOML_STRING)
                return config_error(file, v.lineno, name, "description: a string");
        } else {
            char msg[96];
            snprintf(msg, sizeof msg, "unknown key '%.40s'", key);
            return config_error(file, v.lineno, name, msg);
        }
    }
    if (sc->threshold_mean < 0.0 || sc->threshold_p99 < 0.0)
        return config_error(file, t.lineno, name, "threshold_mean and threshold_p99 are required");
    return true;
}

/* Loads scenes.toml and checks it against the registered scenes, both ways. */
static bool load_config(const char *path) {
    toml_result_t r = toml_parse_file_ex(path);
    if (!r.ok) {
        fprintf(stderr, "de-golden: %s: %s\n", path, r.errmsg);
        toml_free(r);
        return false;
    }
    bool ok = true;
    const toml_datum_t top = r.toptab;
    for (i32 k = 0; k < top.u.tab.size && ok; ++k) {
        if (strcmp(top.u.tab.key[k], "scene") != 0) {
            fprintf(stderr,
                    "de-golden: %s: unknown top-level key '%s' (scenes are [scene.<name>])\n", path,
                    top.u.tab.key[k]);
            ok = false;
            break;
        }
        const toml_datum_t scenes = top.u.tab.value[k];
        if (scenes.type != TOML_TABLE) {
            fprintf(stderr, "de-golden: %s: `scene` must be a table of tables\n", path);
            ok = false;
            break;
        }
        for (i32 s = 0; s < scenes.u.tab.size && ok; ++s) {
            if (g_config_count == MAX_CONFIG) {
                fprintf(stderr, "de-golden: %s: more than %u scenes\n", path, MAX_CONFIG);
                ok = false;
                break;
            }
            ok = parse_scene(path, scenes.u.tab.key[s], scenes.u.tab.value[s],
                             &g_config[g_config_count++]);
        }
    }
    toml_free(r);
    for (u32 i = 0; i < g_config_count && ok; ++i) {
        if (de_golden_scene_find(g_config[i].name) == NULL) {
            fprintf(stderr, "de-golden: %s: [scene.%s] has no registered scene (DE_GOLDEN_SCENE)\n",
                    path, g_config[i].name);
            ok = false;
        }
        bool ref_known = false;
        for (u32 j = 0; j < g_config_count; ++j)
            ref_known = ref_known || strcmp(g_config[j].name, g_config[i].reference) == 0;
        if (ok && !ref_known) {
            fprintf(stderr, "de-golden: %s: [scene.%s] reference '%s' is not a scene\n", path,
                    g_config[i].name, g_config[i].reference);
            ok = false;
        }
    }
    for (u32 i = 0; i < de_golden_scene_count() && ok; ++i) {
        const char *name = de_golden_scene_at(i)->name;
        bool found = false;
        for (u32 j = 0; j < g_config_count && !found; ++j)
            found = strcmp(g_config[j].name, name) == 0;
        if (!found) {
            fprintf(stderr, "de-golden: scene '%s' has no [scene.%s] table in %s\n", name, name,
                    path);
            ok = false;
        }
    }
    return ok;
}

static const scene_config *config_of(const char *name) {
    for (u32 i = 0; i < g_config_count; ++i)
        if (strcmp(g_config[i].name, name) == 0)
            return &g_config[i];
    return NULL;
}

/* ---------- Files (through the virtual file system: atomic writes) ---------- */

static void mkdir_native(const char *dir) {
    (void)MKDIR(dir); /* an existing directory is fine; a real error shows at the mount */
}

typedef struct png_buf {
    u8 *data;
    size_t size, cap;
    bool oom;
} png_buf;

static void png_append(void *ctx, void *data, int size) {
    png_buf *b = (png_buf *)ctx;
    if (b->oom || size <= 0)
        return;
    if (b->size + (size_t)size > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        while (cap < b->size + (size_t)size)
            cap *= 2;
        u8 *p = (u8 *)realloc(b->data, cap);
        if (p == NULL) {
            b->oom = true;
            return;
        }
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->size, data, (size_t)size);
    b->size += (size_t)size;
}

/* Writes an RGBA8 image as an RGB PNG to `root`:/<dir>/<file>. Alpha is dropped: FLIP does not
 * compare it, and the files stay smaller (docs/testing/golden.md: size budget). */
static bool write_png(const char *root, const char *dir, const char *file, const u8 *rgba, u32 w,
                      u32 h) {
    char vdir[512], vpath[640];
    snprintf(vdir, sizeof vdir, "%s:/%s", root, dir);
    snprintf(vpath, sizeof vpath, "%s/%s", vdir, file);
    png_buf b = {0};
    const size_t n = (size_t)w * h;
    u8 *rgb = (u8 *)malloc(n * 3u);
    for (size_t i = 0; rgb != NULL && i < n; ++i)
        memcpy(rgb + i * 3u, rgba + i * 4u, 3);
    stbi_write_png_compression_level = 9;
    const bool encoded =
        rgb != NULL &&
        stbi_write_png_to_func(png_append, &b, (int)w, (int)h, 3, rgb, (int)(w * 3u)) != 0 &&
        !b.oom;
    free(rgb);
    const bool ok = encoded && de_dir_create(vdir) == DE_OK &&
                    de_file_write_all_atomic(vpath, b.data, b.size) == DE_OK;
    if (!ok)
        fprintf(stderr, "de-golden: cannot write %s\n", vpath);
    free(b.data);
    return ok;
}

/* Reads an RGBA8 PNG; false when it does not exist or does not decode. */
static bool read_png(const char *vpath, de_golden_image *out) {
    memset(out, 0, sizeof *out);
    void *data = NULL;
    u64 size = 0;
    if (!de_path_exists(vpath) || de_file_read_all(vpath, NULL, &data, &size) != DE_OK)
        return false;
    int w = 0, h = 0, comp = 0;
    stbi_uc *px = stbi_load_from_memory((const stbi_uc *)data, (int)size, &w, &h, &comp, 4);
    DE_FREE(de_heap_allocator(), data, (size_t)size + 1u);
    if (px == NULL)
        return false;
    out->rgba = (u8 *)malloc((size_t)w * (size_t)h * 4u);
    if (out->rgba != NULL)
        memcpy(out->rgba, px, (size_t)w * (size_t)h * 4u);
    stbi_image_free(px);
    out->width = (u32)w;
    out->height = (u32)h;
    return out->rgba != NULL;
}

/* ---------- Device ---------- */

static u32 g_errors; /* log records of level ERROR or above */

static void count_errors(const de_log_record *rec, void *user) {
    (void)user;
    if (rec->level >= DE_LOG_ERROR)
        g_errors++;
}

static const char *error_chain(char *buf, size_t cap) {
    buf[0] = '\0';
    size_t n = 0;
    for (const de_error *e = de_error_last(); e != NULL && n + 1 < cap; e = e->cause) {
        int k = snprintf(buf + n, cap - n, "%s%s", n ? ": " : "", e->message);
        if (k < 0)
            break;
        n += (size_t)k;
    }
    return buf[0] ? buf : "?";
}

static bool is_warp(const de_gpu_caps *c) {
    return c->vendor_id == 0x1414u && c->device_id == 0x8cu; /* Microsoft Basic Render Driver */
}

/* Creates the device; returns 0, or the exit status (77 when the backend or WARP is missing). */
static int open_device(const options *o, de_gpu_device **out) {
    de_gpu_device_desc dd = {.backend = o->backend,
                             .adapter = o->adapter,
                             .debug_layer = GOLDEN_VALIDATION,
                             .pipeline_cache_dir = ""}; /* nothing persistent: fixed settings */
    char chain[400];
    if (!o->warp) {
        if (de_gpu_device_create(&dd, out) == DE_OK)
            return 0;
        printf("SKIPPED: no %s device: %s\n", de_gpu_backend_name(o->backend),
               error_chain(chain, sizeof chain));
        return EXIT_SKIP;
    }
    /* WARP: software adapters come last in the D3D12 backend's adapter list. */
    for (u32 n = 1; n < 64u; ++n) {
        dd.adapter = n;
        de_gpu_device *d = NULL;
        de_result r = de_gpu_device_create(&dd, &d);
        if (r == DE_ERR_INVALID_ARGUMENT || r == DE_ERR_UNSUPPORTED)
            break; /* past the end of the list, or no D3D12 */
        if (r != DE_OK)
            continue;
        if (is_warp(de_gpu_get_caps(d))) {
            *out = d;
            return 0;
        }
        de_gpu_device_destroy(d);
    }
    printf("SKIPPED: --warp: no WARP adapter for %s\n", de_gpu_backend_name(o->backend));
    return EXIT_SKIP;
}

/* ---------- One scene at one tier ---------- */

typedef enum result { R_PASS, R_FAIL, R_SKIP } result;

static result compare(const options *o, const scene_config *sc, const char *backend,
                      const char *tier, const de_golden_image *img) {
    char dir[256], ref_path[512];
    snprintf(dir, sizeof dir, "%s/%s", backend, tier);
    snprintf(ref_path, sizeof ref_path, "goldenref:/%s/%s.png", dir, sc->reference);
    if (o->update_refs) {
        if (strcmp(sc->reference, sc->name) != 0) {
            printf("KEPT     %s %s: uses the reference of %s\n", sc->name, dir, sc->reference);
            return R_PASS;
        }
        char file[96];
        snprintf(file, sizeof file, "%s.png", sc->name);
        if (!write_png("goldenref", dir, file, img->rgba, img->width, img->height))
            return R_FAIL;
        printf("UPDATED  %s %s: %s/%s/%s\n", sc->name, dir, o->refs, dir, file);
        return R_PASS;
    }
    de_golden_image ref;
    if (!read_png(ref_path, &ref)) {
        printf("FAIL     %s %s: no reference %s/%s/%s.png (check the image in --out, then copy it "
               "there or run --update-refs on this backend)\n",
               sc->name, dir, o->refs, dir, sc->reference);
        return R_FAIL;
    }
    result res = R_FAIL;
    if (ref.width != img->width || ref.height != img->height) {
        printf("FAIL     %s %s: the reference is %ux%u, the image %ux%u\n", sc->name, dir,
               ref.width, ref.height, img->width, img->height);
        de_golden_image_free(&ref);
        return R_FAIL;
    }
    const size_t n = (size_t)img->width * img->height;
    float *err = (float *)malloc(n * sizeof(float));
    de_flip_stats st;
    const de_flip_image a = {ref.rgba, ref.width, ref.height, 4, 0};
    const de_flip_image b = {img->rgba, img->width, img->height, 4, 0};
    if (err == NULL || !de_flip_ldr(&a, &b, 0.0f, err) || !de_flip_stats_compute(err, n, &st)) {
        printf("FAIL     %s %s: FLIP failed (out of memory)\n", sc->name, dir);
        free(err);
        de_golden_image_free(&ref);
        return R_FAIL;
    }
    const bool within = st.mean <= sc->threshold_mean && st.p99 <= sc->threshold_p99;
    bool heatmap = false;
    if (!within && o->out != NULL) {
        u8 *heat = (u8 *)malloc(n * 4u);
        char file[96];
        if (heat != NULL) {
            de_flip_heatmap(err, img->width, img->height, heat);
            snprintf(file, sizeof file, "%s.flip.png", sc->name);
            heatmap = write_png("goldenout", dir, file, heat, img->width, img->height);
            free(heat);
        }
        snprintf(file, sizeof file, "%s.ref.png", sc->name);
        (void)write_png("goldenout", dir, file, ref.rgba, ref.width, ref.height);
    }
    if (within && o->out != NULL) {
        /* A pass leaves no reference copy or heat map behind from an earlier failed run. */
        static const char *const stale[] = {"ref", "flip"};
        for (int i = 0; i < 2; ++i) {
            char vpath[640];
            snprintf(vpath, sizeof vpath, "goldenout:/%s/%s.%s.png", dir, sc->name, stale[i]);
            if (de_path_exists(vpath))
                (void)de_file_delete(vpath);
        }
    }
    const char *verdict;
    if (sc->expect_fail) {
        /* The self test: the perturbation must be caught, and the heat map written. */
        res = !within && (heatmap || o->out == NULL) ? R_PASS : R_FAIL;
        verdict = res == R_PASS ? "PASS     (failed as expected)" : "FAIL     (expected a failure)";
    } else {
        res = within ? R_PASS : R_FAIL;
        verdict = within ? "PASS    " : "FAIL    ";
    }
    printf("%s %s %s: mean %.6f (<= %.6f), p99 %.6f (<= %.6f), max %.6f%s%s\n", verdict, sc->name,
           dir, st.mean, sc->threshold_mean, (double)st.p99, sc->threshold_p99, (double)st.max,
           heatmap ? "; heat map: " : "", heatmap ? sc->name : "");
    free(err);
    de_golden_image_free(&ref);
    return res;
}

static result run_scene(const options *o, de_gpu_device *d, const de_golden_scene_entry *e,
                        const scene_config *sc, const de_gpu_tier_report *tr, const char *tier) {
    const de_gpu_caps *caps = de_gpu_get_caps(d);
    const char *backend = de_gpu_backend_name(caps->backend);
    if (!sc->backends[caps->backend]) {
        printf("SKIPPED  %s %s/%s: not listed for this backend in scenes.toml\n", e->name, backend,
               tier);
        return R_SKIP;
    }
    if ((strcmp(tier, "base") == 0 && !tr->base_ok) || (strcmp(tier, "rt") == 0 && !tr->rt_ok)) {
        printf("SKIPPED  %s %s/%s: the device is below the %s tier\n", e->name, backend, tier,
               tier);
        return R_SKIP;
    }
    for (u32 i = 0; i < sc->require_count; ++i) {
        if (!cap_present(caps, sc->requires[i])) {
            printf("SKIPPED  %s %s/%s: missing cap %s\n", e->name, backend, tier, sc->requires[i]);
            return R_SKIP;
        }
    }
    de_log_flush();
    const u32 errors_before = g_errors;
    de_golden_render_desc rd = {.device = d,
                                .scene_name = e->name,
                                .fn = e->fn,
                                .tier = tier,
                                .format = sc->format,
                                .width = sc->width,
                                .height = sc->height};
    de_golden_image img;
    char msg[512];
    de_golden_outcome oc = de_golden_render(&rd, &img, msg, sizeof msg);
    de_log_flush(); /* every record of the run has reached count_errors */
    const u32 errors = g_errors - errors_before;
    char dir[256];
    snprintf(dir, sizeof dir, "%s/%s", backend, tier);
    if (img.rgba != NULL && o->out != NULL) {
        char file[96];
        snprintf(file, sizeof file, "%s.png", e->name);
        (void)write_png("goldenout", dir, file, img.rgba, img.width, img.height);
    }
    result res;
    if (oc == DE_GOLDEN_SKIPPED) {
        printf("SKIPPED  %s %s: %s\n", e->name, dir, msg);
        res = R_SKIP;
    } else if (oc == DE_GOLDEN_FAILED) {
        printf("FAIL     %s %s: %s\n", e->name, dir, msg);
        res = R_FAIL;
    } else if (errors != 0) {
        printf("FAIL     %s %s: %u error(s) were logged during the scene (validation?)\n", e->name,
               dir, errors);
        res = R_FAIL;
    } else {
        res = compare(o, sc, backend, tier, &img);
    }
    de_golden_image_free(&img);
    return res;
}

/* ---------- Main ---------- */

static int run(const options *o) {
    char msg[256];
    if (!de_golden_registry_ok(msg, sizeof msg)) {
        fprintf(stderr, "de-golden: %s\n", msg);
        return 2;
    }
    if (!load_config(o->config))
        return 2;
    if (o->list) {
        for (u32 i = 0; i < de_golden_scene_count(); ++i)
            printf("%s\n", de_golden_scene_at(i)->name);
        return 0;
    }
    if (o->update_refs) {
        const char *ci = getenv("CI");
        if (ci != NULL && ci[0] != '\0') {
            fprintf(stderr, "de-golden: --update-refs refused: CI is set. References are written "
                            "on a developer machine and reviewed; take a CI image from the "
                            "job's artifact instead.\n");
            return 2;
        }
    }
    if (o->warp && o->backend != DE_GPU_D3D12) {
        fprintf(stderr, "de-golden: --warp needs --backend d3d12\n");
        return 2;
    }
    /* Virtual roots: goldenref:/ the references (writable for --update-refs), goldenout:/ the
     * output directory. */
    if (o->update_refs)
        mkdir_native(o->refs);
    if (de_fs_mount("goldenref", o->refs, o->update_refs ? DE_MOUNT_WRITABLE : 0, 0) != DE_OK) {
        if (o->update_refs) {
            fprintf(stderr, "de-golden: cannot use the reference directory %s\n", o->refs);
            return 2;
        }
        /* No references at all yet: every comparison fails with "no reference". */
        printf("de-golden: warning: no reference directory %s\n", o->refs);
    }
    if (o->out != NULL) {
        mkdir_native(o->out);
        if (de_fs_mount("goldenout", o->out, DE_MOUNT_WRITABLE, 0) != DE_OK) {
            fprintf(stderr, "de-golden: cannot use the output directory %s\n", o->out);
            return 2;
        }
    }
    u32 selected = 0;
    for (u32 i = 0; i < de_golden_scene_count(); ++i)
        selected += glob_match(o->scene_glob, de_golden_scene_at(i)->name);
    if (selected == 0) {
        fprintf(stderr, "de-golden: no scene matches '%s' (--list shows them)\n", o->scene_glob);
        return 2;
    }

    de_gpu_device *d = NULL;
    int rc = open_device(o, &d);
    if (rc != 0)
        return rc;
    const de_gpu_caps *caps = de_gpu_get_caps(d);
    de_gpu_tier_report tr;
    de_gpu_evaluate_tier(caps, &tr);
    printf("de-golden: %s on %s; base tier %s%s%s, rt tier %s, mesh shaders %s\n",
           de_gpu_backend_name(caps->backend), caps->device_name, tr.base_ok ? "yes" : "NO",
           tr.base_ok ? "" : ": ",
           tr.base_ok ? "" : (tr.base_reason_count ? tr.base_reasons[0] : "?"),
           tr.rt_ok ? "yes" : "no", caps->mesh_shaders ? "yes" : "no");

    u32 pass = 0, fail = 0, skip = 0;
    for (u32 i = 0; i < de_golden_scene_count(); ++i) {
        const de_golden_scene_entry *e = de_golden_scene_at(i);
        if (!glob_match(o->scene_glob, e->name))
            continue;
        const scene_config *sc = config_of(e->name);
        for (u32 t = 0; t < sc->tier_count; ++t) {
            if (o->tier != NULL && strcmp(o->tier, sc->tiers[t]) != 0)
                continue;
            switch (run_scene(o, d, e, sc, &tr, sc->tiers[t])) {
            case R_PASS:
                ++pass;
                break;
            case R_FAIL:
                ++fail;
                break;
            case R_SKIP:
                ++skip;
                break;
            }
        }
    }
    de_gpu_device_destroy(d);
    printf("de-golden: %u passed, %u failed, %u skipped\n", pass, fail, skip);
    return fail != 0 ? 1 : pass == 0 ? EXIT_SKIP : 0;
}

int main(int argc, char **argv) {
    options o;
    memset(&o, 0, sizeof o);
    int rc = parse(argc, argv, &o);
    if (rc != 0)
        return rc;
    (void)de_log_init(NULL);
    de_log_add_sink(count_errors, NULL);
    rc = run(&o);
    de_log_flush();
    de_golden_shutdown();
    de_fs_shutdown();
    de_log_shutdown();
    return rc;
}
