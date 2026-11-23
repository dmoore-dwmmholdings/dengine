/*
 * sky_luts.c: the sky on the GPU (DENG-206, SPEC §12.13): the transmittance and multi-scattering
 * LUTs (regenerated when the turbidity or the ground albedo changes), and the view stage that
 * renders the sky-view LUT and the sky (with the sun and moon disks) into the lighting target
 * where no geometry is. Views lit by an HDRI environment keep it: the stage skips them.
 */
#include "world/sky/sky_internal.h"

#if DE_HAS_RENDER

#include "render/graph/rg_internal.h"
#include "render/lighting/lighting.h"
#include "render/passes/vis/vis_stages.h"
#include "render/renderer_internal.h"
#include "render/shaders/engine_shaders.h"
#include "render/views_internal.h"
#include "render/world/render_world.h"

#include <math.h>
#include <string.h>

/* ---------- The LUTs ---------- */

typedef struct transmittance_params {
    u32 target;
    f32 mie_scale;
    u32 steps, reserved;
} transmittance_params;

typedef struct multiscatter_params {
    u32 target, transmittance, smp;
    f32 mie_scale, albedo;
    u32 steps, reserved0, reserved1;
} multiscatter_params;

#define SKY_TRANSMITTANCE_STEPS 1024u
#define SKY_MULTISCATTER_STEPS  64u
#define SKY_T_PITCH             (DE__SKY_TRANSMITTANCE_W * 16u) /* 4096: a multiple of 256 */
#define SKY_MS_PITCH            (DE__SKY_MULTISCATTER_N * 16u)  /* 512 */

struct de__sky_luts {
    de_gpu_device *dev;
    const de_allocator *alloc;
    de_gpu_pipeline pipe_t, pipe_ms;
    de_gpu_texture tex_t, tex_ms;
    de_gpu_buffer buf_t, buf_ms; /* staging: the kernels write buffers, a copy fills the textures */
    de_gpu_sampler smp;
    u32 t_bindless, ms_bindless, smp_bindless;
    de_gpu_fence fence;
    u64 fence_value;
    bool ready, generated;
    f32 mie_scale, albedo;
};

static de_gpu_barrier buf_barrier(de_gpu_buffer buf, u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.buffer = buf;
    b.before = before;
    b.after = after;
    return b;
}

static de_gpu_barrier tex_barrier(de_gpu_texture t, u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.texture = t;
    b.before = before;
    b.after = after;
    return b;
}

/* A compute pipeline from engine shader `name`. DE_OK with an invalid pipeline when this backend
 * has no code for it. */
static de_result load_pipe(de_gpu_device *dev, const de_allocator *alloc, const char *name,
                           const char *entry, u32 pc_bytes, u32 gx, u32 gy, de_gpu_pipeline *out) {
    *out = (de_gpu_pipeline){0};
    const char *const entries[1] = {entry};
    de__engine_shader_code code;
    char why[512] = "";
    de_result r = de__engine_shader_load(dev, alloc, name, 0, entries, 1, &code, why, sizeof why);
    if (r == DE_ERR_OUT_OF_MEMORY || r == DE_ERR_INVALID_ARGUMENT)
        return DE_FAIL(r, "sky: engine shader %s: %.400s", name, why);
    if (r != DE_OK) {
        de_error_clear();
        return DE_OK;
    }
    *out = de_gpu_pipeline_create(dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                               .cs = code.entries[0].blob,
                                                               .push_constant_bytes = pc_bytes,
                                                               .threadgroup_size = {gx, gy, 1u},
                                                               .name = name});
    de__engine_shader_code_free(&code);
    if (!DE_HANDLE_VALID(*out))
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "sky: the %s pipeline", name);
    return DE_OK;
}

void de__sky_luts_destroy(de__sky_luts *l) {
    if (l == NULL)
        return;
    de_gpu_device *dev = l->dev;
    if (DE_HANDLE_VALID(l->pipe_t))
        de_gpu_pipeline_destroy(dev, l->pipe_t);
    if (DE_HANDLE_VALID(l->pipe_ms))
        de_gpu_pipeline_destroy(dev, l->pipe_ms);
    if (DE_HANDLE_VALID(l->tex_t))
        de_gpu_texture_destroy(dev, l->tex_t);
    if (DE_HANDLE_VALID(l->tex_ms))
        de_gpu_texture_destroy(dev, l->tex_ms);
    if (DE_HANDLE_VALID(l->buf_t))
        de_gpu_buffer_destroy(dev, l->buf_t);
    if (DE_HANDLE_VALID(l->buf_ms))
        de_gpu_buffer_destroy(dev, l->buf_ms);
    if (DE_HANDLE_VALID(l->smp))
        de_gpu_sampler_destroy(dev, l->smp);
    if (DE_HANDLE_VALID(l->fence))
        de_gpu_fence_destroy(dev, l->fence);
    DE_FREE(l->alloc, l, sizeof *l);
}

de_result de__sky_luts_create(de_gpu_device *dev, const de_allocator *alloc, de__sky_luts **out) {
    if (out != NULL)
        *out = NULL;
    if (dev == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__sky_luts_create: NULL argument");
    if (alloc == NULL)
        alloc = de_heap_allocator();
    de__sky_luts *l = DE_NEW(alloc, de__sky_luts, DE_MEM_RENDER);
    if (l == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "sky LUTs: no memory");
    memset(l, 0, sizeof *l);
    l->dev = dev;
    l->alloc = alloc;
    l->t_bindless = l->ms_bindless = l->smp_bindless = DE_GPU_BINDLESS_INVALID;
    de_result r = load_pipe(dev, alloc, "sky_transmittance", "cs_transmittance",
                            (u32)sizeof(transmittance_params), 8u, 8u, &l->pipe_t);
    if (r == DE_OK)
        r = load_pipe(dev, alloc, "sky_multiscatter", "cs_multiscatter",
                      (u32)sizeof(multiscatter_params), 64u, 1u, &l->pipe_ms);
    if (r != DE_OK) {
        de__sky_luts_destroy(l);
        return r;
    }
    if (!DE_HANDLE_VALID(l->pipe_t) || !DE_HANDLE_VALID(l->pipe_ms)) {
        *out = l; /* no code for this backend: not ready */
        return DE_OK;
    }
    const u32 tex_usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC;
    l->tex_t = de_gpu_texture_create(dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA32_FLOAT,
                                                                 .width = DE__SKY_TRANSMITTANCE_W,
                                                                 .height = DE__SKY_TRANSMITTANCE_H,
                                                                 .usage = tex_usage,
                                                                 .name = "sky transmittance LUT"});
    l->tex_ms = de_gpu_texture_create(dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA32_FLOAT,
                                                                  .width = DE__SKY_MULTISCATTER_N,
                                                                  .height = DE__SKY_MULTISCATTER_N,
                                                                  .usage = tex_usage,
                                                                  .name = "sky multi-scattering LUT"});
    l->buf_t = de_gpu_buffer_create(
        dev, &(de_gpu_buffer_desc){.size = (u64)SKY_T_PITCH * DE__SKY_TRANSMITTANCE_H,
                                   .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                   .stride = 16,
                                   .name = "sky transmittance staging"});
    l->buf_ms = de_gpu_buffer_create(
        dev, &(de_gpu_buffer_desc){.size = (u64)SKY_MS_PITCH * DE__SKY_MULTISCATTER_N,
                                   .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                   .stride = 16,
                                   .name = "sky multi-scattering staging"});
    l->smp = de_gpu_sampler_create(dev, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                               .mag = DE_FILTER_LINEAR,
                                                               .mip = DE_FILTER_NEAREST,
                                                               .u = DE_ADDR_CLAMP,
                                                               .v = DE_ADDR_CLAMP,
                                                               .w = DE_ADDR_CLAMP});
    l->fence = de_gpu_fence_create(dev, 0);
    if (!DE_HANDLE_VALID(l->tex_t) || !DE_HANDLE_VALID(l->tex_ms) || !DE_HANDLE_VALID(l->buf_t) ||
        !DE_HANDLE_VALID(l->buf_ms) || !DE_HANDLE_VALID(l->smp) || !DE_HANDLE_VALID(l->fence)) {
        de__sky_luts_destroy(l);
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "sky LUTs: the textures, buffers, sampler or fence");
    }
    l->t_bindless = de_gpu_texture_bindless(dev, l->tex_t, 0, false);
    l->ms_bindless = de_gpu_texture_bindless(dev, l->tex_ms, 0, false);
    l->smp_bindless = de_gpu_sampler_bindless(dev, l->smp);
    l->ready = true;
    *out = l;
    return DE_OK;
}

bool de__sky_luts_ready(const de__sky_luts *l) {
    return l != NULL && l->ready;
}

de_result de__sky_luts_update(de__sky_luts *l, f32 mie_scale, f32 albedo) {
    if (l == NULL || !l->ready)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__sky_luts_update: no LUTs");
    if (l->generated && l->mie_scale == mie_scale && l->albedo == albedo)
        return DE_OK;
    de_gpu_device *dev = l->dev;
    de_gpu_cmd *c = de_gpu_cmd_begin(dev, DE_QUEUE_GRAPHICS, "sky LUTs");
    if (c == NULL)
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "sky LUTs: no command list");
    const u32 tex_before = l->generated ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE;
    const u32 buf_before = l->generated ? DE_ACCESS_COPY_SRC : DE_ACCESS_NONE;
    de_gpu_barrier b[4] = {buf_barrier(l->buf_t, buf_before, DE_ACCESS_SHADER_WRITE),
                           buf_barrier(l->buf_ms, buf_before, DE_ACCESS_SHADER_WRITE),
                           tex_barrier(l->tex_t, tex_before, DE_ACCESS_COPY_DST),
                           tex_barrier(l->tex_ms, tex_before, DE_ACCESS_COPY_DST)};
    de_gpu_cmd_barriers(c, b, 4);
    /* Transmittance. */
    de_gpu_cmd_set_pipeline(c, l->pipe_t);
    const transmittance_params tp = {.target = de_gpu_buffer_bindless(dev, l->buf_t),
                                     .mie_scale = mie_scale,
                                     .steps = SKY_TRANSMITTANCE_STEPS};
    de_gpu_cmd_push_constants(c, &tp, (u32)sizeof tp);
    de_gpu_cmd_dispatch(c, DE__SKY_TRANSMITTANCE_W / 8u, DE__SKY_TRANSMITTANCE_H / 8u, 1);
    b[0] = buf_barrier(l->buf_t, DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, b, 1);
    de_gpu_cmd_copy_buffer_to_texture(c, l->tex_t, 0, 0, l->buf_t, 0, SKY_T_PITCH);
    b[0] = tex_barrier(l->tex_t, DE_ACCESS_COPY_DST, DE_ACCESS_SHADER_READ);
    de_gpu_cmd_barriers(c, b, 1);
    /* Multi-scattering: one group per texel. */
    de_gpu_cmd_set_pipeline(c, l->pipe_ms);
    const multiscatter_params mp = {.target = de_gpu_buffer_bindless(dev, l->buf_ms),
                                    .transmittance = l->t_bindless,
                                    .smp = l->smp_bindless,
                                    .mie_scale = mie_scale,
                                    .albedo = albedo,
                                    .steps = SKY_MULTISCATTER_STEPS};
    de_gpu_cmd_push_constants(c, &mp, (u32)sizeof mp);
    de_gpu_cmd_dispatch(c, DE__SKY_MULTISCATTER_N, DE__SKY_MULTISCATTER_N, 1);
    b[0] = buf_barrier(l->buf_ms, DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, b, 1);
    de_gpu_cmd_copy_buffer_to_texture(c, l->tex_ms, 0, 0, l->buf_ms, 0, SKY_MS_PITCH);
    b[0] = tex_barrier(l->tex_ms, DE_ACCESS_COPY_DST, DE_ACCESS_SHADER_READ);
    de_gpu_cmd_barriers(c, b, 1);
    const de_gpu_submit s = {
        .cmds = &c, .cmd_count = 1, .signal = l->fence, .signal_value = l->fence_value + 1u};
    const de_result r = de_gpu_queue_submit(dev, DE_QUEUE_GRAPHICS, &s);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "sky LUTs: the submission");
    ++l->fence_value;
    l->generated = true;
    l->mie_scale = mie_scale;
    l->albedo = albedo;
    return DE_OK;
}

de_gpu_texture de__sky_luts_transmittance(const de__sky_luts *l) {
    return l != NULL ? l->tex_t : (de_gpu_texture){0};
}
de_gpu_texture de__sky_luts_multiscatter(const de__sky_luts *l) {
    return l != NULL ? l->tex_ms : (de_gpu_texture){0};
}
u32 de__sky_luts_transmittance_bindless(const de__sky_luts *l) {
    return l != NULL ? l->t_bindless : DE_GPU_BINDLESS_INVALID;
}
u32 de__sky_luts_multiscatter_bindless(const de__sky_luts *l) {
    return l != NULL ? l->ms_bindless : DE_GPU_BINDLESS_INVALID;
}
de_gpu_fence de__sky_luts_fence(const de__sky_luts *l, u64 *value) {
    if (value != NULL)
        *value = l != NULL ? l->fence_value : 0u;
    return l != NULL ? l->fence : (de_gpu_fence){0};
}

/* ---------- The view stage ---------- */

typedef struct skyview_params {
    u32 idx[4]; /* target (storage), transmittance, multiscatter, clamp sampler */
    f32 sun[4], moon[4], misc[4];
} skyview_params;

typedef struct sky_params {
    u32 idx0[4]; /* sky-view, transmittance, wrap-u sampler, clamp sampler */
    u32 idx1[4]; /* depth, target (storage), width, height */
    f32 ray00[4], ray_du[4], ray_dv[4], sun[4], moon[4], misc[4];
} sky_params;

#define SKY_VIEW_STEPS 32.0f
/* The largest radiance written (pre-exposed): keeps the sun disk finite in fp16 and in TAA. */
#define SKY_MAX_VALUE 60000.0f

struct de__sky_render {
    de_renderer *renderer;
    struct de_sky *sky;
    const de_allocator *alloc;
    de_gpu_device *dev;
    de__sky_luts *luts;
    de_gpu_pipeline pipe_view, pipe_sky;
    de_gpu_sampler smp_wrap;
    u32 smp_wrap_bindless;
    bool registered, failed;
};

typedef struct view_pass {
    de_gpu_device *dev;
    de_gpu_pipeline pipe;
    de_rg_resource target;
    skyview_params pc;
} view_pass;

typedef struct sky_pass {
    de_gpu_device *dev;
    de_gpu_pipeline pipe;
    de_rg_resource skyview, depth, target;
    sky_params pc;
    u32 gx, gy;
} sky_pass;

static void view_exec(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    view_pass *p = (view_pass *)user;
    p->pc.idx[0] = de_gpu_texture_bindless(p->dev, de_rg_texture(pass, p->target), 0, true);
    de_gpu_cmd_set_pipeline(cmd, p->pipe);
    de_gpu_cmd_push_constants(cmd, &p->pc, (u32)sizeof p->pc);
    de_gpu_cmd_dispatch(cmd, (DE__SKY_VIEW_W + 7u) / 8u, (DE__SKY_VIEW_H + 7u) / 8u, 1);
}

static void sky_exec(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    sky_pass *p = (sky_pass *)user;
    p->pc.idx0[0] = de_gpu_texture_bindless(p->dev, de_rg_texture(pass, p->skyview), 0, false);
    p->pc.idx1[0] = de_gpu_texture_bindless(p->dev, de_rg_texture(pass, p->depth), 0, false);
    p->pc.idx1[1] = de_gpu_texture_bindless(p->dev, de_rg_texture(pass, p->target), 0, true);
    de_gpu_cmd_set_pipeline(cmd, p->pipe);
    de_gpu_cmd_push_constants(cmd, &p->pc, (u32)sizeof p->pc);
    de_gpu_cmd_dispatch(cmd, p->gx, p->gy, 1);
}

static de_vec3 unproject(const de_mat4 *inv, f32 x, f32 y, f32 z) {
    const de_vec4 h = de_m4_mul_v4(inv, (de_vec4){x, y, z, 1.0f});
    const f32 w = h.w != 0.0f ? h.w : 1e-20f;
    return (de_vec3){h.x / w, h.y / w, h.z / w};
}

static de_vec3 ray_at(const de_mat4 *inv, f32 x, f32 y) {
    const de_vec3 far = unproject(inv, x, y, 0.5f), near = unproject(inv, x, y, 1.0f);
    return (de_vec3){far.x - near.x, far.y - near.y, far.z - near.z};
}

static void put3(f32 out[4], de_vec3 v, f32 w) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    out[3] = w;
}

static void stage(de__view_frame *vf, void *user) {
    de__sky_render *sr = (de__sky_render *)user;
    if (sr->failed || !DE_HANDLE_VALID(sr->pipe_view) || !DE_HANDLE_VALID(sr->pipe_sky) ||
        !de__sky_luts_ready(sr->luts) || vf->graph == NULL)
        return;
    de_renderer *r = vf->renderer;
    de__vis_context *vc =
        de__vis_frame_context(de__views_vis(r), vf->view, vf->graph, vf->frame_index);
    de__lighting *lt = de__views_lighting(r);
    de__lighting_context *lc =
        vc != NULL ? de__lighting_frame_context(lt, vf->view, vf->graph, vf->frame_index) : NULL;
    if (vc == NULL || lc == NULL || !lc->lit || !DE_HANDLE_VALID(lc->target) ||
        !DE_HANDLE_VALID(vc->depth) || DE_HANDLE_VALID(lc->strip))
        return; /* nothing lit, or an HDRI environment lights the view */
    de__sky_snapshot sn;
    if (!de__sky_snapshot_get(sr->sky, &sn))
        return;
    const de_result lr = de__sky_luts_update(sr->luts, sn.mie_scale, sn.albedo);
    if (lr != DE_OK) {
        const de_error *e = de_error_last();
        DE_ERROR(r->log, "sky: the LUTs: %s; the sky is off",
                 e != NULL ? e->message : de_result_str(lr));
        sr->failed = true;
        return;
    }
    const u32 slot = (vf->view.id & 0xFFu) - 1u;
    const f32 pre = de__lighting_view_exposure(
        lt, slot, vf->view, vf->frame_index,
        vf->camera != NULL ? vf->camera->camera.exposure_ev100 : (f32)NAN, NULL);
    f64 alt_m = 1.0;
    if (vf->camera != NULL)
        alt_m = vf->camera->has_position ? vf->camera->position.z : (f64)vf->camera->offset.z;
    const f32 r_km = (f32)(DE__SKY_BOTTOM + fmax(alt_m / 1000.0, 0.001));
    const f32 sun_w = sn.sun_weight * pre, moon_w = sn.moon_weight * pre;

    de_rg *g = vf->graph;
    const de_rg_resource skyview = de_rg_create_texture(
        g, &(de_gpu_texture_desc){.width = DE__SKY_VIEW_W,
                                  .height = DE__SKY_VIEW_H,
                                  .format = DE_FMT_RGBA16_FLOAT,
                                  .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED,
                                  .name = "sky.view_lut"});
    view_pass *vp = (view_pass *)de_arena_push(vc->arena, sizeof *vp, 16);
    sky_pass *sp = (sky_pass *)de_arena_push(vc->arena, sizeof *sp, 16);
    if (!DE_HANDLE_VALID(skyview) || vp == NULL || sp == NULL) {
        ++g->error_count;
        return;
    }
    memset(vp, 0, sizeof *vp);
    vp->dev = sr->dev;
    vp->pipe = sr->pipe_view;
    vp->target = skyview;
    vp->pc.idx[1] = de__sky_luts_transmittance_bindless(sr->luts);
    vp->pc.idx[2] = de__sky_luts_multiscatter_bindless(sr->luts);
    vp->pc.idx[3] = sr->luts->smp_bindless;
    put3(vp->pc.sun, sn.sun_dir, sun_w);
    put3(vp->pc.moon, sn.moon_dir, moon_w);
    vp->pc.misc[0] = r_km;
    vp->pc.misc[1] = sn.mie_scale;
    vp->pc.misc[2] = sn.albedo;
    vp->pc.misc[3] = SKY_VIEW_STEPS;
    de_rg_pass *pass = de_rg_add_pass(g, "sky.view_lut", DE_QUEUE_GRAPHICS, view_exec, vp);
    if (pass == NULL)
        return;
    de_rg_write(pass, skyview, DE_ACCESS_SHADER_WRITE);

    /* Camera rays: the inverse of proj x view with the translation removed. */
    de_mat4 rot = vf->view_matrix;
    rot.m[12] = rot.m[13] = rot.m[14] = 0.0f;
    const de_mat4 vp_m = de_m4_mul(&vf->proj, &rot);
    const de_mat4 inv = de_m4_inverse(&vp_m);
    const de_vec3 r00 = ray_at(&inv, -1.0f, 1.0f), r10 = ray_at(&inv, 1.0f, 1.0f),
                  r01 = ray_at(&inv, -1.0f, -1.0f);
    memset(sp, 0, sizeof *sp);
    sp->dev = sr->dev;
    sp->pipe = sr->pipe_sky;
    sp->skyview = skyview;
    sp->depth = vc->depth;
    sp->target = lc->target;
    sp->pc.idx0[1] = de__sky_luts_transmittance_bindless(sr->luts);
    sp->pc.idx0[2] = sr->smp_wrap_bindless;
    sp->pc.idx0[3] = sr->luts->smp_bindless;
    sp->pc.idx1[2] = vf->width;
    sp->pc.idx1[3] = vf->height;
    put3(sp->pc.ray00, r00, r_km);
    put3(sp->pc.ray_du, (de_vec3){r10.x - r00.x, r10.y - r00.y, r10.z - r00.z}, sun_w);
    put3(sp->pc.ray_dv, (de_vec3){r01.x - r00.x, r01.y - r00.y, r01.z - r00.z},
         DE__SKY_SOLAR_ILLUMINANCE * pre);
    put3(sp->pc.sun, sn.sun_dir, DE__SKY_SUN_RADIUS);
    put3(sp->pc.moon, sn.moon_dir, DE__SKY_MOON_RADIUS);
    sp->pc.misc[0] = SKY_MAX_VALUE;
    sp->pc.misc[1] = DE__SKY_MOON_ALBEDO;
    sp->gx = (vf->width + 7u) / 8u;
    sp->gy = (vf->height + 7u) / 8u;
    pass = de_rg_add_pass(g, "sky.render", DE_QUEUE_GRAPHICS, sky_exec, sp);
    if (pass == NULL)
        return;
    de_rg_read(pass, skyview, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, vc->depth, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, lc->target, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, lc->target, DE_ACCESS_SHADER_WRITE);
}

void de__sky_render_destroy(de__sky_render *sr) {
    if (sr == NULL)
        return;
    if (sr->registered) {
        de__render_world_wait_idle(sr->renderer);
        de__view_unregister_stage(sr->renderer, stage, sr);
    }
    if (DE_HANDLE_VALID(sr->pipe_view))
        de_gpu_pipeline_destroy(sr->dev, sr->pipe_view);
    if (DE_HANDLE_VALID(sr->pipe_sky))
        de_gpu_pipeline_destroy(sr->dev, sr->pipe_sky);
    if (DE_HANDLE_VALID(sr->smp_wrap))
        de_gpu_sampler_destroy(sr->dev, sr->smp_wrap);
    de__sky_luts_destroy(sr->luts);
    DE_FREE(sr->alloc, sr, sizeof *sr);
}

de_result de__sky_render_create(struct de_renderer *r, struct de_sky *s, const de_allocator *alloc,
                                de__sky_render **out) {
    if (out != NULL)
        *out = NULL;
    if (r == NULL || s == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__sky_render_create: NULL argument");
    if (alloc == NULL)
        alloc = de_heap_allocator();
    de__sky_render *sr = DE_NEW(alloc, de__sky_render, DE_MEM_RENDER);
    if (sr == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "sky: no memory");
    memset(sr, 0, sizeof *sr);
    sr->renderer = r;
    sr->sky = s;
    sr->alloc = alloc;
    sr->dev = r->dev;
    sr->smp_wrap_bindless = DE_GPU_BINDLESS_INVALID;
    de_result res = de__sky_luts_create(r->dev, alloc, &sr->luts);
    if (res == DE_OK)
        res = load_pipe(r->dev, alloc, "sky_skyview", "cs_skyview", (u32)sizeof(skyview_params), 8u,
                        8u, &sr->pipe_view);
    if (res == DE_OK)
        res = load_pipe(r->dev, alloc, "sky_render", "cs_sky", (u32)sizeof(sky_params), 8u, 8u,
                        &sr->pipe_sky);
    if (res == DE_OK) {
        sr->smp_wrap = de_gpu_sampler_create(r->dev, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                                            .mag = DE_FILTER_LINEAR,
                                                                            .mip = DE_FILTER_NEAREST,
                                                                            .u = DE_ADDR_WRAP,
                                                                            .v = DE_ADDR_CLAMP,
                                                                            .w = DE_ADDR_CLAMP});
        if (!DE_HANDLE_VALID(sr->smp_wrap))
            res = DE_FAIL_WRAP(DE_ERR_INTERNAL, "sky: the sampler");
        else
            sr->smp_wrap_bindless = de_gpu_sampler_bindless(r->dev, sr->smp_wrap);
    }
    if (res == DE_OK) {
        res = de__view_register_stage(r, DE__VIEW_SLOT_TRANSPARENT, -100, "sky", stage, sr);
        sr->registered = res == DE_OK;
    }
    if (res != DE_OK) {
        de__sky_render_destroy(sr);
        return res;
    }
    *out = sr;
    return DE_OK;
}

#endif /* DE_HAS_RENDER */
