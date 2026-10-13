/* tier.c — the Base / RT tier decision over de_gpu_caps (DENG-290; SPEC §3.3, §12.8, §24).
 *
 * A pure function: the backends do the platform-specific Base-tier check when they fill the caps
 * (shader model 6.6 and binding tier 3 on D3D12, the Vulkan 1.3 features and bindless limits,
 * Metal 3 / Apple7 / argument buffers tier 2), because only they can see those features. This file
 * adds what the renderer needs from every backend and can read from the caps, the RT rule, and the
 * default-to-Base list (R7). The renderer (S12) and tools/caps (`de-caps`) call it.
 */
#include <dengine/de_gpu.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------- The default-to-Base list (SPEC §3.3, R7; DENG-2 Q4) ----------
 *
 * Devices that meet the RT tier but are too slow for it at the DENG-2 Q4 targets (Steam Deck: Base
 * tier at 30 fps), so they default to Base. Extend it from the tested-hardware list (Trama page
 * "Tested Hardware"). An entry matches on the PCI vendor id, plus the device id when it is not 0,
 * plus a device-name prefix when it is not NULL; the prefix must end at the end of the name or at
 * a space, so "Apple M1" matches "Apple M1 Pro" but not a future "Apple M10". */
typedef struct de__gpu_default_base_entry {
    u32 vendor_id;
    u32 device_id;           /* 0 = any */
    const char *name_prefix; /* NULL = any */
    const char *label;
    const char *why;
} de__gpu_default_base_entry;

#define DE__GPU_VENDOR_AMD   0x1002u
#define DE__GPU_VENDOR_APPLE 0x106Bu

static const de__gpu_default_base_entry k_default_base[] = {
    /* Steam Deck LCD (Van Gogh) and OLED (Sephiroth) APUs: RDNA2 ray queries, Base for speed. */
    {DE__GPU_VENDOR_AMD, 0x163Fu, NULL, "Steam Deck",
     "Steam Deck defaults to the Base tier (DENG-2 Q4: Base tier at 30 fps)"},
    {DE__GPU_VENDOR_AMD, 0x1435u, NULL, "Steam Deck OLED",
     "Steam Deck defaults to the Base tier (DENG-2 Q4: Base tier at 30 fps)"},
    /* Apple GPU family 7/8 (M1, M2): Metal ray tracing without hardware acceleration (the Metal
     * backend reports DE_RT_API_ONLY); listed so a backend that calls it hardware (MoltenVK) still
     * defaults to Base. Apple9 (M3) and later accelerate it. */
    {DE__GPU_VENDOR_APPLE, 0, "Apple M1", "Apple M1",
     "Apple M1/M2 (below Apple GPU family 9) default to the Base tier (SPEC R7)"},
    {DE__GPU_VENDOR_APPLE, 0, "Apple M2", "Apple M2",
     "Apple M1/M2 (below Apple GPU family 9) default to the Base tier (SPEC R7)"},
};

static bool name_matches(const char *name, const char *prefix) {
    const size_t n = strlen(prefix);
    return strncmp(name, prefix, n) == 0 && (name[n] == '\0' || name[n] == ' ');
}

static const de__gpu_default_base_entry *find_default_base(const de_gpu_caps *caps) {
    for (size_t i = 0; i < sizeof k_default_base / sizeof k_default_base[0]; ++i) {
        const de__gpu_default_base_entry *e = &k_default_base[i];
        if (caps->vendor_id != e->vendor_id)
            continue;
        if (e->device_id != 0 && caps->device_id != e->device_id)
            continue;
        if (e->name_prefix && !name_matches(caps->device_name, e->name_prefix))
            continue;
        return e;
    }
    return NULL;
}

/* ---------- Reasons ---------- */

static void add_reason(char (*list)[DE_GPU_TIER_REASON_BYTES], u32 *count, const char *fmt, ...)
    DE_PRINTF_FMT(3, 4);
static void add_reason(char (*list)[DE_GPU_TIER_REASON_BYTES], u32 *count, const char *fmt, ...) {
    if (*count >= DE_GPU_TIER_MAX_REASONS)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(list[*count], DE_GPU_TIER_REASON_BYTES, fmt, ap);
    va_end(ap);
    ++*count;
}

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---------- Evaluation ---------- */

void de_gpu_evaluate_tier(const de_gpu_caps *caps, de_gpu_tier_report *out) {
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    if (!caps) {
        add_reason(out->base_reasons, &out->base_reason_count, "no caps (no device)");
        add_reason(out->rt_reasons, &out->rt_reason_count, "below the Base tier");
        out->recommended = DE_GPU_TIER_NONE;
        return;
    }
    const char *backend = de_gpu_backend_name(caps->backend);

    /* Base: the backend's SPEC §3.3 check first, then the front-end's own requirements. */
    if (caps->backend == DE_GPU_DEFAULT || (u32)caps->backend >= DE_GPU_BACKEND_COUNT_)
        add_reason(out->base_reasons, &out->base_reason_count, "caps name no concrete backend (%s)",
                   backend);
    if (!caps->base_tier_ok)
        add_reason(out->base_reasons, &out->base_reason_count, "%s: %s", backend,
                   caps->base_tier_reason[0] ? caps->base_tier_reason
                                             : "the backend's Base-tier check failed");
    const struct {
        u32 have;
        const char *what;
    } tables[] = {
        {caps->max_bindless_textures, "sampled textures"},
        {caps->max_bindless_storage_textures, "storage textures"},
        {caps->max_bindless_buffers, "buffers"},
        {caps->max_bindless_samplers, "samplers"},
    };
    for (size_t i = 0; i < sizeof tables / sizeof tables[0]; ++i)
        if (tables[i].have == 0)
            add_reason(out->base_reasons, &out->base_reason_count, "no bindless table for %s",
                       tables[i].what);
    if (caps->max_push_constant_bytes < DE_GPU_MAX_PUSH_CONSTANT_BYTES)
        add_reason(out->base_reasons, &out->base_reason_count,
                   "push constants are %u bytes (needs %u)", caps->max_push_constant_bytes,
                   DE_GPU_MAX_PUSH_CONSTANT_BYTES);
    out->base_ok = out->base_reason_count == 0;

    /* RT: Base, inline ray queries (DXR 1.1, VK_KHR_ray_query, Metal intersection queries), and
     * hardware acceleration. */
    if (!out->base_ok)
        add_reason(out->rt_reasons, &out->rt_reason_count, "below the Base tier");
    if (caps->raytracing == DE_RT_NONE)
        add_reason(out->rt_reasons, &out->rt_reason_count, "no ray tracing");
    else if (caps->raytracing == DE_RT_API_ONLY)
        add_reason(out->rt_reasons, &out->rt_reason_count,
                   "ray tracing is not hardware-accelerated (API only)");
    if (!caps->ray_query)
        add_reason(out->rt_reasons, &out->rt_reason_count, "no inline ray queries");
    out->rt_ok = out->rt_reason_count == 0;

    out->mesh_shaders = caps->mesh_shaders && caps->mesh_max_output_vertices > 0 &&
                        caps->mesh_max_output_primitives > 0;

    const de__gpu_default_base_entry *e = find_default_base(caps);
    if (e)
        copy_str(out->default_base_device, sizeof out->default_base_device, e->label);
    if (out->rt_ok && !e) {
        out->recommended = DE_GPU_TIER_RT;
    } else if (out->base_ok) {
        out->recommended = DE_GPU_TIER_BASE;
        if (out->rt_ok)
            copy_str(out->recommended_reason, sizeof out->recommended_reason, e->why);
    } else {
        out->recommended = DE_GPU_TIER_NONE;
    }
}

const char *de_gpu_tier_name(de_gpu_tier t) {
    switch (t) {
    case DE_GPU_TIER_NONE:
        return "none";
    case DE_GPU_TIER_BASE:
        return "base";
    case DE_GPU_TIER_RT:
        return "rt";
    }
    return "unknown";
}
