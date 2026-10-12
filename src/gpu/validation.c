/* validation.c — de_gpu argument checks with specific messages (DENG-281).
 *
 * Create descs and submits are checked in every build: creation is not a per-frame hot path, and
 * these checks are what keeps an invalid desc from reaching a backend. Command recording is
 * checked in Debug and Dev (DE__GPU_VALIDATE_COMMANDS, in gpu.c). Every failure sets the error
 * context (DE_FAIL) and logs the same message to the `gpu` category; stale-handle failures log
 * only in Debug and Dev, so a release build pays one compare for them.
 *
 * Messages name the function, the object (by its debug name) and the field, and say what is
 * allowed: `de_gpu_pipeline_create("blur"): push_constant_bytes is 129; the limit is 128`.
 */
#include "gpu/gpu_backend.h"

#include <stdarg.h>
#include <stdio.h>

/* ---------- Formats ---------- */

typedef struct fmt_info {
    const char *name;
    bool depth, compressed, srgb;
    u32 block_bytes; /* per texel, or per 4x4 block when compressed */
} fmt_info;

static const fmt_info k_formats[DE_FMT_COUNT_] = {
    [DE_FMT_UNKNOWN] = {"DE_FMT_UNKNOWN", false, false, false, 0},
    [DE_FMT_R8_UNORM] = {"DE_FMT_R8_UNORM", false, false, false, 1},
    [DE_FMT_RG8_UNORM] = {"DE_FMT_RG8_UNORM", false, false, false, 2},
    [DE_FMT_RGBA8_UNORM] = {"DE_FMT_RGBA8_UNORM", false, false, false, 4},
    [DE_FMT_RGBA8_SRGB] = {"DE_FMT_RGBA8_SRGB", false, false, true, 4},
    [DE_FMT_BGRA8_UNORM] = {"DE_FMT_BGRA8_UNORM", false, false, false, 4},
    [DE_FMT_BGRA8_SRGB] = {"DE_FMT_BGRA8_SRGB", false, false, true, 4},
    [DE_FMT_R16_FLOAT] = {"DE_FMT_R16_FLOAT", false, false, false, 2},
    [DE_FMT_RG16_FLOAT] = {"DE_FMT_RG16_FLOAT", false, false, false, 4},
    [DE_FMT_RGBA16_FLOAT] = {"DE_FMT_RGBA16_FLOAT", false, false, false, 8},
    [DE_FMT_R32_UINT] = {"DE_FMT_R32_UINT", false, false, false, 4},
    [DE_FMT_RG32_UINT] = {"DE_FMT_RG32_UINT", false, false, false, 8},
    [DE_FMT_R32_FLOAT] = {"DE_FMT_R32_FLOAT", false, false, false, 4},
    [DE_FMT_RG32_FLOAT] = {"DE_FMT_RG32_FLOAT", false, false, false, 8},
    [DE_FMT_RGBA32_FLOAT] = {"DE_FMT_RGBA32_FLOAT", false, false, false, 16},
    [DE_FMT_R11G11B10_FLOAT] = {"DE_FMT_R11G11B10_FLOAT", false, false, false, 4},
    [DE_FMT_RGB10A2_UNORM] = {"DE_FMT_RGB10A2_UNORM", false, false, false, 4},
    [DE_FMT_D32_FLOAT] = {"DE_FMT_D32_FLOAT", true, false, false, 4},
    [DE_FMT_D32_FLOAT_S8] = {"DE_FMT_D32_FLOAT_S8", true, false, false, 8},
    [DE_FMT_BC1_SRGB] = {"DE_FMT_BC1_SRGB", false, true, true, 8},
    [DE_FMT_BC3_SRGB] = {"DE_FMT_BC3_SRGB", false, true, true, 16},
    [DE_FMT_BC4_UNORM] = {"DE_FMT_BC4_UNORM", false, true, false, 8},
    [DE_FMT_BC5_UNORM] = {"DE_FMT_BC5_UNORM", false, true, false, 16},
    [DE_FMT_BC6H_UFLOAT] = {"DE_FMT_BC6H_UFLOAT", false, true, false, 16},
    [DE_FMT_BC7_UNORM] = {"DE_FMT_BC7_UNORM", false, true, false, 16},
    [DE_FMT_BC7_SRGB] = {"DE_FMT_BC7_SRGB", false, true, true, 16},
};

static bool fmt_ok(de_gpu_format f) {
    return (u32)f < (u32)DE_FMT_COUNT_;
}
bool de__gpu_format_is_depth(de_gpu_format f) {
    return fmt_ok(f) && k_formats[f].depth;
}
bool de__gpu_format_is_compressed(de_gpu_format f) {
    return fmt_ok(f) && k_formats[f].compressed;
}
bool de__gpu_format_is_srgb(de_gpu_format f) {
    return fmt_ok(f) && k_formats[f].srgb;
}
u32 de__gpu_format_block_bytes(de_gpu_format f) {
    return fmt_ok(f) ? k_formats[f].block_bytes : 0u;
}
const char *de__gpu_format_name(de_gpu_format f) {
    return fmt_ok(f) ? k_formats[f].name : "(invalid format)";
}

const char *de__gpu_kind_name(de__gpu_kind k) {
    static const char *const names[DE__GPU_KIND_COUNT] = {
        "buffer",    "texture", "sampler",    "pipeline", "acceleration structure",
        "swapchain", "fence",   "query heap",
    };
    return (u32)k < DE__GPU_KIND_COUNT ? names[k] : "object";
}

/* ---------- Failure reporting ---------- */

static de_result fail_v(de_gpu_device *d, de_result r, bool log, const char *fmt, va_list ap) {
    char msg[DE_ERROR_MESSAGE_MAX];
    vsnprintf(msg, sizeof msg, fmt, ap);
    if (log)
        DE_ERROR(d->log, "%s", msg);
    return de_error_set_(r, __FILE__, __LINE__, "de_gpu", "%s", msg);
}

de_result de__gpu_fail(de_gpu_device *d, de_result r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    de_result out = fail_v(d, r, true, fmt, ap);
    va_end(ap);
    return out;
}

static de_result fail_quiet(de_gpu_device *d, de_result r, const char *fmt, ...)
    DE_PRINTF_FMT(3, 4);
static de_result fail_quiet(de_gpu_device *d, de_result r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    de_result out = fail_v(d, r, DE__GPU_VALIDATE_COMMANDS != 0, fmt, ap);
    va_end(ap);
    return out;
}

de_result de__gpu_fail_handle(de_gpu_device *d, const char *func, de__gpu_kind kind, u32 id) {
    if (id == 0)
        return fail_quiet(d, DE_ERR_INVALID_ARGUMENT, "%s: the %s handle is 0 (invalid)", func,
                          de__gpu_kind_name(kind));
    if (de__gpu_pool_is_stale(&d->pools[kind], id))
        return fail_quiet(d, DE_ERR_INVALID_ARGUMENT,
                          "%s: %s handle 0x%08x is stale (slot %u, generation %u: the %s was "
                          "destroyed)",
                          func, de__gpu_kind_name(kind), id, de__gpu_handle_index(id),
                          de__gpu_handle_gen(id), de__gpu_kind_name(kind));
    return fail_quiet(d, DE_ERR_INVALID_ARGUMENT, "%s: 0x%08x is not a %s handle of this device",
                      func, id, de__gpu_kind_name(kind));
}

static const char *nm(const char *name) {
    return name ? name : "";
}

/* ---------- Buffers ---------- */

#define USAGE_ALL          ((1u << 12) - 1u)
#define USAGE_TEXTURE_ONLY (DE_USAGE_SAMPLED | DE_USAGE_RENDER_TARGET | DE_USAGE_DEPTH)
#define USAGE_BUFFER_ONLY                                                                          \
    (DE_USAGE_VERTEX | DE_USAGE_INDEX | DE_USAGE_CONSTANT | DE_USAGE_INDIRECT |                    \
     DE_USAGE_ACCEL_INPUT | DE_USAGE_ACCEL_STORAGE)

de_result de__gpu_validate_buffer_desc(de_gpu_device *d, const de_gpu_buffer_desc *desc) {
    static const char F[] = "de_gpu_buffer_create";
    if (!desc)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s: desc is NULL", F);
    const char *n = nm(desc->name);
    if (desc->size == 0)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s(\"%s\"): size is 0", F, n);
    if (desc->usage == 0)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): usage is 0; give at least one DE_USAGE_* flag", F, n);
    if (desc->usage & ~USAGE_ALL)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s(\"%s\"): usage 0x%x has unknown bits",
                            F, n, desc->usage);
    if (desc->usage & USAGE_TEXTURE_ONLY)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): usage 0x%x has texture-only flags (SAMPLED, "
                            "RENDER_TARGET, DEPTH); buffers are read through STORAGE or CONSTANT",
                            F, n, desc->usage);
    switch (desc->memory) {
    case DE_MEM_GPU_ONLY:
        break;
    case DE_MEM_UPLOAD:
        if (desc->usage & (DE_USAGE_COPY_DST | DE_USAGE_ACCEL_STORAGE))
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                "%s(\"%s\"): DE_MEM_UPLOAD buffers are written by the CPU only; "
                                "usage must not have COPY_DST or ACCEL_STORAGE",
                                F, n);
        break;
    case DE_MEM_READBACK:
        if (desc->usage != DE_USAGE_COPY_DST)
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                "%s(\"%s\"): DE_MEM_READBACK buffers take only DE_USAGE_COPY_DST "
                                "(usage is 0x%x)",
                                F, n, desc->usage);
        break;
    default:
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): memory %d is not a "
                            "de_gpu_memory value",
                            F, n, (int)desc->memory);
    }
    if (desc->stride != 0 && (desc->stride % 4u != 0 || desc->size % desc->stride != 0))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): stride %u must be a multiple of 4 that divides size %llu",
                            F, n, desc->stride, (unsigned long long)desc->size);
    return DE_OK;
}

/* ---------- Textures ---------- */

/* Portable limits: D3D12 feature level 12 / Vulkan and Metal on the Base tier (SPEC §3.3). */
#define MAX_DIM_2D 16384u
#define MAX_DIM_3D 2048u
#define MAX_LAYERS 2048u

static u32 full_mip_chain(u32 w, u32 h, u32 dz) {
    u32 m = w > h ? w : h;
    m = m > dz ? m : dz;
    u32 levels = 1;
    while (m > 1) {
        m >>= 1;
        ++levels;
    }
    return levels;
}

de_result de__gpu_validate_texture_desc(de_gpu_device *d, const de_gpu_texture_desc *desc) {
    static const char F[] = "de_gpu_texture_create";
    if (!desc)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s: desc is NULL", F);
    const char *n = nm(desc->name);
    if ((u32)desc->dim > (u32)DE_TEX_CUBE)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): dim %d is not a "
                            "de_gpu_texture_dim value",
                            F, n, (int)desc->dim);
    if (desc->format == DE_FMT_UNKNOWN || !fmt_ok(desc->format))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): format %d is not a texture "
                            "format",
                            F, n, (int)desc->format);
    const char *fname = de__gpu_format_name(desc->format);
    const bool depth = de__gpu_format_is_depth(desc->format);
    const bool bc = de__gpu_format_is_compressed(desc->format);
    const u32 u = desc->usage;
    if (u == 0)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): usage is 0; give at least one DE_USAGE_* flag", F, n);
    if (u & ~USAGE_ALL)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s(\"%s\"): usage 0x%x has unknown bits",
                            F, n, u);
    if (u & USAGE_BUFFER_ONLY)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): usage 0x%x has buffer-only flags (VERTEX, INDEX, "
                            "CONSTANT, INDIRECT, ACCEL_*)",
                            F, n, u);
    if ((u & DE_USAGE_DEPTH) && !depth)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): usage has DE_USAGE_DEPTH but %s is a color format; use "
                            "DE_FMT_D32_FLOAT or DE_FMT_D32_FLOAT_S8",
                            F, n, fname);
    if (depth && !(u & DE_USAGE_DEPTH))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): %s is a depth format; usage needs DE_USAGE_DEPTH", F, n,
                            fname);
    if ((u & DE_USAGE_DEPTH) && (u & (DE_USAGE_RENDER_TARGET | DE_USAGE_STORAGE)))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): DE_USAGE_DEPTH cannot be combined with RENDER_TARGET or "
                            "STORAGE",
                            F, n);
    if (bc && (u & ~(DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST)))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): %s is block-compressed; usage may only have SAMPLED, "
                            "COPY_SRC and COPY_DST",
                            F, n, fname);
    if ((u & DE_USAGE_STORAGE) && de__gpu_format_is_srgb(desc->format))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): %s is an sRGB format and cannot have DE_USAGE_STORAGE", F,
                            n, fname);

    const u32 w = desc->width;
    const u32 h = desc->height ? desc->height : 1u;
    u32 layers = desc->depth_or_layers;
    if (layers == 0)
        layers = desc->dim == DE_TEX_CUBE ? 6u : 1u;
    if (w == 0)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s(\"%s\"): width is 0", F, n);
    const u32 max_dim = desc->dim == DE_TEX_3D ? MAX_DIM_3D : MAX_DIM_2D;
    if (w > max_dim || h > max_dim)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): %ux%u is larger than the %u limit for this dim", F, n, w,
                            h, max_dim);
    if (desc->dim == DE_TEX_3D && layers > MAX_DIM_3D)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): depth %u is larger than the %u limit", F, n, layers,
                            MAX_DIM_3D);
    if (desc->dim != DE_TEX_3D && layers > MAX_LAYERS)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): %u layers, more than the %u limit", F, n, layers,
                            MAX_LAYERS);
    if (desc->dim == DE_TEX_2D && layers != 1)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): a DE_TEX_2D has 1 layer (depth_or_layers is %u); use "
                            "DE_TEX_2D_ARRAY",
                            F, n, layers);
    if (desc->dim == DE_TEX_CUBE && (w != h || layers % 6u != 0))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): a cube needs width == height and a multiple of 6 layers "
                            "(got %ux%u, %u layers)",
                            F, n, w, h, layers);
    if (desc->dim == DE_TEX_3D && (u & DE_USAGE_DEPTH))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): a DE_TEX_3D cannot be a depth "
                            "target",
                            F, n);
    if (bc && (w % 4u != 0 || h % 4u != 0))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): %s needs a width and height that are multiples of 4 (got "
                            "%ux%u)",
                            F, n, fname, w, h);

    const u32 mips = desc->mip_count ? desc->mip_count : 1u;
    const u32 full = full_mip_chain(w, h, desc->dim == DE_TEX_3D ? layers : 1u);
    if (mips > full || mips > DE_GPU_MAX_MIPS)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): mip_count %u is more than the %u levels of a %ux%u "
                            "texture (limit %u)",
                            F, n, mips, full < DE_GPU_MAX_MIPS ? full : DE_GPU_MAX_MIPS, w, h,
                            DE_GPU_MAX_MIPS);

    const u32 samples = desc->sample_count ? desc->sample_count : 1u;
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): sample_count %u; use 1, 2, 4 or 8", F, n, samples);
    if (samples > 1 && (desc->dim != DE_TEX_2D || mips != 1 || (u & DE_USAGE_STORAGE) || bc))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): multisampled textures must be DE_TEX_2D, 1 mip, "
                            "uncompressed, without DE_USAGE_STORAGE",
                            F, n);
    return DE_OK;
}

/* ---------- Samplers ---------- */

de_result de__gpu_validate_sampler_desc(de_gpu_device *d, const de_gpu_sampler_desc *desc) {
    static const char F[] = "de_gpu_sampler_create";
    if (!desc)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s: desc is NULL", F);
    if ((u32)desc->min > DE_FILTER_LINEAR || (u32)desc->mag > DE_FILTER_LINEAR ||
        (u32)desc->mip > DE_FILTER_LINEAR)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s: min/mag/mip is not a de_gpu_filter "
                            "value",
                            F);
    if ((u32)desc->u > DE_ADDR_BORDER || (u32)desc->v > DE_ADDR_BORDER ||
        (u32)desc->w > DE_ADDR_BORDER)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s: u/v/w is not a de_gpu_address value",
                            F);
    if (!(desc->max_aniso >= 0.0f && desc->max_aniso <= 16.0f))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s: max_aniso %g is outside 0..16", F,
                            (double)desc->max_aniso);
    return DE_OK;
}

/* ---------- Pipelines ---------- */

static bool blob_ok(const de_gpu_shader_blob *b) {
    return b->code != NULL && b->size > 0;
}

de_result de__gpu_validate_pipeline_desc(de_gpu_device *d, const de_gpu_pipeline_desc *desc) {
    static const char F[] = "de_gpu_pipeline_create";
    if (!desc)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s: desc is NULL", F);
    const char *n = nm(desc->name);
    u32 limit = d->caps.max_push_constant_bytes;
    if (limit == 0 || limit > DE_GPU_MAX_PUSH_CONSTANT_BYTES)
        limit = DE_GPU_MAX_PUSH_CONSTANT_BYTES;
    if (desc->push_constant_bytes > limit)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): push_constant_bytes is %u; the limit is %u bytes (SPEC "
                            "12.1: larger per-draw data goes in a buffer)",
                            F, n, desc->push_constant_bytes, limit);
    if (desc->push_constant_bytes % 4u != 0)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): push_constant_bytes %u is not a multiple of 4", F, n,
                            desc->push_constant_bytes);
    switch (desc->kind) {
    case DE_PIPE_COMPUTE:
        if (!blob_ok(&desc->cs))
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                "%s(\"%s\"): a compute pipeline needs a cs blob (code and size)", F,
                                n);
        return DE_OK;
    case DE_PIPE_GRAPHICS:
        if (!blob_ok(&desc->vs))
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                "%s(\"%s\"): a graphics pipeline needs a vs blob (code and size)",
                                F, n);
        break;
    case DE_PIPE_MESH:
        if (!d->caps.mesh_shaders)
            return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                                "%s(\"%s\"): the device has no mesh shaders (caps.mesh_shaders)", F,
                                n);
        if (!blob_ok(&desc->ms))
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                "%s(\"%s\"): a mesh pipeline needs an ms blob (code and size)", F,
                                n);
        break;
    case DE_PIPE_RAYTRACING:
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "%s(\"%s\"): ray-tracing pipelines are not available before S16", F, n);
    default:
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): kind %d is not a de_gpu_pipeline_kind value", F, n,
                            (int)desc->kind);
    }
    /* Raster pipelines. */
    if (desc->color_count > DE_GPU_MAX_COLOR_ATTACHMENTS)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): color_count %u is more than %u", F, n, desc->color_count,
                            DE_GPU_MAX_COLOR_ATTACHMENTS);
    for (u32 i = 0; i < desc->color_count; ++i) {
        de_gpu_format f = desc->color_formats[i];
        if (f == DE_FMT_UNKNOWN || !fmt_ok(f) || de__gpu_format_is_depth(f) ||
            de__gpu_format_is_compressed(f))
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                "%s(\"%s\"): color_formats[%u] is %s, not a renderable color "
                                "format",
                                F, n, i, de__gpu_format_name(f));
        if ((u32)desc->blend[i] > DE_BLEND_ADD)
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                "%s(\"%s\"): blend[%u] is not a de_gpu_blend value", F, n, i);
    }
    if (desc->depth_format != DE_FMT_UNKNOWN && !de__gpu_format_is_depth(desc->depth_format))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): depth_format is %s, not a depth format", F, n,
                            de__gpu_format_name(desc->depth_format));
    if ((desc->depth_test || desc->depth_write) && desc->depth_format == DE_FMT_UNKNOWN)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): depth_test/depth_write need a depth_format", F, n);
    if ((u32)desc->cull > DE_CULL_FRONT)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): cull is not a de_gpu_cull value", F, n);
    return DE_OK;
}

/* ---------- Queries, swapchains, submits ---------- */

#define MAX_QUERIES 65536u

de_result de__gpu_validate_query_heap_desc(de_gpu_device *d, const de_gpu_query_heap_desc *desc) {
    static const char F[] = "de_gpu_query_heap_create";
    if (!desc)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s: desc is NULL", F);
    if (desc->type != DE_QUERY_TIMESTAMP)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): type %d is not a "
                            "de_gpu_query_type value",
                            F, nm(desc->name), (int)desc->type);
    if (desc->count == 0 || desc->count > MAX_QUERIES)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s(\"%s\"): count %u is outside 1..%u", F,
                            nm(desc->name), desc->count, MAX_QUERIES);
    return DE_OK;
}

de_result de__gpu_validate_swapchain_desc(de_gpu_device *d, const de_gpu_swapchain_desc *desc) {
    static const char F[] = "de_gpu_swapchain_create";
    if (!desc)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s: desc is NULL", F);
    if (!DE_HANDLE_VALID(desc->window))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s: window is 0", F);
    if ((u32)desc->colorspace > DE_CS_EDR || (u32)desc->present > DE_PRESENT_IMMEDIATE)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s: colorspace or present is out of range",
                            F);
    return DE_OK;
}

de_result de__gpu_validate_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s) {
    static const char F[] = "de_gpu_queue_submit";
    if ((u32)q >= DE_QUEUE_COUNT_)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s: queue %d is not a de_gpu_queue value",
                            F, (int)q);
    if (!s)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s: submit is NULL", F);
    if (s->cmd_count > 0 && !s->cmds)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s: cmd_count is %u but cmds is NULL", F,
                            s->cmd_count);
    for (u32 i = 0; i < s->cmd_count; ++i) {
        const de_gpu_cmd *c = s->cmds[i];
        if (!c || c->device != d)
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                "%s: cmds[%u] is NULL or from another device", F, i);
        if (c->state != DE__GPU_CMD_RECORDING)
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                "%s: cmds[%u] (\"%s\") is not recording: it was already submitted",
                                F, i, c->name);
        if (c->queue != q)
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                "%s: cmds[%u] (\"%s\") was begun on queue %d, submitted to %d", F,
                                i, c->name, (int)c->queue, (int)q);
        if (c->in_render)
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                "%s: cmds[%u] (\"%s\") has an open render pass (no end_render)", F,
                                i, c->name);
        for (u32 j = 0; j < i; ++j)
            if (s->cmds[j] == c)
                return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                    "%s: cmds[%u] and cmds[%u] are the same command list", F, j, i);
    }
    if (s->wait_count > 0 && !s->waits)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s: wait_count is %u but waits is NULL", F,
                            s->wait_count);
    for (u32 i = 0; i < s->wait_count; ++i)
        if (!de__gpu_pool_get(&d->pools[DE__GPU_FENCE], s->waits[i].fence.id))
            return de__gpu_fail_handle(d, F, DE__GPU_FENCE, s->waits[i].fence.id);
    if (s->signal.id != 0 && !de__gpu_pool_get(&d->pools[DE__GPU_FENCE], s->signal.id))
        return de__gpu_fail_handle(d, F, DE__GPU_FENCE, s->signal.id);
    return DE_OK;
}
