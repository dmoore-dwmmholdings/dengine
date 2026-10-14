/*
 * rg_builder.c: render graph declarations, versions, dependency edges, validation and culling
 * (SPEC §12.3, DENG-171). The data layout and the frame lifecycle are described in
 * rg_internal.h.
 *
 * Every per-frame array lives on the frame's arena. An array that outgrows its capacity is
 * copied into a block twice as large on the same arena (the old block stays until the arena is
 * reset); de__rg_reset remembers the frame's sizes, so the next frame reserves them up front and
 * a steady frame does a fixed number of arena pushes and no heap allocation.
 */
#include "render/graph/rg_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------- Errors ---------- */

/* Sets the error context and logs it to `render`. `count` adds it to the frame's error count,
 * which de__rg_finalize reports; errors raised while executing (possibly on several threads)
 * do not touch the graph. */
static void rg_fail_v(de_rg *g, bool count, const char *fmt, va_list ap) {
    char msg[DE_ERROR_MESSAGE_MAX];
    vsnprintf(msg, sizeof msg, fmt, ap);
    DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s", msg);
    if (g) {
        DE_ERROR(g->log, "%s", msg);
        if (count)
            ++g->error_count;
    }
}

static void rg_fail(de_rg *g, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
static void rg_fail(de_rg *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rg_fail_v(g, true, fmt, ap);
    va_end(ap);
}

#if DE__RG_VALIDATE
static void rg_fail_exec(const de_rg *g, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
static void rg_fail_exec(const de_rg *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rg_fail_v((de_rg *)g, false, fmt, ap);
    va_end(ap);
}
#endif

static void rg_oom(de_rg *g, const char *what) {
    if (!g->oom) {
        g->oom = true;
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "render graph \"%s\": the frame arena is full (%s)", g->name,
                what);
        DE_ERROR(g->log, "render graph \"%s\": the frame arena is full (%s)", g->name, what);
    }
}

/* ---------- Arena storage ---------- */

/* Returns an array of at least `need` elements holding the first `count` of `old`: `old` itself
 * if `*cap` suffices, else a copy in a new arena block. NULL (and the oom flag) on failure. */
static void *rg_grow(de_rg *g, void *old, u32 count, u32 *cap, u32 need, size_t elem, size_t align,
                     const char *what) {
    if (need <= *cap)
        return old;
    u32 nc = *cap ? *cap : 16u;
    while (nc < need)
        nc = nc > UINT32_MAX / 2u ? need : nc * 2u;
    void *p = de_arena_push(g->arena, (size_t)nc * elem, align);
    if (!p) {
        rg_oom(g, what);
        return NULL;
    }
    if (count)
        memcpy(p, old, (size_t)count * elem);
    *cap = nc;
    return p;
}

/* Every element type holds at most 8-byte fields; 16 keeps the arrays cache-line friendly. */
#define RG_GROW(g, arr, count, cap, need, what)                                                    \
    rg_grow((g), (arr), (count), &(cap), (need), sizeof *(arr), 16u, (what))

static const char *rg_strdup(de_rg *g, const char *s) {
    if (!s)
        s = "";
    size_t n = strlen(s) + 1;
    char *p = (char *)de_arena_push(g->arena, n, 1);
    if (!p) {
        rg_oom(g, "names");
        return "";
    }
    memcpy(p, s, n);
    return p;
}

static const char *rg_strfmt(de_rg *g, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
static const char *rg_strfmt(de_rg *g, const char *fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return rg_strdup(g, buf);
}

/* ---------- Ranges and handles ---------- */

bool de__rg_range_overlaps(de__rg_range a, de__rg_range b) {
    return a.mip_first < b.mip_end && b.mip_first < a.mip_end && a.layer_first < b.layer_end &&
           b.layer_first < a.layer_end;
}

bool de__rg_range_covers(de__rg_range outer, de__rg_range inner) {
    return outer.mip_first <= inner.mip_first && outer.mip_end >= inner.mip_end &&
           outer.layer_first <= inner.layer_first && outer.layer_end >= inner.layer_end;
}

static const de__rg_range k_whole = {0, UINT32_MAX, 0, UINT32_MAX};

u32 de__rg_resource_index(const de_rg *g, de_rg_resource r) {
    if (!g || !g->arena || r.id == 0)
        return DE__RG_NONE;
    if ((r.id >> DE__RG_INDEX_BITS) != g->gen)
        return DE__RG_NONE;
    u32 i = r.id & DE__RG_INDEX_MASK;
    return i < g->resource_count ? i : DE__RG_NONE;
}

de_rg_resource de__rg_resource_handle(const de_rg *g, u32 i) {
    return (de_rg_resource){(i & DE__RG_INDEX_MASK) | (g->gen << DE__RG_INDEX_BITS)};
}

/* ---------- Lifecycle ---------- */

de_result de__rg_create(const de__rg_desc *desc, de_rg **out) {
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__rg_create: out is NULL");
    *out = NULL;
    de__rg_desc d = desc ? *desc : (de__rg_desc){0};
    const de_allocator *a = d.allocator ? d.allocator : de_heap_allocator();
    de_rg *g = DE_NEW(a, de_rg, DE_MEM_RENDER);
    if (!g)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de__rg_create: no memory for the graph");
    memset(g, 0, sizeof *g);
    g->alloc = a;
    g->device = d.device;
    snprintf(g->name, sizeof g->name, "%s", d.name ? d.name : "frame");
    g->log = de_log_register_category("render", DE_LOG_INFO);
    g->dump_cvar = de_cvar_register(&(de_cvar_desc){
        .name = "r.rg_dump",
        .help = "Virtual path (user:/rg.dot): the next finalized render graph is written there as "
                "Graphviz, then this clears",
        .type = DE_CVAR_STRING,
        .flags = DE_CVAR_DEV_ONLY,
    });
    *out = g;
    return DE_OK;
}

void de__rg_destroy(de_rg *g) {
    if (!g)
        return;
    de__rg_execute_shutdown(g); /* DENG-173: timelines, timing heaps (waits for the GPU) */
    de__rg_compile_shutdown(g); /* DENG-172: cached plans and the transient pool */
    DE_FREE(g->alloc, g, sizeof *g);
}

void de__rg_reset(de_rg *g) {
    if (!g)
        return;
    de__rg_compile_frame_end(g); /* DENG-172: the frame's plan binding ends */
    if (g->pass_count > g->hint_passes)
        g->hint_passes = g->pass_count;
    if (g->resource_count > g->hint_resources)
        g->hint_resources = g->resource_count;
    if (g->access_count > g->hint_accesses)
        g->hint_accesses = g->access_count;
    if (g->edge_count > g->hint_edges)
        g->hint_edges = g->edge_count;
    g->arena = NULL;
    g->open = false;
    g->finalized = false;
    g->passes = NULL;
    g->resources = NULL;
    g->accesses = NULL;
    g->edges = NULL;
    g->pass_count = g->pass_cap = 0;
    g->resource_count = g->resource_cap = 0;
    g->access_count = g->access_cap = 0;
    g->edge_count = g->edge_cap = 0;
    g->culled_count = 0;
}

de_result de__rg_begin(de_rg *g, de_arena *arena) {
    if (!g || !arena)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__rg_begin: %s is NULL", g ? "arena" : "graph");
    if (g->arena)
        de__rg_reset(g);
    g->arena = arena;
    g->gen = g->gen % DE__RG_GEN_MASK + 1u; /* 1 .. 4095 */
    g->open = true;
    g->finalized = false;
    g->oom = false;
    g->error_count = 0;
    /* Reserve last frames' sizes so a steady frame never copies an array. */
    if (g->hint_passes &&
        !(g->passes = RG_GROW(g, g->passes, 0, g->pass_cap, g->hint_passes, "passes")))
        return DE_ERR_OUT_OF_MEMORY;
    if (g->hint_resources && !(g->resources = RG_GROW(g, g->resources, 0, g->resource_cap,
                                                      g->hint_resources, "resources")))
        return DE_ERR_OUT_OF_MEMORY;
    if (g->hint_accesses &&
        !(g->accesses = RG_GROW(g, g->accesses, 0, g->access_cap, g->hint_accesses, "accesses")))
        return DE_ERR_OUT_OF_MEMORY;
    if (g->hint_edges && !(g->edges = RG_GROW(g, g->edges, 0, g->edge_cap, g->hint_edges, "edges")))
        return DE_ERR_OUT_OF_MEMORY;
    return DE_OK;
}

/* True if declarations are allowed now; reports why not otherwise. */
static bool rg_check_open(de_rg *g, const char *func) {
    if (!g) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: the graph is NULL", func);
        return false;
    }
    if (g->oom)
        return false; /* reported once by rg_oom */
    if (!g->open) {
        rg_fail(g, "%s: render graph \"%s\" is not open for declarations (%s)", func, g->name,
                g->finalized ? "it was finalized" : "no frame was begun");
        return false;
    }
    return true;
}

/* ---------- Resources ---------- */

static u32 rg_new_resource(de_rg *g, de__rg_res_kind kind, const char *func) {
    if (g->resource_count >= DE__RG_MAX_RESOURCES) {
        rg_fail(g, "%s: more than %u resources in one frame", func, DE__RG_MAX_RESOURCES);
        return DE__RG_NONE;
    }
    de__rg_resource *arr = RG_GROW(g, g->resources, g->resource_count, g->resource_cap,
                                   g->resource_count + 1u, "resources");
    if (!arr)
        return DE__RG_NONE;
    g->resources = arr;
    u32 i = g->resource_count++;
    de__rg_resource *r = &arr[i];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    r->access_first = r->access_last = DE__RG_NONE;
    r->first_pass = r->last_pass = DE__RG_NONE;
    return i;
}

static bool rg_access_bits_known(u32 access) {
    return (access & ~(u32)DE__RG_ACCESS_ALL) == 0;
}

de_rg_resource de_rg_create_texture(de_rg *g, const de_gpu_texture_desc *desc) {
    static const char F[] = "de_rg_create_texture";
    if (!rg_check_open(g, F))
        return (de_rg_resource){0};
    if (!desc) {
        rg_fail(g, "%s: desc is NULL", F);
        return (de_rg_resource){0};
    }
    const char *n = desc->name ? desc->name : "";
    if (desc->format == DE_FMT_UNKNOWN || (u32)desc->format >= (u32)DE_FMT_COUNT_ ||
        desc->width == 0 || desc->usage == 0 || (u32)desc->dim > (u32)DE_TEX_CUBE ||
        desc->mip_count > DE_GPU_MAX_MIPS) {
        rg_fail(g,
                "%s(\"%s\"): needs a format, a width, usage bits, a valid dim and at most %u mips "
                "(format %d, width %u, usage 0x%x, dim %d, mip_count %u)",
                F, n, DE_GPU_MAX_MIPS, (int)desc->format, desc->width, desc->usage, (int)desc->dim,
                desc->mip_count);
        return (de_rg_resource){0};
    }
    u32 i = rg_new_resource(g, DE__RG_TEXTURE, F);
    if (i == DE__RG_NONE)
        return (de_rg_resource){0};
    de__rg_resource *r = &g->resources[i];
    r->tex = *desc;
    r->tex.transient = true;
    r->name =
        desc->name && desc->name[0] ? rg_strdup(g, desc->name) : rg_strfmt(g, "texture#%u", i);
    r->tex.name = r->name;
    r->mip_count = desc->mip_count ? desc->mip_count : 1u;
    if (desc->dim == DE_TEX_3D)
        r->layer_count = 1;
    else if (desc->depth_or_layers)
        r->layer_count = desc->depth_or_layers;
    else
        r->layer_count = desc->dim == DE_TEX_CUBE ? 6u : 1u;
    return de__rg_resource_handle(g, i);
}

de_rg_resource de_rg_create_buffer(de_rg *g, const de_gpu_buffer_desc *desc) {
    static const char F[] = "de_rg_create_buffer";
    if (!rg_check_open(g, F))
        return (de_rg_resource){0};
    if (!desc) {
        rg_fail(g, "%s: desc is NULL", F);
        return (de_rg_resource){0};
    }
    const char *n = desc->name ? desc->name : "";
    if (desc->size == 0 || desc->usage == 0) {
        rg_fail(g, "%s(\"%s\"): needs a size and usage bits (size %llu, usage 0x%x)", F, n,
                (unsigned long long)desc->size, desc->usage);
        return (de_rg_resource){0};
    }
    if (desc->memory != DE_MEM_GPU_ONLY) {
        rg_fail(g,
                "%s(\"%s\"): transient buffers are DE_MEM_GPU_ONLY; import UPLOAD and READBACK "
                "buffers with de_rg_import_buffer",
                F, n);
        return (de_rg_resource){0};
    }
    u32 i = rg_new_resource(g, DE__RG_BUFFER, F);
    if (i == DE__RG_NONE)
        return (de_rg_resource){0};
    de__rg_resource *r = &g->resources[i];
    r->buf = *desc;
    r->name = desc->name && desc->name[0] ? rg_strdup(g, desc->name) : rg_strfmt(g, "buffer#%u", i);
    r->buf.name = r->name;
    return de__rg_resource_handle(g, i);
}

static de_rg_resource rg_import(de_rg *g, de__rg_res_kind kind, u32 id, u32 current_access,
                                const char *F) {
    if (!rg_check_open(g, F))
        return (de_rg_resource){0};
    if (id == 0) {
        rg_fail(g, "%s: the %s handle is 0 (invalid)", F,
                kind == DE__RG_TEXTURE ? "texture" : "buffer");
        return (de_rg_resource){0};
    }
    if (!rg_access_bits_known(current_access)) {
        rg_fail(g, "%s: current_access 0x%x has unknown bits", F, current_access);
        return (de_rg_resource){0};
    }
    /* One graph resource per physical resource, or the versions would not see each other. */
    for (u32 i = 0; i < g->resource_count; ++i) {
        const de__rg_resource *r = &g->resources[i];
        if ((r->flags & DE__RG_RES_IMPORTED) && r->kind == kind &&
            (kind == DE__RG_TEXTURE ? r->texture.id : r->buffer.id) == id)
            return de__rg_resource_handle(g, i);
    }
    u32 i = rg_new_resource(g, kind, F);
    if (i == DE__RG_NONE)
        return (de_rg_resource){0};
    de__rg_resource *r = &g->resources[i];
    r->flags = DE__RG_RES_IMPORTED;
    r->initial_access = current_access;
    if (kind == DE__RG_TEXTURE)
        r->texture.id = id;
    else
        r->buffer.id = id;
    r->name = rg_strfmt(g, "import:%s 0x%08x", kind == DE__RG_TEXTURE ? "texture" : "buffer", id);
    return de__rg_resource_handle(g, i);
}

de_rg_resource de_rg_import_texture(de_rg *g, de_gpu_texture t, u32 current_access) {
    return rg_import(g, DE__RG_TEXTURE, t.id, current_access, "de_rg_import_texture");
}

de_rg_resource de_rg_import_buffer(de_rg *g, de_gpu_buffer b, u32 current_access) {
    return rg_import(g, DE__RG_BUFFER, b.id, current_access, "de_rg_import_buffer");
}

/* The resource of `r`, or NULL after reporting a bad handle. */
static de__rg_resource *rg_res(de_rg *g, de_rg_resource r, const char *F, const char *who) {
    u32 i = de__rg_resource_index(g, r);
    if (i == DE__RG_NONE) {
        rg_fail(g, "%s(%s): resource handle 0x%08x is %s", F, who, r.id,
                r.id == 0 ? "0 (invalid)"
                          : "not a resource of this frame (stale, or from another graph)");
        return NULL;
    }
    return &g->resources[i];
}

void de_rg_export(de_rg *g, de_rg_resource r, u32 final_access) {
    static const char F[] = "de_rg_export";
    if (!rg_check_open(g, F))
        return;
    de__rg_resource *res = rg_res(g, r, F, g->name);
    if (!res)
        return;
    if (!rg_access_bits_known(final_access)) {
        rg_fail(g, "%s(\"%s\"): final_access 0x%x has unknown bits", F, res->name, final_access);
        return;
    }
    if ((final_access & DE_ACCESS_PRESENT) && res->kind != DE__RG_TEXTURE) {
        rg_fail(g, "%s(\"%s\"): DE_ACCESS_PRESENT is for textures", F, res->name);
        return;
    }
    res->flags |= DE__RG_RES_EXPORTED;
    res->final_access = final_access;
}

void de_rg_set_name(de_rg *g, de_rg_resource r, const char *name) {
    static const char F[] = "de_rg_set_name";
    if (!rg_check_open(g, F))
        return;
    de__rg_resource *res = rg_res(g, r, F, g->name);
    if (!res || !name || !name[0])
        return;
    res->name = rg_strdup(g, name);
    if (res->kind == DE__RG_TEXTURE)
        res->tex.name = res->name;
    else
        res->buf.name = res->name;
}

/* ---------- Passes ---------- */

de_rg_pass *de_rg_add_pass(de_rg *g, const char *name, de_gpu_queue queue, de_rg_execute_fn fn,
                           void *user) {
    static const char F[] = "de_rg_add_pass";
    if (!rg_check_open(g, F))
        return NULL;
    if ((u32)queue >= (u32)DE_QUEUE_COUNT_) {
        rg_fail(g, "%s(\"%s\"): queue %d is not a de_gpu_queue", F, name ? name : "", (int)queue);
        return NULL;
    }
    de_rg_pass **arr =
        RG_GROW(g, g->passes, g->pass_count, g->pass_cap, g->pass_count + 1u, "passes");
    if (!arr)
        return NULL;
    g->passes = arr;
    de_rg_pass *p = DE_ARENA_PUSH(g->arena, de_rg_pass, 1);
    if (!p) {
        rg_oom(g, "passes");
        return NULL;
    }
    memset(p, 0, sizeof *p);
    p->graph = g;
    p->index = g->pass_count;
    p->name = name && name[0] ? rg_strdup(g, name) : rg_strfmt(g, "pass#%u", p->index);
    p->queue = queue;
    p->fn = fn;
    p->user = user;
    p->access_first = p->access_last = DE__RG_NONE;
    p->edge_first = 0;
    arr[g->pass_count++] = p;
    return p;
}

void de_rg_pass_side_effect(de_rg_pass *p) {
    if (!p) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_rg_pass_side_effect: the pass is NULL");
        return;
    }
    if (rg_check_open(p->graph, "de_rg_pass_side_effect"))
        p->side_effect = true;
}

#if DE__RG_VALIDATE
/* Checks one access declaration; reports and returns false when it is invalid. */
static bool rg_validate_access(de_rg *g, const de_rg_pass *p, const de__rg_resource *res,
                               de_rg_subresource sub, bool has_sub, u32 access, bool write,
                               const char *F) {
    char bits[160];
    const char *what = write ? "write" : "read";
    if (access == DE_ACCESS_NONE) {
        rg_fail(g, "%s(\"%s\", \"%s\"): access is DE_ACCESS_NONE", F, p->name, res->name);
        return false;
    }
    if (!rg_access_bits_known(access)) {
        rg_fail(g, "%s(\"%s\", \"%s\"): access 0x%x has unknown bits", F, p->name, res->name,
                access);
        return false;
    }
    if (access & DE_ACCESS_PRESENT) {
        rg_fail(
            g,
            "%s(\"%s\", \"%s\"): DE_ACCESS_PRESENT is a final state, not a pass access; pass it "
            "to de_rg_export",
            F, p->name, res->name);
        return false;
    }
    const u32 read_write_bits = DE__RG_WRITE_BITS & ~(u32)DE__RG_LOAD_BITS;
    if (!write && (access & read_write_bits)) {
        rg_fail(g, "%s(\"%s\", \"%s\"): %s are write accesses; declare them with de_rg_write", F,
                p->name, res->name, de__rg_access_str(access & read_write_bits, bits, sizeof bits));
        return false;
    }
    if (write && !(access & DE__RG_WRITE_BITS)) {
        rg_fail(g, "%s(\"%s\", \"%s\"): %s has no write access; declare reads with de_rg_read", F,
                p->name, res->name, de__rg_access_str(access, bits, sizeof bits));
        return false;
    }
    const u32 tex_only = DE_ACCESS_RENDER_TARGET | DE_ACCESS_DEPTH_READ | DE_ACCESS_DEPTH_WRITE;
    const u32 buf_only = DE_ACCESS_INDIRECT | DE_ACCESS_ACCEL_READ | DE_ACCESS_ACCEL_WRITE;
    if (res->kind == DE__RG_BUFFER && (access & tex_only)) {
        rg_fail(g, "%s(\"%s\", \"%s\"): %s is texture-only access, and \"%s\" is a buffer", F,
                p->name, res->name, de__rg_access_str(access & tex_only, bits, sizeof bits),
                res->name);
        return false;
    }
    if (res->kind == DE__RG_TEXTURE && (access & buf_only)) {
        rg_fail(g, "%s(\"%s\", \"%s\"): %s is buffer-only access, and \"%s\" is a texture", F,
                p->name, res->name, de__rg_access_str(access & buf_only, bits, sizeof bits),
                res->name);
        return false;
    }
    if (p->queue == DE_QUEUE_COMPUTE && (access & tex_only)) {
        rg_fail(g,
                "%s(\"%s\", \"%s\"): pass \"%s\" runs on the compute queue, which cannot use %s; "
                "put it on DE_QUEUE_GRAPHICS",
                F, p->name, res->name, p->name,
                de__rg_access_str(access & tex_only, bits, sizeof bits));
        return false;
    }
    const u32 copy_ok = DE_ACCESS_COPY_SRC | DE_ACCESS_COPY_DST;
    if (p->queue == DE_QUEUE_COPY && (access & ~copy_ok)) {
        rg_fail(g,
                "%s(\"%s\", \"%s\"): pass \"%s\" runs on the copy queue, which can only %s with "
                "COPY_SRC or COPY_DST, not %s",
                F, p->name, res->name, p->name, what,
                de__rg_access_str(access & ~copy_ok, bits, sizeof bits));
        return false;
    }
    if (has_sub && res->kind == DE__RG_BUFFER &&
        (sub.mip_first | sub.mip_count | sub.layer_first | sub.layer_count) != 0) {
        rg_fail(g,
                "%s(\"%s\", \"%s\"): buffers are accessed whole; the subresource range must "
                "be zero",
                F, p->name, res->name);
        return false;
    }
    if (res->kind == DE__RG_TEXTURE && !(res->flags & DE__RG_RES_IMPORTED)) {
        u64 mip_end = (u64)sub.mip_first + (sub.mip_count ? sub.mip_count : 1u);
        u64 layer_end = (u64)sub.layer_first + (sub.layer_count ? sub.layer_count : 1u);
        if (mip_end > res->mip_count || layer_end > res->layer_count) {
            rg_fail(g,
                    "%s(\"%s\", \"%s\"): mips from %u (count %u), layers from %u (count %u) "
                    "are outside the texture (%u mips, %u layers)",
                    F, p->name, res->name, sub.mip_first, sub.mip_count, sub.layer_first,
                    sub.layer_count, res->mip_count, res->layer_count);
            return false;
        }
    }
    return true;
}
#endif

static de__rg_range rg_resolve_range(const de__rg_resource *res, de_rg_subresource sub) {
    if (res->kind == DE__RG_BUFFER)
        return k_whole;
    de__rg_range r;
    /* Imports have unknown counts: "to the last" is open-ended. */
    u32 mips = res->mip_count ? res->mip_count : UINT32_MAX;
    u32 layers = res->layer_count ? res->layer_count : UINT32_MAX;
    r.mip_first = sub.mip_first;
    r.mip_end = sub.mip_count ? sub.mip_first + sub.mip_count : mips;
    r.layer_first = sub.layer_first;
    r.layer_end = sub.layer_count ? sub.layer_first + sub.layer_count : layers;
    /* Whole-texture accesses of transients compare equal to open-ended ones. */
    if (r.mip_first == 0 && r.mip_end == res->mip_count)
        r.mip_end = UINT32_MAX;
    if (r.layer_first == 0 && r.layer_end == res->layer_count)
        r.layer_end = UINT32_MAX;
    if (r.mip_end < r.mip_first) /* overflow of an unchecked (Release) range */
        r.mip_end = UINT32_MAX;
    if (r.layer_end < r.layer_first)
        r.layer_end = UINT32_MAX;
    return r;
}

static void rg_declare(de_rg_pass *p, de_rg_resource r, de_rg_subresource sub, bool has_sub,
                       u32 access, bool write, const char *F) {
    if (!p) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: the pass is NULL", F);
        return;
    }
    de_rg *g = p->graph;
    if (!rg_check_open(g, F))
        return;
    de__rg_resource *res = rg_res(g, r, F, p->name);
    if (!res)
        return;
#if DE__RG_VALIDATE
    if (!rg_validate_access(g, p, res, sub, has_sub, access, write, F))
        return;
#else
    (void)has_sub;
#endif
    u32 ri = (u32)(res - g->resources);
    de__rg_range range = rg_resolve_range(res, sub);
    /* The same declaration twice merges its access bits. */
    for (u32 a = p->access_first; a != DE__RG_NONE; a = g->accesses[a].pass_next) {
        de__rg_access *x = &g->accesses[a];
        if (x->resource == ri && x->write == write &&
            memcmp(&x->range, &range, sizeof range) == 0) {
            x->access |= access;
            return;
        }
    }
    de__rg_access *arr =
        RG_GROW(g, g->accesses, g->access_count, g->access_cap, g->access_count + 1u, "accesses");
    if (!arr)
        return;
    g->accesses = arr;
    u32 ai = g->access_count++;
    de__rg_access *x = &arr[ai];
    memset(x, 0, sizeof *x);
    x->pass = p->index;
    x->resource = ri;
    x->access = access;
    x->range = range;
    x->write = write;
    x->pass_next = DE__RG_NONE;
    x->res_prev = x->res_next = DE__RG_NONE;
    if (p->access_last == DE__RG_NONE)
        p->access_first = ai;
    else
        arr[p->access_last].pass_next = ai;
    p->access_last = ai;
    ++p->access_count;
}

void de_rg_read(de_rg_pass *p, de_rg_resource r, u32 access) {
    rg_declare(p, r, (de_rg_subresource){0}, false, access, false, "de_rg_read");
}

void de_rg_write(de_rg_pass *p, de_rg_resource r, u32 access) {
    rg_declare(p, r, (de_rg_subresource){0}, false, access, true, "de_rg_write");
}

void de_rg_read_sub(de_rg_pass *p, de_rg_resource r, de_rg_subresource sub, u32 access) {
    rg_declare(p, r, sub, true, access, false, "de_rg_read_sub");
}

void de_rg_write_sub(de_rg_pass *p, de_rg_resource r, de_rg_subresource sub, u32 access) {
    rg_declare(p, r, sub, true, access, true, "de_rg_write_sub");
}

/* ---------- Execute-time lookups ---------- */

static const de__rg_resource *rg_exec_res(const de_rg_pass *p, de_rg_resource r,
                                          de__rg_res_kind kind, const char *F) {
    if (!p) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: the pass is NULL", F);
        return NULL;
    }
    const de_rg *g = p->graph;
    u32 i = de__rg_resource_index(g, r);
#if DE__RG_VALIDATE
    if (!p->executing) {
        rg_fail_exec(g, "%s(\"%s\"): called outside the pass's execute callback", F, p->name);
        return NULL;
    }
    if (i == DE__RG_NONE) {
        rg_fail_exec(g, "%s(\"%s\"): resource handle 0x%08x is not a resource of this frame", F,
                     p->name, r.id);
        return NULL;
    }
    const de__rg_resource *res = &g->resources[i];
    if (res->kind != kind) {
        rg_fail_exec(g, "%s(\"%s\"): \"%s\" is a %s", F, p->name, res->name,
                     res->kind == DE__RG_TEXTURE ? "texture" : "buffer");
        return NULL;
    }
    bool declared = false;
    for (u32 a = p->access_first; a != DE__RG_NONE && !declared; a = g->accesses[a].pass_next)
        declared = g->accesses[a].resource == i;
    if (!declared) {
        rg_fail_exec(g, "%s(\"%s\"): the pass did not declare a read or write of \"%s\"", F,
                     p->name, res->name);
        return NULL;
    }
    return res;
#else
    (void)kind;
    (void)F;
    return i == DE__RG_NONE ? NULL : &g->resources[i];
#endif
}

de_gpu_texture de_rg_texture(const de_rg_pass *p, de_rg_resource r) {
    const de__rg_resource *res = rg_exec_res(p, r, DE__RG_TEXTURE, "de_rg_texture");
    return res ? res->texture : (de_gpu_texture){0};
}

de_gpu_buffer de_rg_buffer(const de_rg_pass *p, de_rg_resource r) {
    const de__rg_resource *res = rg_exec_res(p, r, DE__RG_BUFFER, "de_rg_buffer");
    return res ? res->buffer : (de_gpu_buffer){0};
}

void de__rg_execute_pass(de_rg *g, u32 pass, de_gpu_cmd *cmd) {
    DE_ASSERT(g && g->finalized);
    if (!g || pass >= g->pass_count)
        return;
    de_rg_pass *p = g->passes[pass];
    if (p->culled || !p->fn)
        return;
    p->executing = true;
    p->fn(cmd, p, p->user);
    p->executing = false;
}

/* ---------- Finalize ---------- */

/* Links every access into its resource's chain in pass order (a pass's reads before its writes,
 * because a pass reads the old contents and writes new ones) and numbers the versions. */
static void rg_build_chains(de_rg *g) {
    for (u32 i = 0; i < g->resource_count; ++i) {
        de__rg_resource *r = &g->resources[i];
        r->access_first = r->access_last = DE__RG_NONE;
        r->version_count = 0;
        r->first_pass = r->last_pass = DE__RG_NONE;
        r->flags &= ~(u32)DE__RG_RES_USED;
    }
    for (u32 pi = 0; pi < g->pass_count; ++pi) {
        const de_rg_pass *p = g->passes[pi];
        for (int phase = 0; phase < 2; ++phase) {
            const bool writes = phase == 1;
            for (u32 a = p->access_first; a != DE__RG_NONE; a = g->accesses[a].pass_next) {
                de__rg_access *x = &g->accesses[a];
                if (x->write != writes)
                    continue;
                de__rg_resource *r = &g->resources[x->resource];
                x->res_prev = r->access_last;
                x->res_next = DE__RG_NONE;
                if (r->access_last == DE__RG_NONE)
                    r->access_first = a;
                else
                    g->accesses[r->access_last].res_next = a;
                r->access_last = a;
                x->version = writes ? ++r->version_count : r->version_count;
            }
        }
    }
}

#if DE__RG_VALIDATE
static void rg_finalize_checks(de_rg *g) {
    char bits[160];
    for (u32 pi = 0; pi < g->pass_count; ++pi) {
        const de_rg_pass *p = g->passes[pi];
        for (u32 a = p->access_first; a != DE__RG_NONE; a = g->accesses[a].pass_next) {
            const de__rg_access *x = &g->accesses[a];
            const de__rg_resource *r = &g->resources[x->resource];
            if (!x->write && !(r->flags & DE__RG_RES_IMPORTED)) {
                /* A transient has no contents until a pass writes them. */
                bool written = false;
                for (u32 b = x->res_prev; b != DE__RG_NONE && !written;
                     b = g->accesses[b].res_prev) {
                    const de__rg_access *y = &g->accesses[b];
                    written = y->write && de__rg_range_overlaps(y->range, x->range);
                }
                if (!written)
                    rg_fail(g,
                            "render graph \"%s\": pass \"%s\" reads transient \"%s\" before any "
                            "pass writes it",
                            g->name, p->name, r->name);
            }
            if (x->write && (x->access & DE__RG_READ_BITS)) {
                /* Read bits in a write mean the pass also reads the old contents: that read
                 * must be declared, or the version it reads is lost (and may be culled). */
                bool read = false;
                for (u32 b = p->access_first; b != DE__RG_NONE && !read;
                     b = g->accesses[b].pass_next) {
                    const de__rg_access *y = &g->accesses[b];
                    read = !y->write && y->resource == x->resource &&
                           de__rg_range_covers(y->range, x->range);
                }
                if (!read)
                    rg_fail(g,
                            "render graph \"%s\": pass \"%s\" writes \"%s\" with read access (%s) "
                            "but does not declare the read; add de_rg_read for the same range",
                            g->name, p->name, r->name,
                            de__rg_access_str(x->access & DE__RG_READ_BITS, bits, sizeof bits));
            }
        }
    }
}
#endif

static void rg_mark(de_rg *g, u32 pass, u32 *work, u32 *work_count) {
    de_rg_pass *p = g->passes[pass];
    if (!p->culled)
        return;
    p->culled = false;
    work[(*work_count)++] = pass;
}

/* Marks the passes whose writes are visible to a read of `range` that comes after access
 * `start` in the resource chain (inclusive): walks back to the first write that covers it. */
static void rg_mark_producers(de_rg *g, u32 start, de__rg_range range, u32 skip_pass, u32 *work,
                              u32 *work_count) {
    for (u32 b = start; b != DE__RG_NONE; b = g->accesses[b].res_prev) {
        const de__rg_access *y = &g->accesses[b];
        if (!y->write || y->pass == skip_pass || !de__rg_range_overlaps(y->range, range))
            continue;
        rg_mark(g, y->pass, work, work_count);
        if (de__rg_range_covers(y->range, range))
            return;
    }
}

/* Reverse reachability from the outputs: side-effect passes, and the producers of the final
 * contents of exported resources and of imported resources that are written. */
static bool rg_cull(de_rg *g) {
    u32 *work = g->pass_count ? DE_ARENA_PUSH(g->arena, u32, g->pass_count) : NULL;
    if (g->pass_count && !work) {
        rg_oom(g, "cull worklist");
        return false;
    }
    u32 n = 0;
    for (u32 pi = 0; pi < g->pass_count; ++pi)
        g->passes[pi]->culled = true;
    for (u32 pi = 0; pi < g->pass_count; ++pi)
        if (g->passes[pi]->side_effect)
            rg_mark(g, pi, work, &n);
    for (u32 i = 0; i < g->resource_count; ++i) {
        const de__rg_resource *r = &g->resources[i];
        bool output = (r->flags & DE__RG_RES_EXPORTED) ||
                      ((r->flags & DE__RG_RES_IMPORTED) && r->version_count > 0);
        if (output)
            rg_mark_producers(g, r->access_last, k_whole, DE__RG_NONE, work, &n);
    }
    while (n) {
        const de_rg_pass *p = g->passes[work[--n]];
        for (u32 a = p->access_first; a != DE__RG_NONE; a = g->accesses[a].pass_next) {
            const de__rg_access *x = &g->accesses[a];
            if (!x->write)
                rg_mark_producers(g, x->res_prev, x->range, p->index, work, &n);
        }
    }
    g->culled_count = 0;
    for (u32 pi = 0; pi < g->pass_count; ++pi)
        g->culled_count += g->passes[pi]->culled ? 1u : 0u;
    return true;
}

static bool rg_add_edge(de_rg *g, de_rg_pass *to, u32 from, u32 resource, de__rg_edge_kind kind) {
    for (u32 e = to->edge_first; e < g->edge_count; ++e) {
        const de__rg_edge *x = &g->edges[e];
        if (x->from == from && x->resource == resource && x->kind == kind)
            return true;
    }
    de__rg_edge *arr =
        RG_GROW(g, g->edges, g->edge_count, g->edge_cap, g->edge_count + 1u, "edges");
    if (!arr)
        return false;
    g->edges = arr;
    /* Insert sorted by (from, resource, kind); a pass has few incoming edges. */
    de__rg_edge e = {from, to->index, resource, kind};
    u32 k = g->edge_count++;
    while (k > to->edge_first) {
        const de__rg_edge *x = &arr[k - 1u];
        bool after = x->from > from ||
                     (x->from == from &&
                      (x->resource > resource || (x->resource == resource && x->kind > kind)));
        if (!after)
            break;
        arr[k] = arr[k - 1u];
        --k;
    }
    arr[k] = e;
    ++to->edge_count;
    return true;
}

/* Incoming edges of every live pass, and which resources live passes use. Accesses of culled
 * passes are skipped: they never run, so nothing has to wait for them. */
static bool rg_build_edges(de_rg *g) {
    for (u32 pi = 0; pi < g->pass_count; ++pi) {
        de_rg_pass *p = g->passes[pi];
        p->edge_first = g->edge_count;
        p->edge_count = 0;
        if (p->culled)
            continue;
        for (int phase = 0; phase < 2; ++phase) {
            const bool writes = phase == 1;
            for (u32 a = p->access_first; a != DE__RG_NONE; a = g->accesses[a].pass_next) {
                const de__rg_access *x = &g->accesses[a];
                if (x->write != writes)
                    continue;
                de__rg_resource *r = &g->resources[x->resource];
                r->flags |= DE__RG_RES_USED;
                if (r->first_pass == DE__RG_NONE)
                    r->first_pass = pi;
                r->last_pass = pi;
                for (u32 b = x->res_prev; b != DE__RG_NONE; b = g->accesses[b].res_prev) {
                    const de__rg_access *y = &g->accesses[b];
                    if (y->pass == pi || g->passes[y->pass]->culled)
                        continue;
                    if (!de__rg_range_overlaps(y->range, x->range))
                        continue;
                    if (x->write || y->write) {
                        de__rg_edge_kind k = !x->write  ? DE__RG_EDGE_RAW
                                             : y->write ? DE__RG_EDGE_WAW
                                                        : DE__RG_EDGE_WAR;
                        if (!rg_add_edge(g, p, y->pass, x->resource, k))
                            return false;
                    }
                    /* Everything before a covering write is ordered before that write. */
                    if (y->write && de__rg_range_covers(y->range, x->range))
                        break;
                }
            }
        }
    }
    return true;
}

de_result de__rg_finalize(de_rg *g) {
    if (!g)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__rg_finalize: the graph is NULL");
    if (!g->open)
        return DE_FAIL(DE_ERR_BUSY, "de__rg_finalize: render graph \"%s\" %s", g->name,
                       g->finalized ? "is already finalized" : "has no open frame");
    g->open = false;
    if (g->oom)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "render graph \"%s\": the frame arena is full",
                       g->name);
    rg_build_chains(g);
#if DE__RG_VALIDATE
    rg_finalize_checks(g);
#endif
    if (!rg_cull(g) || !rg_build_edges(g))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "render graph \"%s\": the frame arena is full",
                       g->name);
    g->finalized = true;
    de__rg_dump_if_requested(g);
    if (g->error_count)
        return DE_FAIL_WRAP(DE_ERR_INVALID_ARGUMENT,
                            "render graph \"%s\": %u invalid declaration%s this frame (the last "
                            "is the cause)",
                            g->name, g->error_count, g->error_count == 1 ? "" : "s");
    return DE_OK;
}
