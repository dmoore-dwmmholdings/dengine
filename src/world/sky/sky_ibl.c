/*
 * sky_ibl.c: the physical sky as the image-based lighting environment when there is no HDRI
 * (DENG-838; SPEC §12.13, §12.8). The sky view stage (sky_luts.c) asks for a generation when the
 * sun or the moon turned more than DE__SKY_IBL_SUN_DEG (or the atmosphere or the viewer's altitude
 * changed): one standalone command list on the graphics queue
 *
 *   1. cs_sky_cube (shaders/sky/sky_ibl.slang) writes the sky's radiance (nits, sky_integrate with
 *      the ground bounce, no sun or moon disk) into mip 0 of a 128 x 768 face strip;
 *   2. the probe kernels (shaders/probes/probe_prefilter.slang, S14) downsample it into a full mip
 *      chain and GGX-prefilter it into a second strip (mip k = roughness k / 7);
 *   3. the prefiltered strip goes through a transfer buffer into the back one of two sampled
 *      strips (so each bindless view has one consistent layout, as probe_capture.c);
 *   4. mip DE__SKY_IBL_SH_MIP (16 x 96) of the box-filtered radiance is copied to a readback
 *      buffer,
 *
 * then signals a fence. de__sky_ibl_poll, once the fence passed, projects the SH9 irradiance on the
 * CPU (de__sky_ibl_project_sh) and swaps the strips; the stage hands the current one to lighting
 * (de__lighting_set_sky), which uses it while no DeEnvironment HDRI is loaded. One generation is
 * in flight at a time; every texture and buffer is made at create: generating allocates nothing.
 */
#include "world/sky/sky_internal.h"

#include <math.h>
#include <string.h>

/* ---------- Pure parts ---------- */

static f32 sky_half_to_float(u16 h) {
    const u32 s = (u32)(h >> 15) << 31, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    u32 bits;
    if (e == 0u) {
        if (m == 0u) {
            bits = s;
        } else { /* subnormal */
            f32 v = (f32)m / 1024.0f / 16384.0f;
            memcpy(&bits, &v, sizeof bits);
            bits |= s;
        }
    } else if (e == 31u) {
        bits = s | 0x7F800000u | (m << 13);
    } else {
        bits = s | ((e + 112u) << 23) | (m << 13);
    }
    f32 out;
    memcpy(&out, &bits, sizeof out);
    return out;
}

/* The face directions of de_cooked_env.h (and probe_prefilter.slang's face_dir), unnormalized. */
static void sky_face_dir(u32 f, f64 sc, f64 tc, f64 d[3]) {
    switch (f) {
    case 0:
        d[0] = 1.0, d[1] = -tc, d[2] = -sc;
        break;
    case 1:
        d[0] = -1.0, d[1] = -tc, d[2] = sc;
        break;
    case 2:
        d[0] = sc, d[1] = 1.0, d[2] = tc;
        break;
    case 3:
        d[0] = sc, d[1] = -1.0, d[2] = -tc;
        break;
    case 4:
        d[0] = sc, d[1] = -tc, d[2] = 1.0;
        break;
    default:
        d[0] = -sc, d[1] = -tc, d[2] = -1.0;
        break;
    }
}

void de__sky_ibl_project_sh(const u16 *texels, u32 n, u32 pitch_texels, f32 out[9][3]) {
    f64 acc[9][3];
    memset(acc, 0, sizeof acc);
    f64 total = 0.0;
    if (texels != NULL && n > 0u) {
        const f64 texel = 2.0 / (f64)n;
        for (u32 f = 0; f < 6u; ++f)
            for (u32 j = 0; j < n; ++j)
                for (u32 i = 0; i < n; ++i) {
                    const f64 sc = ((f64)i + 0.5) * texel - 1.0, tc = ((f64)j + 0.5) * texel - 1.0;
                    const f64 q = 1.0 + sc * sc + tc * tc;
                    const f64 w = texel * texel / (q * sqrt(q)); /* the texel's solid angle */
                    f64 d[3];
                    sky_face_dir(f, sc, tc, d);
                    const f64 inv = 1.0 / sqrt(q);
                    const f64 x = d[0] * inv, y = d[1] * inv, z = d[2] * inv;
                    const f64 b[9] = {0.282094791773878,
                                      0.488602511902920 * y,
                                      0.488602511902920 * z,
                                      0.488602511902920 * x,
                                      1.092548430592079 * x * y,
                                      1.092548430592079 * y * z,
                                      0.315391565252520 * (3.0 * z * z - 1.0),
                                      1.092548430592079 * x * z,
                                      0.546274215296040 * (x * x - y * y)};
                    const u16 *t = texels + ((size_t)(f * n + j) * pitch_texels + i) * 4u;
                    const f64 L[3] = {sky_half_to_float(t[0]), sky_half_to_float(t[1]),
                                      sky_half_to_float(t[2])};
                    for (u32 k = 0; k < 9u; ++k)
                        for (u32 c = 0; c < 3u; ++c)
                            acc[k][c] += L[c] * b[k] * w;
                    total += w;
                }
    }
    /* The texels' solid angles sum to 4 pi up to the midpoint rule: normalize them to it. */
    const f64 norm = total > 0.0 ? 4.0 * 3.14159265358979323846 / total : 0.0;
    const f64 pi = 3.14159265358979323846;
    const f64 A[9] = {pi,          2.0 * pi / 3.0, 2.0 * pi / 3.0, 2.0 * pi / 3.0, pi / 4.0,
                      pi / 4.0,    pi / 4.0,       pi / 4.0,       pi / 4.0};
    for (u32 k = 0; k < 9u; ++k)
        for (u32 c = 0; c < 3u; ++c)
            out[k][c] = (f32)(acc[k][c] * norm * A[k]);
}

static bool sky_turned(de_vec3 a, de_vec3 b) {
    const f32 cos_limit = cosf(DE__SKY_IBL_SUN_DEG * 3.14159265f / 180.0f);
    return a.x * b.x + a.y * b.y + a.z * b.z < cos_limit;
}

static bool sky_weight_changed(f32 a, f32 b) {
    const f32 m = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b);
    return fabsf(a - b) > 0.01f * m;
}

bool de__sky_ibl_stale(const de__sky_snapshot *last, f32 last_r_km, const de__sky_snapshot *now,
                       f32 r_km) {
    if (last == NULL || now == NULL || !last->valid)
        return true;
    return sky_turned(last->sun_dir, now->sun_dir) || sky_turned(last->moon_dir, now->moon_dir) ||
           sky_weight_changed(last->sun_weight, now->sun_weight) ||
           sky_weight_changed(last->moon_weight, now->moon_weight) ||
           last->mie_scale != now->mie_scale || last->albedo != now->albedo ||
           fabsf(last_r_km - r_km) > DE__SKY_IBL_ALT_KM;
}

bool de__sky_ibl_owner_claim(de__sky_ibl_owner *o, u32 view_id, u64 frame_index) {
    const bool free_or_lapsed = o->last_frame_plus1 == 0 || o->view_id == view_id ||
                                frame_index > o->last_frame_plus1; /* unseen for a whole frame */
    if (!free_or_lapsed)
        return false;
    o->view_id = view_id;
    o->last_frame_plus1 = frame_index + 1u;
    return true;
}

/* ---------- The generator (render builds) ---------- */

#if DE_HAS_RENDER

#include "render/lighting/lighting.h"
#include "render/shaders/engine_shaders.h"

typedef struct sky_cube_params {
    u32 idx[4]; /* target (storage, mip 0), transmittance, multiscatter, clamp sampler */
    f32 sun[4], moon[4], misc[4];
    u32 size[4];
} sky_cube_params;
DE_STATIC_ASSERT(sizeof(sky_cube_params) == 80, "SkyIblParams is 80 bytes");

/* probe_prefilter.slang's ProbeFilterParams (probe_capture.c). */
typedef struct sky_filter_params {
    u32 src, dst, size, face, mip, mips, sampler, samples;
    u32 raw_mips[12];
} sky_filter_params;
DE_STATIC_ASSERT(sizeof(sky_filter_params) == 80, "ProbeFilterParams is 80 bytes");

#define SKY_IBL_STEPS   32.0f
#define SKY_IBL_SAMPLES 256u
#define SKY_IBL_SH_N    (DE__SKY_IBL_SIZE >> DE__SKY_IBL_SH_MIP)
#define SKY_IBL_SH_PITCH 512u /* bytes: 16 texels x 8, padded to the copy alignment */

struct de__sky_ibl {
    de_gpu_device *dev;
    const de_allocator *alloc;
    de_gpu_pipeline pipe_cube, pipe_down, pipe_filter;
    de_gpu_texture raw, filtered; /* storage strips, full chains */
    de_gpu_texture strip[2];      /* sampled strips: current and back */
    bool strip_written[2];
    de_gpu_buffer transfer, readback;
    u64 mip_offset[DE__SKY_IBL_MIPS];
    u32 mip_pitch[DE__SKY_IBL_MIPS];
    u32 raw_access, filtered_access, transfer_access;
    de_gpu_fence fence;
    u64 fence_value;
    u32 current; /* strip index of the current environment */
    bool ready, pending, have_current;
    f32 sh9[9][3];
    de__sky_snapshot last; /* what the last generation (pending or done) was for */
    f32 last_r_km;
};

static de_gpu_barrier sky_tex_barrier(de_gpu_texture t, u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.texture = t;
    b.before = before;
    b.after = after;
    return b;
}

static de_gpu_barrier sky_buf_barrier(de_gpu_buffer buf, u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.buffer = buf;
    b.before = before;
    b.after = after;
    return b;
}

/* Compute pipelines for `count` entries of engine shader `name` (8 x 8 groups). DE_OK with
 * invalid pipelines when this backend has no code for it. */
static de_result sky_load(de_gpu_device *dev, const de_allocator *alloc, const char *name,
                          const char *const *entries, u32 count, u32 pc_bytes,
                          de_gpu_pipeline *out) {
    for (u32 i = 0; i < count; ++i)
        out[i] = (de_gpu_pipeline){0};
    de__engine_shader_code code;
    char why[512] = "";
    de_result r = de__engine_shader_load(dev, alloc, name, 0, entries, count, &code, why, sizeof why);
    if (r == DE_ERR_OUT_OF_MEMORY || r == DE_ERR_INVALID_ARGUMENT)
        return DE_FAIL(r, "sky IBL: engine shader %s: %.400s", name, why);
    if (r != DE_OK) {
        de_error_clear();
        return DE_OK;
    }
    for (u32 i = 0; i < count && r == DE_OK; ++i) {
        out[i] = de_gpu_pipeline_create(dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                                    .cs = code.entries[i].blob,
                                                                    .push_constant_bytes = pc_bytes,
                                                                    .threadgroup_size = {8u, 8u, 1u},
                                                                    .name = entries[i]});
        if (!DE_HANDLE_VALID(out[i]))
            r = DE_FAIL_WRAP(DE_ERR_INTERNAL, "sky IBL: the %s pipeline", entries[i]);
    }
    de__engine_shader_code_free(&code);
    return r;
}

void de__sky_ibl_destroy(de__sky_ibl *ibl) {
    if (ibl == NULL)
        return;
    de_gpu_device *dev = ibl->dev;
    if (ibl->pending && DE_HANDLE_VALID(ibl->fence))
        (void)de_gpu_fence_wait(dev, ibl->fence, ibl->fence_value, UINT64_MAX);
    const de_gpu_pipeline pipes[3] = {ibl->pipe_cube, ibl->pipe_down, ibl->pipe_filter};
    for (u32 i = 0; i < 3u; ++i)
        if (DE_HANDLE_VALID(pipes[i]))
            de_gpu_pipeline_destroy(dev, pipes[i]);
    const de_gpu_texture texs[4] = {ibl->raw, ibl->filtered, ibl->strip[0], ibl->strip[1]};
    for (u32 i = 0; i < 4u; ++i)
        if (DE_HANDLE_VALID(texs[i]))
            de_gpu_texture_destroy(dev, texs[i]); /* deferred until the GPU is done */
    if (DE_HANDLE_VALID(ibl->transfer))
        de_gpu_buffer_destroy(dev, ibl->transfer);
    if (DE_HANDLE_VALID(ibl->readback))
        de_gpu_buffer_destroy(dev, ibl->readback);
    if (DE_HANDLE_VALID(ibl->fence))
        de_gpu_fence_destroy(dev, ibl->fence);
    DE_FREE(ibl->alloc, ibl, sizeof *ibl);
}

de_result de__sky_ibl_create(de_gpu_device *dev, const de_allocator *alloc, de__sky_ibl **out) {
    if (out != NULL)
        *out = NULL;
    if (dev == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__sky_ibl_create: NULL argument");
    if (alloc == NULL)
        alloc = de_heap_allocator();
    de__sky_ibl *ibl = DE_NEW(alloc, de__sky_ibl, DE_MEM_RENDER);
    if (ibl == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "sky IBL: no memory");
    memset(ibl, 0, sizeof *ibl);
    ibl->dev = dev;
    ibl->alloc = alloc;
    const char *const cube_entry[1] = {"cs_sky_cube"};
    const char *const probe_entries[3] = {"cs_probe_downsample", "cs_probe_prefilter",
                                          "cs_probe_copy"};
    de_gpu_pipeline probe[3];
    de_result r = sky_load(dev, alloc, "sky_ibl", cube_entry, 1, (u32)sizeof(sky_cube_params),
                           &ibl->pipe_cube);
    if (r == DE_OK) {
        /* cs_probe_copy is loaded with the others (one entry list) but not used. */
        r = sky_load(dev, alloc, "probe_prefilter", probe_entries, 3,
                     (u32)sizeof(sky_filter_params), probe);
        ibl->pipe_down = probe[0];
        ibl->pipe_filter = probe[1];
        if (DE_HANDLE_VALID(probe[2]))
            de_gpu_pipeline_destroy(dev, probe[2]);
    }
    if (r != DE_OK) {
        de__sky_ibl_destroy(ibl);
        return r;
    }
    if (!DE_HANDLE_VALID(ibl->pipe_cube) || !DE_HANDLE_VALID(ibl->pipe_down) ||
        !DE_HANDLE_VALID(ibl->pipe_filter)) {
        *out = ibl; /* no code for this backend: not ready */
        return DE_OK;
    }
    de_gpu_texture_desc desc = {.format = DE_FMT_RGBA16_FLOAT,
                                .width = DE__SKY_IBL_SIZE,
                                .height = DE__SKY_IBL_SIZE * 6u,
                                .mip_count = DE__SKY_IBL_MIPS,
                                .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                                .name = "sky.ibl_raw"};
    ibl->raw = de_gpu_texture_create(dev, &desc);
    desc.name = "sky.ibl_filtered";
    ibl->filtered = de_gpu_texture_create(dev, &desc);
    desc.usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC; /* tests read it back */
    desc.name = "sky.ibl_strip0";
    ibl->strip[0] = de_gpu_texture_create(dev, &desc);
    desc.name = "sky.ibl_strip1";
    ibl->strip[1] = de_gpu_texture_create(dev, &desc);
    u64 at = 0;
    for (u32 m = 0; m < DE__SKY_IBL_MIPS; ++m) {
        const u32 n = DE__SKY_IBL_SIZE >> m;
        ibl->mip_pitch[m] = (n * 8u + 511u) & ~511u;
        ibl->mip_offset[m] = at;
        at += (u64)ibl->mip_pitch[m] * n * 6u;
        at = (at + 511u) & ~(u64)511u;
    }
    ibl->transfer = de_gpu_buffer_create(
        dev, &(de_gpu_buffer_desc){.size = at,
                                   .usage = DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                                   .name = "sky.ibl_transfer"});
    ibl->readback = de_gpu_buffer_create(
        dev, &(de_gpu_buffer_desc){.size = (u64)SKY_IBL_SH_PITCH * SKY_IBL_SH_N * 6u,
                                   .usage = DE_USAGE_COPY_DST,
                                   .memory = DE_MEM_READBACK,
                                   .name = "sky.ibl_sh_readback"});
    ibl->fence = de_gpu_fence_create(dev, 0);
    if (!DE_HANDLE_VALID(ibl->raw) || !DE_HANDLE_VALID(ibl->filtered) ||
        !DE_HANDLE_VALID(ibl->strip[0]) || !DE_HANDLE_VALID(ibl->strip[1]) ||
        !DE_HANDLE_VALID(ibl->transfer) || !DE_HANDLE_VALID(ibl->readback) ||
        !DE_HANDLE_VALID(ibl->fence)) {
        de__sky_ibl_destroy(ibl);
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "sky IBL: the textures, buffers or fence");
    }
    ibl->raw_access = ibl->filtered_access = DE_ACCESS_NONE;
    /* New GPU-only storage can reuse a retired transfer-source heap range (probe_capture.c). */
    ibl->transfer_access = DE_ACCESS_COPY_SRC;
    ibl->ready = true;
    *out = ibl;
    return DE_OK;
}

bool de__sky_ibl_ready(const de__sky_ibl *ibl) {
    return ibl != NULL && ibl->ready;
}

bool de__sky_ibl_pending(const de__sky_ibl *ibl) {
    return ibl != NULL && ibl->pending;
}

bool de__sky_ibl_wants(const de__sky_ibl *ibl, const de__sky_snapshot *sn, f32 r_km) {
    if (!de__sky_ibl_ready(ibl) || sn == NULL || !sn->valid)
        return false;
    return de__sky_ibl_stale(&ibl->last, ibl->last_r_km, sn, r_km);
}

static void sky_filter_dispatch(de__sky_ibl *ibl, de_gpu_cmd *cmd, de_gpu_pipeline pipe,
                                sky_filter_params pc, u32 width, u32 height) {
    for (u32 m = 0; m < DE__SKY_IBL_MIPS; ++m)
        pc.raw_mips[m] = de_gpu_texture_bindless(ibl->dev, ibl->raw, m, true);
    de_gpu_cmd_set_pipeline(cmd, pipe);
    de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
    de_gpu_cmd_dispatch(cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1);
}

de_result de__sky_ibl_generate(de__sky_ibl *ibl, const de__sky_luts *luts,
                               const de__sky_snapshot *sn, f32 r_km) {
    if (!de__sky_ibl_ready(ibl) || !de__sky_luts_ready(luts) || sn == NULL || !sn->valid)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__sky_ibl_generate: not ready");
    if (ibl->pending)
        return DE_FAIL(DE_ERR_BUSY, "de__sky_ibl_generate: a generation is in flight");
    de_gpu_device *dev = ibl->dev;
    const u32 back = ibl->have_current ? 1u - ibl->current : 0u;
    const de_gpu_texture strip = ibl->strip[back];
    de_gpu_cmd *c = de_gpu_cmd_begin(dev, DE_QUEUE_GRAPHICS, "sky.ibl");
    if (c == NULL)
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "sky IBL: no command list");
    const u32 rw = DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE;
    de_gpu_barrier b[2];

    /* 1. The sky into the raw strip's mip 0. */
    b[0] = sky_tex_barrier(ibl->raw, ibl->raw_access, DE_ACCESS_SHADER_WRITE);
    de_gpu_cmd_barriers(c, b, 1);
    sky_cube_params cp;
    memset(&cp, 0, sizeof cp);
    cp.idx[0] = de_gpu_texture_bindless(dev, ibl->raw, 0, true);
    cp.idx[1] = de__sky_luts_transmittance_bindless(luts);
    cp.idx[2] = de__sky_luts_multiscatter_bindless(luts);
    cp.idx[3] = de__sky_luts_sampler_bindless(luts);
    cp.sun[0] = sn->sun_dir.x, cp.sun[1] = sn->sun_dir.y, cp.sun[2] = sn->sun_dir.z;
    cp.sun[3] = sn->sun_weight;
    cp.moon[0] = sn->moon_dir.x, cp.moon[1] = sn->moon_dir.y, cp.moon[2] = sn->moon_dir.z;
    cp.moon[3] = sn->moon_weight;
    cp.misc[0] = r_km;
    cp.misc[1] = sn->mie_scale;
    cp.misc[2] = sn->albedo;
    cp.misc[3] = SKY_IBL_STEPS;
    cp.size[0] = DE__SKY_IBL_SIZE;
    de_gpu_cmd_set_pipeline(c, ibl->pipe_cube);
    de_gpu_cmd_push_constants(c, &cp, (u32)sizeof cp);
    de_gpu_cmd_dispatch(c, DE__SKY_IBL_SIZE / 8u, DE__SKY_IBL_SIZE * 6u / 8u, 1);
    b[0] = sky_tex_barrier(ibl->raw, DE_ACCESS_SHADER_WRITE, rw);
    de_gpu_cmd_barriers(c, b, 1);

    /* 2. The box-filtered chain, then the GGX prefilter (probe_capture.c's sequence). */
    for (u32 m = 1; m < DE__SKY_IBL_MIPS; ++m) {
        sky_filter_params pc;
        memset(&pc, 0, sizeof pc);
        pc.src = de_gpu_texture_bindless(dev, ibl->raw, m - 1u, true);
        pc.dst = de_gpu_texture_bindless(dev, ibl->raw, m, true);
        pc.size = DE__SKY_IBL_SIZE >> m;
        pc.mip = m;
        sky_filter_dispatch(ibl, c, ibl->pipe_down, pc, pc.size, pc.size * 6u);
        b[0] = sky_tex_barrier(ibl->raw, rw, rw);
        de_gpu_cmd_barriers(c, b, 1);
    }
    b[0] = sky_tex_barrier(ibl->filtered, ibl->filtered_access, DE_ACCESS_SHADER_WRITE);
    de_gpu_cmd_barriers(c, b, 1);
    for (u32 m = 0; m < DE__SKY_IBL_MIPS; ++m) {
        sky_filter_params pc;
        memset(&pc, 0, sizeof pc);
        pc.dst = de_gpu_texture_bindless(dev, ibl->filtered, m, true);
        pc.size = DE__SKY_IBL_SIZE;
        pc.mip = m;
        pc.mips = DE__SKY_IBL_MIPS;
        pc.samples = SKY_IBL_SAMPLES;
        const u32 n = DE__SKY_IBL_SIZE >> m;
        sky_filter_dispatch(ibl, c, ibl->pipe_filter, pc, n, n * 6u);
    }

    /* 3. The prefiltered strip into the back sampled strip through the transfer buffer. */
    b[0] = sky_tex_barrier(ibl->filtered, DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_SRC);
    b[1] = sky_buf_barrier(ibl->transfer, ibl->transfer_access, DE_ACCESS_COPY_DST);
    de_gpu_cmd_barriers(c, b, 2);
    for (u32 m = 0; m < DE__SKY_IBL_MIPS; ++m)
        de_gpu_cmd_copy_texture_to_buffer(c, ibl->transfer, ibl->mip_offset[m], ibl->mip_pitch[m],
                                          ibl->filtered, m, 0);
    b[0] = sky_buf_barrier(ibl->transfer, DE_ACCESS_COPY_DST, DE_ACCESS_COPY_SRC);
    b[1] = sky_tex_barrier(strip, ibl->strip_written[back] ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE,
                           DE_ACCESS_COPY_DST);
    de_gpu_cmd_barriers(c, b, 2);
    for (u32 m = 0; m < DE__SKY_IBL_MIPS; ++m)
        de_gpu_cmd_copy_buffer_to_texture(c, strip, m, 0, ibl->transfer, ibl->mip_offset[m],
                                          ibl->mip_pitch[m]);
    b[0] = sky_tex_barrier(strip, DE_ACCESS_COPY_DST, DE_ACCESS_SHADER_READ);
    de_gpu_cmd_barriers(c, b, 1);
    ibl->transfer_access = DE_ACCESS_COPY_SRC;
    ibl->filtered_access = DE_ACCESS_COPY_SRC;

    /* 4. The SH mip of the radiance to the readback buffer. */
    b[0] = sky_tex_barrier(ibl->raw, rw, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, b, 1);
    de_gpu_cmd_copy_texture_to_buffer(c, ibl->readback, 0, SKY_IBL_SH_PITCH, ibl->raw,
                                      DE__SKY_IBL_SH_MIP, 0);
    ibl->raw_access = DE_ACCESS_COPY_SRC;

    const de_gpu_submit s = {
        .cmds = &c, .cmd_count = 1, .signal = ibl->fence, .signal_value = ibl->fence_value + 1u};
    const de_result r = de_gpu_queue_submit(dev, DE_QUEUE_GRAPHICS, &s);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "sky IBL: the submission");
    ++ibl->fence_value;
    ibl->strip_written[back] = true;
    ibl->pending = true;
    ibl->last = *sn;
    ibl->last_r_km = r_km;
    return DE_OK;
}

bool de__sky_ibl_poll(de__sky_ibl *ibl, bool wait) {
    if (ibl == NULL || !ibl->pending)
        return false;
    if (wait)
        (void)de_gpu_fence_wait(ibl->dev, ibl->fence, ibl->fence_value, UINT64_MAX);
    if (de_gpu_fence_value(ibl->dev, ibl->fence) < ibl->fence_value)
        return false;
    ibl->pending = false;
    const u16 *texels = (const u16 *)de_gpu_buffer_map(ibl->dev, ibl->readback);
    if (texels == NULL)
        return false; /* keeps the previous environment */
    de__sky_ibl_project_sh(texels, SKY_IBL_SH_N, SKY_IBL_SH_PITCH / 8u, ibl->sh9);
    ibl->current = ibl->have_current ? 1u - ibl->current : 0u;
    ibl->have_current = true;
    return true;
}

void de__sky_ibl_current(const de__sky_ibl *ibl, de__ibl_sky *out) {
    if (out == NULL)
        return;
    memset(out, 0, sizeof *out);
    if (ibl == NULL || !ibl->have_current)
        return;
    out->valid = true;
    out->strip = ibl->strip[ibl->current];
    out->face_size = DE__SKY_IBL_SIZE;
    out->strip_mips = DE__SKY_IBL_MIPS;
    out->mip_per_roughness = (f32)(DE__SKY_IBL_MIPS - 1u);
    memcpy(out->sh9, ibl->sh9, sizeof out->sh9);
}

#endif /* DE_HAS_RENDER */
