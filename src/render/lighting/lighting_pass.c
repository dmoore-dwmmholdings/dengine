/*
 * lighting_pass.c: the lighting stage and the deferred lighting pass (SPEC §12.5 step 5, §12.7;
 * DENG-303). lighting.h describes the module; shaders/lighting/deferred_lighting.slang (engine
 * shader `lighting_deferred`) has the kernels, lighting_common.slang the per-pixel light loop.
 *
 * Per view and frame, after the vis frame (DE__VIEW_SLOT_LIGHTING), when the material resolve ran:
 *
 *   (clusters.c)              the cluster bounds and the light lists
 *   lighting.clear_split      the diffuse and specular targets to 0 (split     targets: WRITE
 *                             frames only: r.split_lighting)
 *   lighting.deferred         one indirect dispatch per material model on      G-buffer, depth,
 * lists, classification's tile lists (classify_args, byte inputs: READ; 16 m), Unlit (model 1)
 * skipped: per pixel of the  lighting, split model, the clustered direct light and the targets,
 * tile indirect inputs, added to the HDR lighting target counters: READ+WRITE lighting.heatmap
 * r.cluster_debug: the lights per cluster as a     heatmap: WRITE colour (the view shows it)
 *   lighting.readback         r.cluster_debug or the test readback: the        COPY_SRC -> ring
 *                             counters into the view's readback ring
 *
 * The lighting constants (DeLightingView) go into a per-view UPLOAD ring (frame N's entry N %
 * DE__VIS_FRAME_RING) while the graph is built; the deferred pass's execute writes the inputs'
 * bindless indices into the same entry (transient textures have indices only then). Units:
 * point and spot lights are in lumens, directional lights in lux, rect and disk lights in nits;
 * the shaders convert (lights.slang). Game material models are lit as Standard until the model
 * registry carries a lighting kernel.
 */
#include "render/lighting/lighting.h"

#include "render/graph/rg_internal.h"

#include "render/materials/brdf_luts.h"
#include "render/passes/vis/gbuffer.h"
#include "render/renderer_internal.h"
#include "render/world/render_world.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/** deferred_lighting.slang LightingParams (128 bytes, DE_GPU_MAX_PUSH_CONSTANT_BYTES). */
typedef struct lp_params {
    u32 view, entry, scene, view_slot;
    u32 depth, gb0, gb1, gb2;
    u32 lighting, diffuse, specular, lists;
    u32 args, tile, tiles_x, tile_count;
    u32 model, width, height, tile_counters;
    /* DeClusterLists */
    u32 lights, dir_lights, grid, list, counters, ltc1, ltc2, sampler;
    /* DENG-429: the pixel's material and triangle */
    u32 ids, ids_pitch, visible, visible_count;
} lp_params;
DE_STATIC_ASSERT(sizeof(lp_params) == 128, "LightingParams is 128 bytes");
DE_STATIC_ASSERT(sizeof(lp_params) <= DE_GPU_MAX_PUSH_CONSTANT_BYTES, "push constants");

/* Cvar defaults. */
#define LP_TILE_DEFAULT         64
#define LP_SLICES_DEFAULT       32
#define LP_FAR_DEFAULT          500.0
#define LP_MAX_LIGHTS_DEFAULT   256
#define LP_CUTOFF_DEFAULT       0.05
#define LP_EMISSIVE_DEFAULT     1000.0
#define LP_EMISSIVE_MAX_DEFAULT 64
#define LP_LOCAL_CAPACITY_MAX   65536u
#define LP_LIST_CAPACITY_MAX    (1u << 22)
#define LP_LIST_AVERAGE         64u /* list entries per cluster provisioned beyond the minimum */
#define LP_LTC1_BYTES           (DE__LTC_SIZE * DE__LTC_SIZE * 16u)
#define LP_LTC2_BYTES           (DE__LTC_SIZE * DE__LTC_SIZE * 8u)

static const de__lighting_const k_consts[] = {
    {"DE_LIGHTING_ORTHO", DE__LIGHTING_ORTHO},
    {"DE_LIGHTING_SPLIT", DE__LIGHTING_SPLIT},
    {"DE_LIGHTING_SSS", DE__LIGHTING_SSS},
    {"DE_LIGHTING_MAX_DIRECTIONAL", DE__LIGHTING_MAX_DIRECTIONAL},
    {"DE_LIGHTING_COUNTER_LOCAL", DE__LIGHTING_COUNTER_LOCAL},
    {"DE_LIGHTING_COUNTER_DIRECTIONAL", DE__LIGHTING_COUNTER_DIRECTIONAL},
    {"DE_LIGHTING_COUNTER_PROXIES", DE__LIGHTING_COUNTER_PROXIES},
    {"DE_LIGHTING_COUNTER_OVERFLOW", DE__LIGHTING_COUNTER_OVERFLOW},
    {"DE_LIGHTING_COUNTER_ENTRIES", DE__LIGHTING_COUNTER_ENTRIES},
    {"DE_LIGHTING_COUNTER_DROPPED", DE__LIGHTING_COUNTER_DROPPED},
    {"DE_LIGHTING_COUNTER_BYTES", DE__LIGHTING_COUNTER_BYTES},
    {"DE_LIGHT_KIND_DIRECTIONAL", DE__LIGHT_DIRECTIONAL},
    {"DE_LIGHT_KIND_POINT", DE__LIGHT_POINT},
    {"DE_LIGHT_KIND_SPOT", DE__LIGHT_SPOT},
    {"DE_LIGHT_KIND_RECT", DE__LIGHT_RECT},
    {"DE_LIGHT_KIND_DISK", DE__LIGHT_DISK},
    {"DE_CLUSTER_LIGHT_PROXY", DE__CLUSTER_LIGHT_PROXY},
    {"DE_CLUSTER_LIGHT_SHADOWS", DE__CLUSTER_LIGHT_SHADOWS},
    {"DE_LIGHT_SCAN_GROUP", DE__LIGHTING_SCAN_GROUP},
    {"DE_LTC_SIZE", DE__LTC_SIZE},
    {"DE_LTC_DISK_SIDES", 12u},
    {"DE_LIGHTING_GROUP", 64u},
    {"DE_IBL_ENVIRONMENT", DE__IBL_ENVIRONMENT}, /* DENG-304 */
    {"DE_IBL_STRIP_FACES", DE__IBL_STRIP_FACES},
    {"DE_INPUT_SPECULAR_OPAQUE", DE__INPUT_SPECULAR_OPAQUE},
    {"DE_INPUT_DIFFUSE_OPAQUE", DE__INPUT_DIFFUSE_OPAQUE},
    {"DE_INPUT_SHADOW_METADATA", DE__INPUT_SHADOW_METADATA},
};

const de__lighting_const *de__lighting_layout_consts(u32 *count) {
    *count = (u32)(sizeof k_consts / sizeof k_consts[0]);
    return k_consts;
}

/* =================================================================================================
 * Per renderer
 * ============================================================================================== */

static void register_cvars(de__lighting *lt) {
    lt->cv_tile = de_cvar_register(
        &(de_cvar_desc){.name = "r.cluster_tile",
                        .help = "Clustered lighting: the edge of a cluster tile in pixels",
                        .type = DE_CVAR_INT,
                        .default_number = LP_TILE_DEFAULT,
                        .min = 8,
                        .max = 256});
    lt->cv_slices = de_cvar_register(
        &(de_cvar_desc){.name = "r.cluster_slices",
                        .help = "Clustered lighting: exponential depth slices of the cluster grid",
                        .type = DE_CVAR_INT,
                        .default_number = LP_SLICES_DEFAULT,
                        .min = 1,
                        .max = 128});
    lt->cv_far = de_cvar_register(&(de_cvar_desc){
        .name = "r.cluster_far",
        .help = "Clustered lighting: the view depth (metres) where the last, infinite slice starts",
        .type = DE_CVAR_FLOAT,
        .default_number = LP_FAR_DEFAULT,
        .min = 1.0,
        .max = 1.0e6});
    lt->cv_max_lights = de_cvar_register(&(de_cvar_desc){
        .name = "r.cluster_max_lights",
        .help = "Clustered lighting: local lights per cluster at most (more are counted, not lit)",
        .type = DE_CVAR_INT,
        .default_number = LP_MAX_LIGHTS_DEFAULT,
        .min = 1,
        .max = 1024});
    lt->cv_cutoff = de_cvar_register(&(de_cvar_desc){
        .name = "r.light_cutoff_lux",
        .help = "Lights without a range end where their illuminance falls to this (lux)",
        .type = DE_CVAR_FLOAT,
        .default_number = LP_CUTOFF_DEFAULT,
        .min = 1.0e-4,
        .max = 100.0});
    lt->cv_emissive_threshold = de_cvar_register(&(de_cvar_desc){
        .name = "r.emissive_light_threshold",
        .help = "Emissive meshes whose luminance (nits) x bounds area (m^2) exceeds this light "
                "the scene through a proxy light (0: no proxies)",
        .type = DE_CVAR_FLOAT,
        .default_number = LP_EMISSIVE_DEFAULT,
        .min = 0.0,
        .max = 1.0e9});
    lt->cv_emissive_max = de_cvar_register(&(de_cvar_desc){
        .name = "r.emissive_light_max",
        .help = "Emissive proxy lights per view and frame at most (more are counted)",
        .type = DE_CVAR_INT,
        .default_number = LP_EMISSIVE_MAX_DEFAULT,
        .min = 0,
        .max = 4096});
    lt->cv_split = de_cvar_register(&(de_cvar_desc){
        .name = "r.split_lighting",
        .help = "Deferred lighting: also write diffuse and specular to separate targets",
        .type = DE_CVAR_BOOL,
        .default_number = 0});
    lt->cv_debug = de_cvar_register(&(de_cvar_desc){
        .name = "r.cluster_debug",
        .help = "Show each view's lights per cluster as a heatmap instead of its colour",
        .type = DE_CVAR_BOOL,
        .flags = DE_CVAR_DEV_ONLY,
        .default_number = 0});
    de_error_clear(); /* a Shipping build refuses the Dev-only one without an error */
}

static void view_release(de__lighting *lt, de__lighting_view_state *vs) {
    if (DE_HANDLE_VALID(vs->constants))
        de_gpu_buffer_destroy(lt->dev, vs->constants); /* deferred until the GPU is done */
    if (DE_HANDLE_VALID(vs->rb))
        de_gpu_buffer_destroy(lt->dev, vs->rb);
    const de__lighting_inputs in = vs->inputs;
    memset(vs, 0, sizeof *vs);
    vs->inputs = in;
}

static void kernels_free(de__lighting *lt) {
    de__vis_shared *s = lt->vis;
    if (s == NULL)
        return;
    de__vis_kernel *k[] = {&lt->cluster_build, &lt->light_clear, &lt->light_prep,
                           &lt->light_count,   &lt->light_scan,  &lt->light_fill,
                           &lt->deferred,      &lt->clear_split, &lt->heatmap};
    for (u32 i = 0; i < sizeof k / sizeof k[0]; ++i)
        de__vis_kernel_destroy(s, k[i]);
}

void de__lighting_destroy(de__lighting *lt) {
    if (lt == NULL)
        return;
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        view_release(lt, &lt->views[i]);
    kernels_free(lt);
    if (DE_HANDLE_VALID(lt->ltc1))
        de_gpu_texture_destroy(lt->dev, lt->ltc1);
    if (DE_HANDLE_VALID(lt->ltc2))
        de_gpu_texture_destroy(lt->dev, lt->ltc2);
    if (DE_HANDLE_VALID(lt->sampler))
        de_gpu_sampler_destroy(lt->dev, lt->sampler);
    de__ibl_shutdown(lt); /* DENG-304 */
    DE_FREE(lt->alloc, lt, sizeof *lt);
}

/* The kernels of the three engine shaders. */
static de_result kernels_load(de__lighting *lt) {
    de__vis_shared *s = lt->vis;
    static const u32 g64[3] = {64u, 1u, 1u}, g8[3] = {8u, 8u, 1u}, g1024[3] = {1024u, 1u, 1u};
    de__engine_shader_code code;
    static const char *const k_build[1] = {"cs_cluster_build"};
    de_result r = de__vis_shader_load(s, "lighting_clusters", k_build, 1, &code);
    if (r != DE_OK)
        return r;
    r = de__vis_kernel_create(s, &code, 0, "lighting cluster build", g64, 32u, &lt->cluster_build);
    de__engine_shader_code_free(&code);
    if (r != DE_OK)
        return r;

    static const char *const k_cull[5] = {"cs_light_clear", "cs_light_prep", "cs_light_count",
                                          "cs_light_scan", "cs_light_fill"};
    r = de__vis_shader_load(s, "lighting_cull", k_cull, 5, &code);
    if (r != DE_OK)
        return r;
    r = de__vis_kernel_create(s, &code, 0, "lighting lights clear", g64, 48u, &lt->light_clear);
    if (r == DE_OK)
        r = de__vis_kernel_create(s, &code, 1, "lighting lights prep", g64, 48u, &lt->light_prep);
    if (r == DE_OK)
        r = de__vis_kernel_create(s, &code, 2, "lighting lights count", g64, 48u, &lt->light_count);
    if (r == DE_OK)
        r = de__vis_kernel_create(s, &code, 3, "lighting lights scan", g1024, 48u, &lt->light_scan);
    if (r == DE_OK)
        r = de__vis_kernel_create(s, &code, 4, "lighting lights fill", g64, 48u, &lt->light_fill);
    de__engine_shader_code_free(&code);
    if (r != DE_OK)
        return r;

    static const char *const k_def[3] = {"cs_lighting_standard", "cs_lighting_clear_split",
                                         "cs_cluster_heatmap"};
    r = de__vis_shader_load(s, "lighting_deferred", k_def, 3, &code);
    if (r != DE_OK)
        return r;
    const u32 pb = (u32)sizeof(lp_params);
    r = de__vis_kernel_create(s, &code, 0, "lighting deferred standard", g64, pb, &lt->deferred);
    if (r == DE_OK)
        r = de__vis_kernel_create(s, &code, 1, "lighting clear split", g8, pb, &lt->clear_split);
    if (r == DE_OK)
        r = de__vis_kernel_create(s, &code, 2, "lighting cluster heatmap", g8, pb, &lt->heatmap);
    de__engine_shader_code_free(&code);
    return r;
}

/* The LTC textures and their sampler: one blocking copy on the graphics queue at create. */
static de_result ltc_create(de__lighting *lt) {
    lt->sampler = de_gpu_sampler_create(lt->dev, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                                        .mag = DE_FILTER_LINEAR,
                                                                        .mip = DE_FILTER_NEAREST,
                                                                        .u = DE_ADDR_CLAMP,
                                                                        .v = DE_ADDR_CLAMP,
                                                                        .w = DE_ADDR_CLAMP});
    lt->ltc1 = de_gpu_texture_create(
        lt->dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA32_FLOAT,
                                        .width = DE__LTC_SIZE,
                                        .height = DE__LTC_SIZE,
                                        .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST,
                                        .name = "lighting ltc1"});
    lt->ltc2 = de_gpu_texture_create(
        lt->dev, &(de_gpu_texture_desc){.format = DE_FMT_RG32_FLOAT,
                                        .width = DE__LTC_SIZE,
                                        .height = DE__LTC_SIZE,
                                        .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST,
                                        .name = "lighting ltc2"});
    const de_gpu_buffer staging =
        de_gpu_buffer_create(lt->dev, &(de_gpu_buffer_desc){.size = LP_LTC1_BYTES + LP_LTC2_BYTES,
                                                            .usage = DE_USAGE_COPY_SRC,
                                                            .memory = DE_MEM_UPLOAD,
                                                            .name = "lighting ltc staging"});
    const de_gpu_fence fence = de_gpu_fence_create(lt->dev, 0);
    de_result r = DE_OK;
    u8 *map = NULL;
    if (!DE_HANDLE_VALID(lt->sampler) || !DE_HANDLE_VALID(lt->ltc1) || !DE_HANDLE_VALID(lt->ltc2) ||
        !DE_HANDLE_VALID(staging) || !DE_HANDLE_VALID(fence) ||
        (map = (u8 *)de_gpu_buffer_map(lt->dev, staging)) == NULL)
        r = DE_FAIL_WRAP(DE_ERR_INTERNAL, "lighting: the LTC tables");
    de_gpu_cmd *cmd = NULL;
    if (r == DE_OK) {
        DE_STATIC_ASSERT(sizeof de__ltc_tab1 == LP_LTC1_BYTES, "tab1 is RGBA32F");
        DE_STATIC_ASSERT(sizeof de__ltc_tab2 == LP_LTC2_BYTES, "tab2 is RG32F");
        memcpy(map, de__ltc_tab1, LP_LTC1_BYTES);
        memcpy(map + LP_LTC1_BYTES, de__ltc_tab2, LP_LTC2_BYTES);
        cmd = de_gpu_cmd_begin(lt->dev, DE_QUEUE_GRAPHICS, "lighting ltc upload");
        if (cmd == NULL)
            r = DE_FAIL_WRAP(DE_ERR_INTERNAL, "lighting: the LTC upload");
    }
    if (r == DE_OK) {
        de_gpu_barrier b[2] = {
            {.texture = lt->ltc1, .before = DE_ACCESS_NONE, .after = DE_ACCESS_COPY_DST},
            {.texture = lt->ltc2, .before = DE_ACCESS_NONE, .after = DE_ACCESS_COPY_DST}};
        de_gpu_cmd_barriers(cmd, b, 2);
        de_gpu_cmd_copy_buffer_to_texture(cmd, lt->ltc1, 0, 0, staging, 0, DE__LTC_SIZE * 16u);
        de_gpu_cmd_copy_buffer_to_texture(cmd, lt->ltc2, 0, 0, staging, LP_LTC1_BYTES,
                                          DE__LTC_SIZE * 8u);
        for (u32 i = 0; i < 2; ++i) {
            b[i].before = DE_ACCESS_COPY_DST;
            b[i].after = DE_ACCESS_SHADER_READ;
        }
        de_gpu_cmd_barriers(cmd, b, 2);
        r = de_gpu_queue_submit(
            lt->dev, DE_QUEUE_GRAPHICS,
            &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1, .signal = fence, .signal_value = 1});
        if (r == DE_OK)
            r = de_gpu_fence_wait(lt->dev, fence, 1, 30ull * 1000000000ull);
        if (r != DE_OK)
            r = DE_FAIL_WRAP(r, "lighting: the LTC upload");
    }
    if (DE_HANDLE_VALID(fence))
        de_gpu_fence_destroy(lt->dev, fence);
    if (DE_HANDLE_VALID(staging))
        de_gpu_buffer_destroy(lt->dev, staging); /* deferred until the GPU is done */
    if (r == DE_OK)
        lt->sampler_bindless = de_gpu_sampler_bindless(lt->dev, lt->sampler);
    return r;
}

de_result de__lighting_create_ex(const de__lighting_desc *desc, de__lighting **out) {
    if (out != NULL)
        *out = NULL;
    if (desc == NULL || out == NULL || desc->device == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__lighting_create_ex: %s",
                       desc == NULL || out == NULL ? "NULL argument" : "no device");
    const de_allocator *a = desc->allocator != NULL ? desc->allocator : de_heap_allocator();
    de__lighting *lt = DE_NEW(a, de__lighting, DE_MEM_RENDER);
    if (lt == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__lighting_create_ex: no memory");
    memset(lt, 0, sizeof *lt);
    lt->dev = desc->device;
    lt->alloc = a;
    lt->vis = desc->vis;
    lt->brdf_lut = desc->brdf_lut;
    lt->log = de_log_register_category("render", DE_LOG_INFO);
    register_cvars(lt);
    if (lt->vis == NULL || !lt->vis->enabled) {
        (void)de__ibl_init(lt); /* the cvars (the exposure is used without the vis frame too) */
        de_error_clear();
        snprintf(lt->why, sizeof lt->why, "the visibility-buffer frame is off");
        *out = lt; /* off: nothing to light */
        return DE_OK;
    }
    de_result r = kernels_load(lt);
    if (r == DE_OK)
        r = ltc_create(lt);
    if (r == DE_OK)
        r = de__ibl_init(lt); /* DENG-304 */
    if (r == DE_ERR_UNSUPPORTED) {
        const de_error *e = de_error_last();
        snprintf(lt->why, sizeof lt->why, "%.250s", e != NULL ? e->message : "no engine shaders");
        DE_WARN(lt->log, "lighting: the deferred lighting is off: %s", lt->why);
        de_error_clear();
        kernels_free(lt);
        *out = lt;
        return DE_OK;
    }
    if (r != DE_OK) {
        de__lighting_destroy(lt);
        return r;
    }
    lt->enabled = true;
    *out = lt;
    return DE_OK;
}

bool de__lighting_enabled(const de__lighting *lt) {
    return lt != NULL && lt->enabled;
}

de__lighting_inputs *de__lighting_view_inputs(de__lighting *lt, u32 slot) {
    return lt != NULL && slot < DE__VIEW_MAX ? &lt->views[slot].inputs : NULL;
}

de__lighting_stats de__lighting_view_stats(const de__lighting *lt, u32 slot) {
    if (lt == NULL || slot >= DE__VIEW_MAX)
        return (de__lighting_stats){0};
    return lt->views[slot].stats;
}

void de__lighting_set_debug(de__lighting *lt, const de__lighting_debug *d) {
    if (lt != NULL)
        lt->debug = d != NULL ? *d : (de__lighting_debug){0};
}

void de__lighting_inv_proj(const de_mat4 *proj, f32 out[4][4]) {
    const de_mat4 inv = de_m4_inverse(proj);
    for (u32 r = 0; r < 4; ++r)
        for (u32 c = 0; c < 4; ++c)
            out[r][c] = inv.m[c * 4u + r];
}

/* =================================================================================================
 * One view
 * ============================================================================================== */

static bool view_ensure(de__lighting *lt, de__lighting_view_state *vs, u32 slot) {
    if (DE_HANDLE_VALID(vs->constants))
        return true;
    char name[48];
    snprintf(name, sizeof name, "lighting view %u", slot);
    vs->constants = de_gpu_buffer_create(
        lt->dev, &(de_gpu_buffer_desc){.size = DE__VIS_FRAME_RING * sizeof(de__lighting_view),
                                       .usage = DE_USAGE_STORAGE,
                                       .memory = DE_MEM_UPLOAD,
                                       .stride = (u32)sizeof(de__lighting_view),
                                       .name = name});
    snprintf(name, sizeof name, "lighting readback %u", slot);
    vs->rb =
        de_gpu_buffer_create(lt->dev, &(de_gpu_buffer_desc){.size = (u64)DE__VIS_FRAME_RING *
                                                                    DE__LIGHTING_READBACK_STRIDE,
                                                            .usage = DE_USAGE_COPY_DST,
                                                            .memory = DE_MEM_READBACK,
                                                            .name = name});
    vs->constants_map =
        DE_HANDLE_VALID(vs->constants) ? (u8 *)de_gpu_buffer_map(lt->dev, vs->constants) : NULL;
    vs->rb_map = DE_HANDLE_VALID(vs->rb) ? (const u8 *)de_gpu_buffer_map(lt->dev, vs->rb) : NULL;
    if (vs->constants_map == NULL || vs->rb_map == NULL) {
        const de_error *e = de_error_last();
        DE_ERROR(lt->log, "lighting: view slot %u: buffers: %s", slot,
                 e != NULL ? e->message : "?");
        const de_view h = vs->handle;
        view_release(lt, vs);
        vs->handle = h;
        return false;
    }
    vs->constants_bindless = de_gpu_buffer_bindless(lt->dev, vs->constants);
    vs->rb_access = DE_ACCESS_NONE;
    return true;
}

/* The newest completed frame's counters (frames before `completed` are done). */
static void read_stats(de__lighting_view_state *vs, u64 completed) {
    i32 best = -1;
    for (u32 k = 0; k < DE__VIS_FRAME_RING; ++k) {
        if (!vs->slot_live[k] || vs->slot_frame[k] >= completed)
            continue;
        if (vs->stats.valid && vs->slot_frame[k] <= vs->stats.frame)
            continue;
        if (best < 0 || vs->slot_frame[k] > vs->slot_frame[best])
            best = (i32)k;
    }
    if (best < 0)
        return;
    u32 c[(DE__LIGHTING_COUNTER_BYTES + DE__LIGHTING_TILE_COUNTER_BYTES) / 4u];
    memcpy(c, vs->rb_map + (size_t)best * DE__LIGHTING_READBACK_STRIDE, sizeof c);
    de__lighting_stats *st = &vs->stats;
    st->valid = true;
    st->frame = vs->slot_frame[best];
    st->local = c[DE__LIGHTING_COUNTER_LOCAL / 4u];
    st->directional = c[DE__LIGHTING_COUNTER_DIRECTIONAL / 4u];
    st->proxies = c[DE__LIGHTING_COUNTER_PROXIES / 4u];
    st->overflow = c[DE__LIGHTING_COUNTER_OVERFLOW / 4u];
    st->entries = c[DE__LIGHTING_COUNTER_ENTRIES / 4u];
    st->dropped = c[DE__LIGHTING_COUNTER_DROPPED / 4u];
    memcpy(st->tiles, &c[DE__LIGHTING_COUNTER_BYTES / 4u], sizeof st->tiles);
}

static u32 next_pow2(u32 v) {
    u32 p = 1u;
    while (p < v && p < 0x80000000u)
        p <<= 1;
    return p;
}

typedef struct lp_pass {
    de__lighting *lt;
    lp_params pc;
    u32 groups_x, groups_y;
    de_rg_resource depth, gb[3], lighting, diffuse, specular, lists, args, tile_counters;
    de_rg_resource lights, dir_lights, grid, list, counters, ltc1, ltc2;
    de_rg_resource ids, visible; /**< DENG-429: the vis IDs and the visible list */
    de_rg_resource inputs[9];
    u32 probe_entry, probe_sampler;
    f32 probe_view_z[4];
    u8 *inputs_dst;  /**< the inputs of this frame's constants entry (the deferred pass) */
    u32 input_flags; /**< DENG-339: bit 0 is an opaque-only indirect specular override. */
    de_gpu_buffer rb;
    u64 rb_offset;
} lp_pass;

static u32 buf_index(const lp_pass *p, const de_rg_pass *pass, de_rg_resource r) {
    return DE_HANDLE_VALID(r) ? de_gpu_buffer_bindless(p->lt->dev, de_rg_buffer(pass, r)) : 0u;
}
static u32 tex_index(const lp_pass *p, const de_rg_pass *pass, de_rg_resource r, bool storage) {
    return DE_HANDLE_VALID(r)
               ? de_gpu_texture_bindless(p->lt->dev, de_rg_texture(pass, r), 0, storage)
               : 0u;
}

static lp_params params(const lp_pass *p, const de_rg_pass *pass) {
    lp_params pc = p->pc;
    pc.depth = tex_index(p, pass, p->depth, false);
    pc.gb0 = tex_index(p, pass, p->gb[0], false);
    pc.gb1 = tex_index(p, pass, p->gb[1], false);
    pc.gb2 = tex_index(p, pass, p->gb[2], false);
    pc.lighting = tex_index(p, pass, p->lighting, true);
    pc.diffuse = tex_index(p, pass, p->diffuse, true);
    pc.specular = tex_index(p, pass, p->specular, true);
    pc.lists = buf_index(p, pass, p->lists);
    pc.args = buf_index(p, pass, p->args);
    pc.tile_counters = buf_index(p, pass, p->tile_counters);
    pc.lights = buf_index(p, pass, p->lights);
    pc.dir_lights = buf_index(p, pass, p->dir_lights);
    pc.grid = buf_index(p, pass, p->grid);
    pc.list = buf_index(p, pass, p->list);
    pc.counters = buf_index(p, pass, p->counters);
    pc.ltc1 = tex_index(p, pass, p->ltc1, false);
    pc.ltc2 = tex_index(p, pass, p->ltc2, false);
    pc.ids = buf_index(p, pass, p->ids); /* DENG-429; 0 (invalid) outside the deferred pass */
    pc.visible = buf_index(p, pass, p->visible);
    return pc;
}

static void clear_split_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const lp_pass *p = (const lp_pass *)user;
    const lp_params pc = params(p, pass);
    de_gpu_cmd_set_pipeline(cmd, p->lt->clear_split.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
    de_gpu_cmd_dispatch(cmd, (pc.width + 7u) / 8u, (pc.height + 7u) / 8u, 1);
}

static void deferred_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const lp_pass *p = (const lp_pass *)user;
    lp_params pc = params(p, pass);
    /* The inputs' indices exist only now (transients): into this frame's constants entry, which
     * the GPU reads after the submission. */
    const de__lighting_inputs_gpu in = {
        .ao = tex_index(p, pass, p->inputs[0], false),
        .shadow_directional = (p->input_flags & DE__INPUT_SHADOW_METADATA)
                                  ? buf_index(p, pass, p->inputs[1])
                                  : tex_index(p, pass, p->inputs[1], false),
        .shadow_local = tex_index(p, pass, p->inputs[2], false),
        .contact_shadows = tex_index(p, pass, p->inputs[3], false),
        .indirect_diffuse = tex_index(p, pass, p->inputs[4], false),
        .indirect_specular = tex_index(p, pass, p->inputs[5], false),
        .reserved0 = p->input_flags,
        .reserved1 = buf_index(p, pass, p->inputs[8]),
        .probe_records = buf_index(p, pass, p->inputs[6]),
        .probe_list = buf_index(p, pass, p->inputs[7]),
        .probe_entry = p->probe_entry,
        .probe_sampler = p->probe_sampler,
        .probe_view_z = {p->probe_view_z[0], p->probe_view_z[1], p->probe_view_z[2],
                         p->probe_view_z[3]}};
    memcpy(p->inputs_dst, &in, sizeof in);
    const de_gpu_buffer args = de_rg_buffer(pass, p->args);
    de_gpu_cmd_set_pipeline(cmd, p->lt->deferred.pipeline);
    for (u32 m = 0; m < DE__GS_MATERIAL_MODELS; ++m) {
        if (m == DE__GS_MATERIAL_MODEL_UNLIT)
            continue; /* emission only: the resolve wrote it */
        pc.model = m;
        de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
        de_gpu_cmd_dispatch_indirect(cmd, args, (u64)m * 16u);
    }
}

static void heatmap_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const lp_pass *p = (const lp_pass *)user;
    const lp_params pc = params(p, pass);
    de_gpu_cmd_set_pipeline(cmd, p->lt->heatmap.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
    de_gpu_cmd_dispatch(cmd, (pc.width + 7u) / 8u, (pc.height + 7u) / 8u, 1);
}

static void readback_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const lp_pass *p = (const lp_pass *)user;
    de_gpu_cmd_copy_buffer(cmd, p->rb, p->rb_offset, de_rg_buffer(pass, p->counters), 0,
                           DE__LIGHTING_COUNTER_BYTES);
    de_gpu_cmd_copy_buffer(cmd, p->rb, p->rb_offset + DE__LIGHTING_COUNTER_BYTES,
                           de_rg_buffer(pass, p->tile_counters), 0,
                           DE__LIGHTING_TILE_COUNTER_BYTES);
}

static lp_pass *pass_data(de__vis_context *vc, const lp_pass *base) {
    lp_pass *p = (lp_pass *)de_arena_push(vc->arena, sizeof *p, 16);
    if (p != NULL)
        *p = *base;
    else
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "lighting: the frame arena is full");
    return p;
}

/* The camera's near plane and far plane (+inf for an infinite projection) from inv_proj rows. */
static void near_far(const f32 *ip, f32 *near_z, f32 *far_z) {
    f32 v[2][4];
    for (u32 i = 0; i < 2; ++i) {
        const f32 z = i == 0 ? 1.0f : 0.0f; /* reverse-Z: 1 = near, 0 = far */
        for (u32 r = 0; r < 4; ++r)
            v[i][r] = ip[4 * r + 2] * z + ip[4 * r + 3];
    }
    *near_z = fabsf(v[0][3]) > 1e-20f ? -v[0][2] / v[0][3] : 0.0f;
    *far_z = fabsf(v[1][3]) > 1e-20f ? -v[1][2] / v[1][3] : INFINITY;
}

/* The constants of one view and frame (lc->view, lc->grid, the capacities). */
static void fill_view(de__lighting *lt, const de__vis_context *vc, const de__lighting_inputs *in,
                      bool split, bool sss, de__lighting_context *lc) {
    de__lighting_view *lv = &lc->view;
    de__lighting_inv_proj(&vc->proj, lv->inv_proj);
    f32 near_z, cam_far;
    near_far(&lv->inv_proj[0][0], &near_z, &cam_far);
    f32 far_z = (f32)de_cvar_float(lt->cv_far);
    if (!(far_z > 0.0f))
        far_z = (f32)LP_FAR_DEFAULT;
    if (cam_far > near_z && cam_far < far_z)
        far_z = cam_far;
    i64 tile = de_cvar_int(lt->cv_tile), slices = de_cvar_int(lt->cv_slices);
    lc->grid =
        de__cluster_grid_make(vc->width, vc->height, tile > 0 ? (u32)tile : (u32)LP_TILE_DEFAULT,
                              slices > 0 ? (u32)slices : (u32)LP_SLICES_DEFAULT, near_z, far_z);
    const de__cluster_grid *g = &lc->grid;
    const i64 max_lights = de_cvar_int(lt->cv_max_lights);
    const u32 per_cluster = max_lights > 0 ? (u32)max_lights : (u32)LP_MAX_LIGHTS_DEFAULT;
    const i64 proxies = DE_HANDLE_VALID(lt->cv_emissive_max) ? de_cvar_int(lt->cv_emissive_max)
                                                             : LP_EMISSIVE_MAX_DEFAULT;
    const u32 light_slots =
        vc->gpu_scene != NULL ? de__gpu_scene_table(vc->gpu_scene, DE__GS_LIGHTS).count : 0u;
    u32 local = next_pow2(light_slots + (u32)(proxies > 0 ? proxies : 0));
    local = local < 64u ? 64u : local > LP_LOCAL_CAPACITY_MAX ? LP_LOCAL_CAPACITY_MAX : local;
    const u64 full = (u64)g->count * per_cluster;
    u64 list = (u64)g->count * LP_LIST_AVERAGE;
    list = list < 65536u ? 65536u : list;
    list = full < list ? full : list;
    list = list > LP_LIST_CAPACITY_MAX ? LP_LIST_CAPACITY_MAX : list;
    lc->local_capacity = local;
    lc->list_capacity = (u32)(list > 0 ? list : 1u);

    lv->tiles_x = g->tiles_x;
    lv->tiles_y = g->tiles_y;
    lv->slices = g->slices;
    lv->tile_px = g->tile_px;
    lv->cluster_count = g->count;
    lv->max_per_cluster = per_cluster;
    lv->list_capacity = lc->list_capacity;
    lv->local_capacity = lc->local_capacity;
    lv->near_z = g->near_z;
    lv->far_z = g->far_z;
    lv->log_scale = g->log_scale;
    lv->log_bias = g->log_bias;
    lv->cutoff = (f32)de_cvar_float(lt->cv_cutoff);
    if (!(lv->cutoff > 0.0f))
        lv->cutoff = (f32)LP_CUTOFF_DEFAULT;
    lv->emissive_threshold = (f32)de_cvar_float(lt->cv_emissive_threshold);
    /* DENG-304: the view's exposure of this frame (exposure.c), unless a producer overrides it. */
    const de__exposure_state *ex =
        vc->view_slot < DE__VIEW_MAX ? &lt->exposure[vc->view_slot] : NULL;
    const bool ex_now =
        ex != NULL && ex->valid && ex->view.id == vc->view.id && ex->frame == vc->frame_index;
    lv->pre_exposure = in->pre_exposure > 0.0f ? in->pre_exposure : ex_now ? ex->current : 1.0f;
    lv->proxy_capacity = proxies > 0 ? (u32)proxies : 0u;
    const bool ortho = vc->proj.m[11] == 0.0f; /* row 3 is (0, 0, -1, 0) in a perspective */
    lv->flags = (ortho ? DE__LIGHTING_ORTHO : 0u) | (split ? DE__LIGHTING_SPLIT : 0u) |
                (sss ? DE__LIGHTING_SSS : 0u);
    lv->light_slots = light_slots;
    lv->brdf_lut = lt->brdf_lut;
    lv->instance_slots =
        lv->emissive_threshold > 0.0f && lv->proxy_capacity > 0 ? vc->instance_count : 0u;
}

de_result de__lighting_build(de__lighting *lt, de_rg *g, de__vis_context *vc,
                             de__lighting_context **out) {
    if (out != NULL)
        *out = NULL;
    if (lt == NULL || g == NULL || vc == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__lighting_build: NULL argument");
    if (!lt->enabled || vc->view_slot >= DE__VIEW_MAX)
        return DE_OK;
    const u32 slot = vc->view_slot;
    de__lighting_view_state *vs = &lt->views[slot];
    if (vc->view_new || vs->handle.id != vc->view.id) {
        view_release(lt, vs);
        vs->handle = vc->view;
    }
    if (DE_HANDLE_VALID(vs->rb))
        read_stats(vs, vc->frames_completed);
    const de__lighting_inputs in = vs->inputs;
    memset(&vs->inputs, 0, sizeof vs->inputs); /* producers set them again each frame */
    if (!DE_HANDLE_VALID(vc->gbuffer[DE__GB_LIGHTING]) || !DE_HANDLE_VALID(vc->depth) ||
        !DE_HANDLE_VALID(vc->classify_tiles) || !DE_HANDLE_VALID(vc->classify_args) ||
        !DE_HANDLE_VALID(vc->scene_views) || vc->width == 0 || vc->height == 0)
        return DE_OK; /* no G-buffer this frame */
    if (!view_ensure(lt, vs, slot))
        return DE_OK; /* logged */
    de__lighting_context *lc =
        (de__lighting_context *)de_arena_push_zero(vc->arena, sizeof *lc, 16);
    if (lc == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__lighting_build: the frame arena is full");

    const bool split = lt->debug.split || de_cvar_bool(lt->cv_split);
    const bool heatmap =
        lt->debug.heatmap || (DE_HANDLE_VALID(lt->cv_debug) && de_cvar_bool(lt->cv_debug));
    const bool readback = lt->debug.readback || heatmap;
    const bool sss = in.subsurface; /* DENG-306 */
    fill_view(lt, vc, &in, split, sss, lc);
    lc->shadow_records = in.shadow_records;
    lc->shadow_texture_count = in.shadow_texture_count;
    memcpy(lc->shadow_textures, in.shadow_textures, sizeof lc->shadow_textures);
    lc->gi_records = in.gi_records;
    lc->gi_texture_count = in.gi_texture_count;
    lc->gi_buffer_count = in.gi_buffer_count;
    memcpy(lc->gi_textures, in.gi_textures, sizeof lc->gi_textures);
    memcpy(lc->gi_buffers, in.gi_buffers, sizeof lc->gi_buffers);
    lc->probe_records = in.probe_records;
    lc->probe_list = in.probe_list;
    lc->probe_texture_count = in.probe_texture_count;
    memcpy(lc->probe_textures, in.probe_textures, sizeof lc->probe_textures);
    lc->target = vc->gbuffer[DE__GB_LIGHTING];
    lc->width = vc->width;
    lc->height = vc->height;
    de__ibl_build(lt, g, lc); /* DENG-304: the environment (lc->view.ibl, the strip) */
    vs->entry = (u32)(vc->frame_index % DE__VIS_FRAME_RING);
    u8 *entry = vs->constants_map + (size_t)vs->entry * sizeof(de__lighting_view);
    memcpy(entry, &lc->view, sizeof lc->view);

    if (!de__clusters_build(lt, g, vc, vs, lc))
        return DE_OK; /* error set; finalize reports it */

    lp_pass base = {
        .lt = lt,
        .pc = {.view = vs->constants_bindless,
               .entry = vs->entry,
               .scene = vc->scene_views_bindless,
               .view_slot = slot,
               .tile = vc->classify_tile,
               .tiles_x = vc->classify_tiles_x,
               .tile_count = vc->classify_tile_count,
               .width = vc->width,
               .height = vc->height,
               .sampler = lt->sampler_bindless},
    };
    /* The LTC tables (uploaded at create). */
    const de_rg_resource ltc1 = de_rg_import_texture(g, lt->ltc1, DE_ACCESS_SHADER_READ);
    const de_rg_resource ltc2 = de_rg_import_texture(g, lt->ltc2, DE_ACCESS_SHADER_READ);
    de_rg_set_name(g, ltc1, "lighting.ltc1");
    de_rg_set_name(g, ltc2, "lighting.ltc2");
    lc->ltc1 = ltc1; /* DENG-305: the forward pass reads the same lists and tables */
    lc->ltc2 = ltc2;
    lc->constants_bindless = vs->constants_bindless;
    lc->constants_entry = vs->entry;
    lc->sampler_bindless = lt->sampler_bindless;

    /* The split targets. DENG-306: subsurface scattering alone needs only the diffuse target, and
     * no clear (the SSS pass reads it at SUBSURFACE pixels only, which the deferred pass writes).
     */
    if (sss && !split) {
        lc->diffuse = de_rg_create_texture(
            g,
            &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                   .width = vc->width,
                                   .height = vc->height,
                                   .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                   .name = "lighting.diffuse"});
        if (!DE_HANDLE_VALID(lc->diffuse))
            return DE_OK;
    }
    if (split) {
        lc->diffuse = de_rg_create_texture(
            g,
            &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                   .width = vc->width,
                                   .height = vc->height,
                                   .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                   .name = "lighting.diffuse"});
        lc->specular = de_rg_create_texture(
            g,
            &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                   .width = vc->width,
                                   .height = vc->height,
                                   .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                   .name = "lighting.specular"});
        if (!DE_HANDLE_VALID(lc->diffuse) || !DE_HANDLE_VALID(lc->specular))
            return DE_OK;
        lp_pass b = base;
        b.diffuse = lc->diffuse;
        b.specular = lc->specular;
        lp_pass *pd = pass_data(vc, &b);
        if (pd == NULL)
            return DE_OK;
        de_rg_pass *p =
            de_rg_add_pass(g, "lighting.clear_split", DE_QUEUE_GRAPHICS, clear_split_execute, pd);
        de_rg_write(p, lc->diffuse, DE_ACCESS_SHADER_WRITE);
        de_rg_write(p, lc->specular, DE_ACCESS_SHADER_WRITE);
    }

    /* The deferred pass. */
    {
        lp_pass b = base;
        b.depth = vc->depth;
        b.gb[0] = vc->gbuffer[DE__GB0];
        b.gb[1] = vc->gbuffer[DE__GB1];
        b.gb[2] = vc->gbuffer[DE__GB2];
        b.lighting = vc->gbuffer[DE__GB_LIGHTING];
        b.diffuse = lc->diffuse;
        b.specular = lc->specular;
        b.lists = vc->classify_tiles;
        b.args = vc->classify_args;
        b.tile_counters = lc->tile_counters;
        b.lights = lc->lights;
        b.dir_lights = lc->dir_lights;
        b.grid = lc->grid_buf;
        b.list = lc->list;
        b.counters = lc->counters;
        b.ltc1 = ltc1;
        b.ltc2 = ltc2;
        /* DENG-429: the pixel's material (and triangle) through its vis ID. */
        b.ids = vc->vis_ids_buffer;
        b.visible = vc->visible_meshlets;
        b.pc.ids_pitch = vc->vis_ids_pitch;
        b.pc.visible_count = vc->visible_capacity + vc->visible_alpha_capacity;
        const de_rg_resource inputs[9] = {
            in.ao,
            DE_HANDLE_VALID(in.shadow_records) ? in.shadow_records : in.shadow_directional,
            in.shadow_local,
            in.contact_shadows,
            in.indirect_diffuse,
            in.indirect_specular,
            in.probe_records,
            in.probe_list,
            in.gi_records};
        memcpy(b.inputs, inputs, sizeof inputs);
        b.probe_entry = in.probe_entry;
        b.probe_sampler = in.probe_sampler;
        memcpy(b.probe_view_z, in.probe_view_z, sizeof b.probe_view_z);
        b.inputs_dst = entry + offsetof(de__lighting_view, inputs);
        b.input_flags = (in.indirect_specular_opaque ? DE__INPUT_SPECULAR_OPAQUE : 0u) |
                        (in.indirect_diffuse_opaque ? DE__INPUT_DIFFUSE_OPAQUE : 0u) |
                        (DE_HANDLE_VALID(in.shadow_records) ? DE__INPUT_SHADOW_METADATA : 0u);
        lp_pass *pd = pass_data(vc, &b);
        if (pd == NULL)
            return DE_OK;
        de_rg_pass *p =
            de_rg_add_pass(g, "lighting.deferred", DE_QUEUE_GRAPHICS, deferred_execute, pd);
        de_rg_read(p, vc->depth, DE_ACCESS_SHADER_READ);
        for (u32 t = 0; t < 3; ++t)
            de_rg_read(p, b.gb[t], DE_ACCESS_SHADER_READ);
        de_rg_read(p, b.lighting, DE_ACCESS_SHADER_READ);
        de_rg_write(p, b.lighting, DE_ACCESS_SHADER_WRITE);
        if (split) {
            de_rg_read(p, lc->diffuse, DE_ACCESS_SHADER_READ);
            de_rg_write(p, lc->diffuse, DE_ACCESS_SHADER_WRITE);
            de_rg_read(p, lc->specular, DE_ACCESS_SHADER_READ);
            de_rg_write(p, lc->specular, DE_ACCESS_SHADER_WRITE);
        } else if (DE_HANDLE_VALID(lc->diffuse)) {
            de_rg_write(p, lc->diffuse, DE_ACCESS_SHADER_WRITE); /* DENG-306: SSS pixels only */
        }
        de_rg_read(p, vc->classify_tiles, DE_ACCESS_SHADER_READ);
        de_rg_read(p, vc->classify_args, DE_ACCESS_INDIRECT | DE_ACCESS_SHADER_READ);
        de_rg_read(p, vc->scene_views, DE_ACCESS_SHADER_READ);
        const de_rg_resource material_reads[5] = {b.ids, b.visible, vc->scene_instances,
                                                  vc->scene_meshes, vc->scene_materials};
        for (u32 i = 0; i < 5; ++i) /* DENG-429 */
            if (DE_HANDLE_VALID(material_reads[i]))
                de_rg_read(p, material_reads[i], DE_ACCESS_SHADER_READ);
        de_rg_read(p, lc->lights, DE_ACCESS_SHADER_READ);
        de_rg_read(p, lc->dir_lights, DE_ACCESS_SHADER_READ);
        de_rg_read(p, lc->grid_buf, DE_ACCESS_SHADER_READ);
        de_rg_read(p, lc->list, DE_ACCESS_SHADER_READ);
        de_rg_read(p, lc->counters, DE_ACCESS_SHADER_READ);
        de_rg_read(p, ltc1, DE_ACCESS_SHADER_READ);
        de_rg_read(p, ltc2, DE_ACCESS_SHADER_READ);
        if (DE_HANDLE_VALID(lc->strip))
            de_rg_read(p, lc->strip, DE_ACCESS_SHADER_READ); /* DENG-304: the environment */
        for (u32 i = 0; i < 9; ++i)
            if (DE_HANDLE_VALID(inputs[i]))
                de_rg_read(p, inputs[i], DE_ACCESS_SHADER_READ);
        for (u32 i = 0; i < in.shadow_texture_count; ++i)
            de_rg_read(p, in.shadow_textures[i], DE_ACCESS_SHADER_READ);
        for (u32 i = 0; i < in.gi_texture_count; ++i) {
            de_rg_read(p, in.gi_textures[i], DE_ACCESS_SHADER_READ);
            de_rg_write(p, in.gi_textures[i], DE_ACCESS_SHADER_WRITE);
        }
        for (u32 i = 0; i < in.gi_buffer_count; ++i)
            de_rg_read(p, in.gi_buffers[i], DE_ACCESS_SHADER_READ);
        for (u32 i = 0; i < in.probe_texture_count; ++i)
            de_rg_read(p, in.probe_textures[i], DE_ACCESS_SHADER_READ);
        de_rg_read(p, lc->tile_counters, DE_ACCESS_SHADER_READ); /* atomics */
        de_rg_write(p, lc->tile_counters, DE_ACCESS_SHADER_WRITE);
        lc->lit = true;
    }

    /* r.cluster_debug: the heatmap (the stage makes it the view's colour). */
    if (heatmap) {
        lc->heatmap = de_rg_create_texture(
            g,
            &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                   .width = vc->width,
                                   .height = vc->height,
                                   .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                   .name = "lighting.heatmap"});
        lp_pass b = base;
        b.depth = vc->depth;
        b.lighting = lc->heatmap;
        b.grid = lc->grid_buf;
        lp_pass *pd = DE_HANDLE_VALID(lc->heatmap) ? pass_data(vc, &b) : NULL;
        if (pd != NULL) {
            de_rg_pass *p =
                de_rg_add_pass(g, "lighting.heatmap", DE_QUEUE_GRAPHICS, heatmap_execute, pd);
            de_rg_read(p, vc->depth, DE_ACCESS_SHADER_READ);
            de_rg_read(p, vc->scene_views, DE_ACCESS_SHADER_READ);
            de_rg_read(p, lc->grid_buf, DE_ACCESS_SHADER_READ);
            de_rg_write(p, lc->heatmap, DE_ACCESS_SHADER_WRITE);
        }
    }

    /* The counters into the readback ring (a side effect: it keeps the passes it reads). */
    if (readback) {
        lp_pass b = base;
        b.counters = lc->counters;
        b.tile_counters = lc->tile_counters;
        b.rb = vs->rb;
        b.rb_offset = (u64)vs->entry * DE__LIGHTING_READBACK_STRIDE;
        lp_pass *pd = pass_data(vc, &b);
        if (pd != NULL) {
            de_rg_pass *p =
                de_rg_add_pass(g, "lighting.readback", DE_QUEUE_GRAPHICS, readback_execute, pd);
            de_rg_pass_side_effect(p);
            de_rg_read(p, lc->counters, DE_ACCESS_COPY_SRC);
            de_rg_read(p, lc->tile_counters, DE_ACCESS_COPY_SRC);
            const de_rg_resource rb = de_rg_import_buffer(g, vs->rb, vs->rb_access);
            de_rg_set_name(g, rb, "lighting.readback");
            de_rg_write(p, rb, DE_ACCESS_COPY_DST);
            vs->rb_access = DE_ACCESS_COPY_DST;
            vs->slot_frame[vs->entry] = vc->frame_index;
            vs->slot_live[vs->entry] = true;
        }
    }
    /* DENG-304: the output pass finds the context. */
    lt->frame_context[slot] = lc;
    lt->frame_context_graph[slot] = g;
    lt->frame_context_frame[slot] = vc->frame_index;
    lt->frame_context_view[slot] = vc->view;
    if (out != NULL)
        *out = lc;
    return DE_OK;
}

de__lighting_context *de__lighting_frame_context(const de__lighting *lt, de_view view,
                                                 const de_rg *g, u64 frame_index) {
    if (lt == NULL || g == NULL)
        return NULL;
    for (u32 i = 0; i < DE__VIEW_MAX; ++i) /* the keys, never the context: arenas are reused */
        if (lt->frame_context[i] != NULL && lt->frame_context_graph[i] == g &&
            lt->frame_context_frame[i] == frame_index && lt->frame_context_view[i].id == view.id)
            return lt->frame_context[i];
    return NULL;
}

/* =================================================================================================
 * The renderer's view stage
 * ============================================================================================== */

static void lighting_stage(de__view_frame *vf, void *user) {
    de__lighting *lt = (de__lighting *)user;
    if (!lt->enabled)
        return;
    de__vis_context *vc = de__vis_frame_context(lt->vis, vf->view, vf->graph, vf->frame_index);
    if (vc == NULL)
        return;
    /* DENG-304: this frame's exposure (the GPU scene's view stage asked first; same value). */
    (void)de__lighting_view_exposure(
        lt, vc->view_slot, vf->view, vf->frame_index,
        vf->camera != NULL ? vf->camera->camera.exposure_ev100 : (f32)NAN, NULL);
    /* DENG-306: the SSS stage runs after this one when the view's sss quality is on. */
    de__lighting_inputs *in = de__lighting_view_inputs(lt, vc->view_slot);
    if (in != NULL)
        in->subsurface = vf->settings != NULL && vf->settings->sss != DE_QUALITY_OFF;
    de__lighting_context *lc = NULL;
    const de_result res = de__lighting_build(lt, vf->graph, vc, &lc);
    if (res != DE_OK) {
        const de_error *e = de_error_last();
        DE_ERROR(lt->log, "lighting: view \"%s\": %s", vf->name,
                 e != NULL ? e->message : de_result_str(res));
    }
    if (lc != NULL && DE_HANDLE_VALID(lc->heatmap)) {
        vf->color = lc->heatmap;
        vf->color_format = DE_FMT_RGBA16_FLOAT;
    }
}

de_result de__lighting_create(de_renderer *r, de__vis_shared *vis, de__lighting **out) {
    *out = NULL;
    de__lighting *lt = NULL;
    de_result res =
        de__lighting_create_ex(&(de__lighting_desc){.device = r->dev,
                                                    .allocator = r->alloc,
                                                    .vis = vis,
                                                    .brdf_lut = de__renderer_brdf_lut(r)},
                               &lt);
    if (res != DE_OK)
        return res;
    res = de__view_register_stage(r, DE__VIEW_SLOT_LIGHTING, 0, "lighting", lighting_stage, lt);
    if (res != DE_OK) {
        de__lighting_destroy(lt);
        return res;
    }
    *out = lt;
    return DE_OK;
}
