/*
 * main.c — `de-caps`, the GPU capability probe (DENG-290; SPEC §3.3, §12.1, §12.8, §24 R5/R7).
 *
 * For every GPU backend compiled into this build (Metal + Vulkan on macOS, D3D12 + Vulkan on
 * Windows, Vulkan on Linux; the null backend only on request) and every adapter of it, creates a
 * device, reads its de_gpu_caps, runs de_gpu_evaluate_tier() and prints both: a summary table and
 * one block per adapter, or JSON with --json (the format of tools/caps/caps_schema.json).
 *
 * Adapters are probed through de_gpu_device_desc.adapter = 1, 2, ... until the backend says the
 * index is past the last adapter (DE_ERR_NOT_FOUND or DE_ERR_INVALID_ARGUMENT). A forced adapter
 * is created even when it is below the Base tier, so its caps and the reason are reported. Then
 * adapter 0 is created once to find the adapter the engine picks by default (Vulkan refuses
 * adapter 0 when no device meets the Base tier; that is reported as "no default").
 *
 * A backend that cannot run here (no Vulkan loader, no driver, no adapter) is a line in the report,
 * not an error: the exit code is 0 whenever the report was written. 2 = bad arguments, 1 = the
 * report could not be written.
 */
#include <dengine/de_gpu.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The executable exports D3D12SDKVersion / D3D12SDKPath so D3D12 runs on the Agility runtime that
 * CMake copies next to it (src/gpu/d3d12/d3d12_agility_exports.h). */
#if defined(_WIN32) && defined(DE_HAS_GPU_D3D12)
#include "gpu/d3d12/d3d12_agility_exports.h"
#endif

#define CAPS_MAX_ADAPTERS 16u
#define CAPS_SCHEMA       "dengine.caps/1"

typedef struct adapter_probe {
    u32 index; /* 1-based, as de_gpu_device_desc.adapter */
    bool ok;
    de_gpu_caps caps;
    de_gpu_tier_report tier;
    char error[DE_ERROR_MESSAGE_MAX];
} adapter_probe;

typedef struct backend_probe {
    de_gpu_backend backend;
    const char *status; /* "ok", "not built", "no loader", "no device", "unavailable" */
    char error[DE_ERROR_MESSAGE_MAX];
    u32 default_adapter; /* index of the adapter that adapter 0 picks; 0 = none */
    char default_error[DE_ERROR_MESSAGE_MAX];
    u32 adapter_count;
    adapter_probe adapters[CAPS_MAX_ADAPTERS];
} backend_probe;

typedef struct caps_options {
    bool json;
    bool verbose;
    bool backend_set;
    de_gpu_backend backend; /* when backend_set */
    u32 adapter;            /* 0 = every adapter */
} caps_options;

/* ---------- Errors ---------- */

/* The innermost cause of the thread's last error: the backend's own message. */
static void last_error_text(char *out, size_t cap) {
    const de_error *e = de_error_last();
    while (e && e->cause)
        e = e->cause;
    snprintf(out, cap, "%s", e ? e->message : "unknown error");
}

/* A backend-level failure (nothing to probe on this machine), or NULL for an adapter-level one. */
static const char *backend_status_from_error(const char *msg) {
    if (strstr(msg, "not compiled into this build"))
        return "not built";
    if (strstr(msg, "no Vulkan loader found"))
        return "no loader";
    if (strstr(msg, "lists no device") || strstr(msg, "no Metal device") ||
        strstr(msg, "lists no adapter"))
        return "no device";
    return NULL;
}

/* ---------- Probing ---------- */

static de_result create_device(de_gpu_backend b, u32 adapter, de_gpu_device **out) {
    de_error_clear();
    de_gpu_device_desc desc = {.backend = b, .adapter = adapter};
    return de_gpu_device_create(&desc, out);
}

static bool same_device(const de_gpu_caps *a, const de_gpu_caps *b) {
    return a->vendor_id == b->vendor_id && a->device_id == b->device_id &&
           a->dedicated_vram == b->dedicated_vram && strcmp(a->device_name, b->device_name) == 0;
}

static void probe_backend(backend_probe *bp, de_gpu_backend b, u32 only_adapter) {
    memset(bp, 0, sizeof *bp);
    bp->backend = b;
    bp->status = "ok";

    /* The null backend ignores the adapter index: it has exactly one "adapter". */
    const u32 first = b == DE_GPU_NULL ? 1u : (only_adapter ? only_adapter : 1u);
    const u32 last = b == DE_GPU_NULL ? 1u : (only_adapter ? only_adapter : CAPS_MAX_ADAPTERS);
    if (b == DE_GPU_NULL && only_adapter > 1) {
        bp->status = "no device";
        snprintf(bp->error, sizeof bp->error, "null: adapter %u requested; the null backend has 1",
                 only_adapter);
        return;
    }

    for (u32 n = first; n <= last; ++n) {
        de_gpu_device *d = NULL;
        de_result r = create_device(b, b == DE_GPU_NULL ? 0u : n, &d);
        if (r == DE_OK) {
            adapter_probe *ap = &bp->adapters[bp->adapter_count++];
            ap->index = n;
            ap->ok = true;
            ap->caps = *de_gpu_get_caps(d);
            de_gpu_evaluate_tier(&ap->caps, &ap->tier);
            de_gpu_device_destroy(d);
            continue;
        }
        char msg[DE_ERROR_MESSAGE_MAX];
        last_error_text(msg, sizeof msg);
        const char *status = backend_status_from_error(msg);
        const bool past_end = r == DE_ERR_NOT_FOUND || r == DE_ERR_INVALID_ARGUMENT;
        if (status || (past_end && bp->adapter_count == 0 && n == first && !only_adapter)) {
            /* Nothing to probe on this machine (or not one adapter). */
            bp->status = status ? status : "no device";
            snprintf(bp->error, sizeof bp->error, "%s", msg);
            bp->adapter_count = 0;
            return;
        }
        if (past_end && !only_adapter)
            break; /* the end of the adapter list */
        /* This adapter fails (for example a Vulkan device below 1.3): report it, try the next.
         * The same failure twice in a row is a backend problem, not an adapter's. */
        if (bp->adapter_count > 0 && !bp->adapters[bp->adapter_count - 1].ok &&
            strcmp(bp->adapters[bp->adapter_count - 1].error, msg) == 0) {
            bool all_failed = true;
            for (u32 i = 0; i < bp->adapter_count; ++i)
                all_failed = all_failed && !bp->adapters[i].ok;
            if (all_failed) {
                bp->status = "unavailable";
                snprintf(bp->error, sizeof bp->error, "%s", msg);
                bp->adapter_count = 0;
                return;
            }
            break;
        }
        adapter_probe *ap = &bp->adapters[bp->adapter_count++];
        ap->index = n;
        ap->ok = false;
        snprintf(ap->error, sizeof ap->error, "%s", msg);
        if (only_adapter)
            break;
    }

    /* The adapter that (de_gpu_device_desc){.backend = b} picks. */
    bool any_ok = false;
    for (u32 i = 0; i < bp->adapter_count; ++i)
        any_ok = any_ok || bp->adapters[i].ok;
    if (!any_ok)
        return;
    if (b == DE_GPU_NULL) {
        bp->default_adapter = 1;
        return;
    }
    de_gpu_device *d = NULL;
    if (create_device(b, 0, &d) == DE_OK) {
        const de_gpu_caps *c = de_gpu_get_caps(d);
        for (u32 i = 0; i < bp->adapter_count; ++i)
            if (bp->adapters[i].ok && same_device(&bp->adapters[i].caps, c)) {
                bp->default_adapter = bp->adapters[i].index;
                break;
            }
        if (!bp->default_adapter)
            snprintf(bp->default_error, sizeof bp->default_error,
                     "the default device \"%s\" is not among the adapters probed", c->device_name);
        de_gpu_device_destroy(d);
    } else {
        last_error_text(bp->default_error, sizeof bp->default_error);
    }
}

/* ---------- Names and formatting ---------- */

static const char *rt_name(de_rt_support rt) {
    switch (rt) {
    case DE_RT_NONE:
        return "none";
    case DE_RT_API_ONLY:
        return "api_only";
    case DE_RT_HARDWARE:
        return "hardware";
    }
    return "unknown";
}

static const char *vendor_name(u32 id) {
    switch (id) {
    case 0x10DEu:
        return "NVIDIA";
    case 0x1002u:
    case 0x1022u:
        return "AMD";
    case 0x8086u:
        return "Intel";
    case 0x106Bu:
        return "Apple";
    case 0x1414u:
        return "Microsoft";
    case 0x5143u:
        return "Qualcomm";
    case 0x13B5u:
        return "ARM";
    case 0x10005u:
        return "Mesa";
    default:
        return "unknown";
    }
}

static const char *platform_name(void) {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "other";
#endif
}

static const char *yes_no(bool b) {
    return b ? "yes" : "no";
}

/* "48.0 GiB", "512 MiB", "0 B". */
static void format_bytes(char *out, size_t cap, u64 bytes) {
    const double gib = 1024.0 * 1024.0 * 1024.0;
    if (bytes >= (u64)1 << 30)
        snprintf(out, cap, "%.1f GiB", (double)bytes / gib);
    else if (bytes >= (u64)1 << 20)
        snprintf(out, cap, "%llu MiB", (unsigned long long)(bytes >> 20));
    else
        snprintf(out, cap, "%llu B", (unsigned long long)bytes);
}

/* ---------- Text report ---------- */

static void text_reasons(FILE *f, const char *label, bool ok,
                         const char (*reasons)[DE_GPU_TIER_REASON_BYTES], u32 n) {
    fprintf(f, "    %-22s %s", label, ok ? "yes" : "no");
    for (u32 i = 0; i < n; ++i)
        fprintf(f, "%s%s", i == 0 ? ": " : "; ", reasons[i]);
    fputc('\n', f);
}

static void text_adapter(FILE *f, const backend_probe *bp, const adapter_probe *ap) {
    const char *bn = de_gpu_backend_name(bp->backend);
    if (!ap->ok) {
        fprintf(f, "\n%s adapter %u: cannot create a device\n    %s\n", bn, ap->index, ap->error);
        return;
    }
    const de_gpu_caps *c = &ap->caps;
    const de_gpu_tier_report *t = &ap->tier;
    char vram[32], shared[32];
    format_bytes(vram, sizeof vram, c->dedicated_vram);
    format_bytes(shared, sizeof shared, c->shared_memory);
    fprintf(f, "\n%s adapter %u: %s%s\n", bn, ap->index, c->device_name,
            bp->default_adapter == ap->index ? " (default)" : "");
    fprintf(f, "    %-22s %s 0x%04X / 0x%08X\n", "vendor / device id", vendor_name(c->vendor_id),
            c->vendor_id, c->device_id);
    fprintf(f, "    %-22s %s dedicated, %s shared%s\n", "memory", vram, shared,
            c->unified_memory ? ", unified" : "");
    fprintf(f, "    %-22s %s; ray queries %s, pipelines %s, opacity micromaps %s\n", "ray tracing",
            rt_name(c->raytracing), yes_no(c->ray_query), yes_no(c->rt_pipelines),
            yes_no(c->opacity_micromaps));
    if (c->mesh_shaders)
        fprintf(f, "    %-22s yes (%u vertices, %u primitives per group)\n", "mesh shaders",
                c->mesh_max_output_vertices, c->mesh_max_output_primitives);
    else
        fprintf(f, "    %-22s no\n", "mesh shaders");
    fprintf(f, "    %-22s %u .. %u\n", "wave size", c->wave_size_min, c->wave_size_max);
    fprintf(f, "    %-22s %u textures, %u storage textures, %u buffers, %u samplers, %u accels\n",
            "bindless tables", c->max_bindless_textures, c->max_bindless_storage_textures,
            c->max_bindless_buffers, c->max_bindless_samplers, c->max_bindless_accels);
    fprintf(f, "    %-22s %u bytes\n", "push constants", c->max_push_constant_bytes);
    fprintf(f, "    %-22s %llu Hz\n", "timestamps", (unsigned long long)c->timestamp_frequency);
    fprintf(f, "    %-22s HDR output %s, VRR %s, enhanced barriers %s, async compute %s\n",
            "features", yes_no(c->hdr_output), yes_no(c->vrr), yes_no(c->enhanced_barriers),
            yes_no(c->async_compute));
    text_reasons(f, "Base tier", t->base_ok, t->base_reasons, t->base_reason_count);
    text_reasons(f, "RT tier", t->rt_ok, t->rt_reasons, t->rt_reason_count);
    fprintf(f, "    %-22s %s%s%s\n", "recommended", de_gpu_tier_name(t->recommended),
            t->recommended_reason[0] ? ": " : "", t->recommended_reason);
}

static void text_report(FILE *f, const backend_probe *bps, u32 count) {
    fprintf(f, "de-caps %d.%d.%d (%s): GPU capabilities and tiers (SPEC 3.3)\n\n", DE_VERSION_MAJOR,
            DE_VERSION_MINOR, DE_VERSION_PATCH, platform_name());
    fprintf(f, "%-8s %-8s %-36s %-5s %-5s %-5s %s\n", "backend", "adapter", "device", "base", "rt",
            "mesh", "recommended");
    for (u32 i = 0; i < count; ++i) {
        const backend_probe *bp = &bps[i];
        const char *bn = de_gpu_backend_name(bp->backend);
        if (bp->adapter_count == 0) {
            fprintf(f, "%-8s %-8s %s\n", bn, "-", bp->status);
            continue;
        }
        for (u32 a = 0; a < bp->adapter_count; ++a) {
            const adapter_probe *ap = &bp->adapters[a];
            char idx[16];
            snprintf(idx, sizeof idx, "%u%s", ap->index,
                     bp->default_adapter == ap->index ? "*" : "");
            if (!ap->ok) {
                fprintf(f, "%-8s %-8s %s\n", bn, idx, "cannot create a device");
                continue;
            }
            fprintf(f, "%-8s %-8s %-36.36s %-5s %-5s %-5s %s\n", bn, idx, ap->caps.device_name,
                    yes_no(ap->tier.base_ok), yes_no(ap->tier.rt_ok), yes_no(ap->tier.mesh_shaders),
                    de_gpu_tier_name(ap->tier.recommended));
        }
    }
    fprintf(f, "(* = the adapter the engine picks by default)\n");
    for (u32 i = 0; i < count; ++i) {
        const backend_probe *bp = &bps[i];
        if (bp->adapter_count == 0) {
            fprintf(f, "\n%s: %s\n    %s\n", de_gpu_backend_name(bp->backend), bp->status,
                    bp->error);
            continue;
        }
        if (!bp->default_adapter && bp->default_error[0])
            fprintf(f, "\n%s: no default adapter\n    %s\n", de_gpu_backend_name(bp->backend),
                    bp->default_error);
        for (u32 a = 0; a < bp->adapter_count; ++a)
            text_adapter(f, bp, &bp->adapters[a]);
    }
}

/* ---------- JSON report ---------- */

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
        case '"':
            fputs("\\\"", f);
            break;
        case '\\':
            fputs("\\\\", f);
            break;
        case '\n':
            fputs("\\n", f);
            break;
        case '\r':
            fputs("\\r", f);
            break;
        case '\t':
            fputs("\\t", f);
            break;
        default:
            if (*p < 0x20)
                fprintf(f, "\\u%04x", *p);
            else
                fputc(*p, f);
        }
    }
    fputc('"', f);
}

/* A string, or null when it is empty. */
static void json_str_or_null(FILE *f, const char *s) {
    if (s && s[0])
        json_str(f, s);
    else
        fputs("null", f);
}

/* Separators after a value: the next key on the same line, or on the next line of an object. */
#define JSON_SP ", "
#define JSON_NL ",\n          "

static void json_bool(FILE *f, const char *key, bool v, const char *sep) {
    fprintf(f, "\"%s\": %s%s", key, v ? "true" : "false", sep);
}

static void json_u64(FILE *f, const char *key, u64 v, const char *sep) {
    fprintf(f, "\"%s\": %llu%s", key, (unsigned long long)v, sep);
}

static void json_reasons(FILE *f, const char *key, const char (*reasons)[DE_GPU_TIER_REASON_BYTES],
                         u32 n) {
    fprintf(f, "\"%s\": [", key);
    for (u32 i = 0; i < n; ++i) {
        if (i)
            fputs(", ", f);
        json_str(f, reasons[i]);
    }
    fputs("]", f);
}

static void json_caps(FILE *f, const de_gpu_caps *c) {
    fputs("{\"backend\": ", f);
    json_str(f, de_gpu_backend_name(c->backend));
    fputs(", \"device_name\": ", f);
    json_str(f, c->device_name);
    fputs(", \"vendor\": ", f);
    json_str(f, vendor_name(c->vendor_id));
    fputs(",\n          ", f);
    json_u64(f, "vendor_id", c->vendor_id, JSON_SP);
    json_u64(f, "device_id", c->device_id, JSON_SP);
    json_u64(f, "dedicated_vram", c->dedicated_vram, JSON_SP);
    json_u64(f, "shared_memory", c->shared_memory, JSON_SP);
    json_bool(f, "unified_memory", c->unified_memory, JSON_NL);
    fputs("\"raytracing\": ", f);
    json_str(f, rt_name(c->raytracing));
    fputs(", ", f);
    json_bool(f, "ray_query", c->ray_query, JSON_SP);
    json_bool(f, "rt_pipelines", c->rt_pipelines, JSON_SP);
    json_bool(f, "opacity_micromaps", c->opacity_micromaps, JSON_SP);
    json_bool(f, "mesh_shaders", c->mesh_shaders, JSON_SP);
    json_u64(f, "mesh_max_output_vertices", c->mesh_max_output_vertices, JSON_SP);
    json_u64(f, "mesh_max_output_primitives", c->mesh_max_output_primitives, JSON_NL);
    json_bool(f, "hdr_output", c->hdr_output, JSON_SP);
    json_bool(f, "vrr", c->vrr, JSON_SP);
    json_bool(f, "enhanced_barriers", c->enhanced_barriers, JSON_SP);
    json_bool(f, "async_compute", c->async_compute, JSON_SP);
    json_u64(f, "wave_size_min", c->wave_size_min, JSON_SP);
    json_u64(f, "wave_size_max", c->wave_size_max, JSON_NL);
    json_u64(f, "max_bindless_textures", c->max_bindless_textures, JSON_SP);
    json_u64(f, "max_bindless_buffers", c->max_bindless_buffers, JSON_SP);
    json_u64(f, "max_bindless_storage_textures", c->max_bindless_storage_textures, JSON_SP);
    json_u64(f, "max_bindless_samplers", c->max_bindless_samplers, JSON_SP);
    json_u64(f, "max_bindless_accels", c->max_bindless_accels, JSON_NL);
    json_u64(f, "max_push_constant_bytes", c->max_push_constant_bytes, JSON_SP);
    json_u64(f, "timestamp_frequency", c->timestamp_frequency, JSON_SP);
    json_bool(f, "base_tier_ok", c->base_tier_ok, JSON_SP);
    fputs("\"base_tier_reason\": ", f);
    json_str(f, c->base_tier_reason);
    fputs("}", f);
}

static void json_tier(FILE *f, const de_gpu_tier_report *t) {
    fputs("{", f);
    json_bool(f, "base_ok", t->base_ok, JSON_SP);
    json_reasons(f, "base_reasons", t->base_reasons, t->base_reason_count);
    fputs(",\n          ", f);
    json_bool(f, "rt_ok", t->rt_ok, JSON_SP);
    json_reasons(f, "rt_reasons", t->rt_reasons, t->rt_reason_count);
    fputs(",\n          ", f);
    json_bool(f, "mesh_shaders", t->mesh_shaders, JSON_SP);
    fputs("\"recommended\": ", f);
    json_str(f, de_gpu_tier_name(t->recommended));
    fputs(", \"recommended_reason\": ", f);
    json_str_or_null(f, t->recommended_reason);
    fputs(", \"default_base_device\": ", f);
    json_str_or_null(f, t->default_base_device);
    fputs("}", f);
}

static void json_report(FILE *f, const backend_probe *bps, u32 count) {
    fprintf(f, "{\"schema\": \"%s\", \"engine_version\": \"%d.%d.%d\", \"platform\": \"%s\",\n",
            CAPS_SCHEMA, DE_VERSION_MAJOR, DE_VERSION_MINOR, DE_VERSION_PATCH, platform_name());
    fputs(" \"backends\": [", f);
    for (u32 i = 0; i < count; ++i) {
        const backend_probe *bp = &bps[i];
        fprintf(f, "%s\n  {\"backend\": \"%s\", \"status\": \"%s\", \"error\": ", i ? "," : "",
                de_gpu_backend_name(bp->backend), bp->status);
        json_str_or_null(f, bp->error);
        fputs(", \"default_adapter\": ", f);
        if (bp->default_adapter)
            fprintf(f, "%u", bp->default_adapter);
        else
            fputs("null", f);
        fputs(", \"default_error\": ", f);
        json_str_or_null(f, bp->default_error);
        fputs(",\n   \"adapters\": [", f);
        for (u32 a = 0; a < bp->adapter_count; ++a) {
            const adapter_probe *ap = &bp->adapters[a];
            fprintf(f, "%s\n    {\"index\": %u, \"status\": \"%s\", ", a ? "," : "", ap->index,
                    ap->ok ? "ok" : "error");
            json_bool(f, "default", bp->default_adapter == ap->index, JSON_SP);
            fputs("\"error\": ", f);
            json_str_or_null(f, ap->ok ? NULL : ap->error);
            fputs(",\n        \"caps\": ", f);
            if (ap->ok)
                json_caps(f, &ap->caps);
            else
                fputs("null", f);
            fputs(",\n        \"tier\": ", f);
            if (ap->ok)
                json_tier(f, &ap->tier);
            else
                fputs("null", f);
            fputs("}", f);
        }
        fputs(bp->adapter_count ? "\n   ]}" : "]}", f);
    }
    fputs("\n ]\n}\n", f);
}

/* ---------- Command line ---------- */

static void usage(FILE *f) {
    fputs("usage: de-caps [--json] [--backend <name>] [--adapter <n>] [--verbose]\n"
          "\n"
          "Prints the GPU capabilities (de_gpu_caps) and the Base / RT tier decision\n"
          "(de_gpu_evaluate_tier) of every adapter of every GPU backend in this build.\n"
          "\n"
          "  --json            machine-readable output (schema dengine.caps/1)\n"
          "  --backend <name>  d3d12, vulkan, metal, null, default (the platform's), or all\n"
          "                    (every backend in the build, and null); default: every backend\n"
          "                    in the build except null\n"
          "  --adapter <n>     only adapter n (1-based, as de_gpu_device_desc.adapter)\n"
          "  --verbose         show the gpu log (device creation messages) on stderr\n"
          "\n"
          "Exit code 0 when the report was written (whatever it says), 2 for bad arguments.\n",
          f);
}

static bool parse_backend(const char *s, de_gpu_backend *out, bool *all) {
    *all = false;
    if (strcmp(s, "all") == 0) {
        *all = true;
        return true;
    }
    if (strcmp(s, "default") == 0) {
        *out = de_gpu_platform_default_backend();
        return true;
    }
    for (u32 b = DE_GPU_D3D12; b < DE_GPU_BACKEND_COUNT_; ++b)
        if (strcmp(s, de_gpu_backend_name((de_gpu_backend)b)) == 0) {
            *out = (de_gpu_backend)b;
            return true;
        }
    return false;
}

static int fail_usage(const char *fmt, ...) DE_PRINTF_FMT(1, 2);
static int fail_usage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("de-caps: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputs("\n\n", stderr);
    va_end(ap);
    usage(stderr);
    return 2;
}

int main(int argc, char **argv) {
    caps_options opt = {0};
    bool all = false;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--json") == 0) {
            opt.json = true;
        } else if (strcmp(a, "--verbose") == 0 || strcmp(a, "-v") == 0) {
            opt.verbose = true;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "--backend") == 0) {
            if (++i >= argc)
                return fail_usage("--backend needs a value");
            if (!parse_backend(argv[i], &opt.backend, &all))
                return fail_usage("unknown backend '%s'", argv[i]);
            opt.backend_set = !all;
        } else if (strcmp(a, "--adapter") == 0) {
            if (++i >= argc)
                return fail_usage("--adapter needs a value");
            char *end = NULL;
            unsigned long n = strtoul(argv[i], &end, 10);
            if (!end || *end || n < 1 || n > CAPS_MAX_ADAPTERS)
                return fail_usage("--adapter takes 1 .. %u (got '%s')", CAPS_MAX_ADAPTERS, argv[i]);
            opt.adapter = (u32)n;
        } else {
            return fail_usage("unknown argument '%s'", a);
        }
    }

    /* Device creation logs to `gpu` (and warns below the Base tier); the report says it all. */
    de_log_set_level(de_log_register_category("gpu", DE_LOG_INFO),
                     opt.verbose ? DE_LOG_INFO : DE_LOG_FATAL);

    de_gpu_backend list[DE_GPU_BACKEND_COUNT_];
    u32 count = 0;
    if (opt.backend_set) {
        list[count++] = opt.backend;
    } else {
#ifdef DE_HAS_GPU_D3D12
        list[count++] = DE_GPU_D3D12;
#endif
#ifdef DE_HAS_GPU_VULKAN
        list[count++] = DE_GPU_VULKAN;
#endif
#ifdef DE_HAS_GPU_METAL
        list[count++] = DE_GPU_METAL;
#endif
        if (all)
            list[count++] = DE_GPU_NULL;
    }

    backend_probe *probes = (backend_probe *)calloc(count ? count : 1u, sizeof *probes);
    if (!probes) {
        fputs("de-caps: out of memory\n", stderr);
        return 1;
    }
    for (u32 i = 0; i < count; ++i)
        probe_backend(&probes[i], list[i], opt.adapter);

    if (opt.json)
        json_report(stdout, probes, count);
    else
        text_report(stdout, probes, count);
    free(probes);
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fputs("de-caps: cannot write the report\n", stderr);
        return 1;
    }
    return 0;
}
