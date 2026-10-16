#include "render/probes/probes_internal.h"
#include "render/formats/de_cooked_env.h"
#include <math.h>
#include <string.h>
void de__probe_face_camera(de_dvec3 position, de_dvec3 origin, u32 face, de__rw_view *out) {
    memset(out, 0, sizeof *out);
    out->camera.fov_y = 1.5707963267948966f;
    out->camera.near_z = 0.05f;
    /* 1/(1.2*2^EV)=1: capture stores absolute radiance. */
    out->camera.exposure_ev100 = -0.2630344058337938f;
    out->offset = de_dv3_relative(position, origin);
    f32 forward[3], right[3], down[3];
    de_cooked_env_face_direction(face, 0, 0, forward);
    de_cooked_env_face_direction(face, 1, 0, right);
    de_cooked_env_face_direction(face, 0, 1, down);
    for (u32 k = 0; k < 3; ++k) {
        out->transform.m[k] = right[k] - forward[k];
        out->transform.m[3 + k] = forward[k] - down[k];
        out->transform.m[6 + k] = -forward[k];
    }
    out->transform.m[9] = out->offset.x;
    out->transform.m[10] = out->offset.y;
    out->transform.m[11] = out->offset.z;
    out->rotation = de_quat_identity();
}
f32 de__probe_weight(const DeReflectionProbe *p, de_vec3 x) {
    f32 d;
    if (p->shape == DE_PROBE_SPHERE)
        d = p->extent.x - sqrtf(x.x * x.x + x.y * x.y + x.z * x.z);
    else
        d = fminf(p->extent.x - fabsf(x.x),
                  fminf(p->extent.y - fabsf(x.y), p->extent.z - fabsf(x.z)));
    if (d <= 0)
        return 0;
    if (p->blend_distance <= 0)
        return 1;
    return fminf(1, d / p->blend_distance);
}
de_vec3 de__probe_parallax(const DeReflectionProbe *p, de_vec3 x, de_vec3 d) {
    if (!p->box_projection)
        return d;
    f32 t = INFINITY;
    if (p->shape == DE_PROBE_SPHERE) {
        f32 a = d.x * d.x + d.y * d.y + d.z * d.z, b = x.x * d.x + x.y * d.y + x.z * d.z;
        f32 c = x.x * x.x + x.y * x.y + x.z * x.z - p->extent.x * p->extent.x;
        if (a > 0 && b * b - a * c >= 0)
            t = (-b + sqrtf(b * b - a * c)) / a;
    } else {
        const f32 pos[] = {x.x, x.y, x.z}, dir[] = {d.x, d.y, d.z},
                  e[] = {p->extent.x, p->extent.y, p->extent.z};
        for (u32 i = 0; i < 3; ++i)
            if (fabsf(dir[i]) > 1e-8f) {
                f32 v = ((dir[i] > 0 ? e[i] : -e[i]) - pos[i]) / dir[i];
                if (v >= 0)
                    t = fminf(t, v);
            }
    }
    return isfinite(t) ? (de_vec3){x.x + t * d.x, x.y + t * d.y, x.z + t * d.z} : d;
}

#include "render/renderer_internal.h"
#include "render/lighting/lighting.h"
#include <stdio.h>
typedef struct probe_filter_params {
    u32 src, dst, size, face, mip, mips, sampler, samples;
    u32 raw_mips[12];
} probe_filter_params;
DE_STATIC_ASSERT(sizeof(probe_filter_params) == 80, "ProbeFilterParams is 80 bytes");
static void capture_barrier(de_gpu_cmd *cmd, de_gpu_texture texture, u32 before, u32 after) {
    const de_gpu_barrier b = {.texture = texture, .before = before, .after = after};
    de_gpu_cmd_barriers(cmd, &b, 1);
}
static void filter_dispatch(de__probe_capture *c, de_gpu_cmd *cmd, const de__vis_kernel *kernel,
                            probe_filter_params pc, u32 width, u32 height) {
    for (u32 m = 0; m < c->mips; ++m)
        pc.raw_mips[m] = de_gpu_texture_bindless(c->renderer->dev, c->raw, m, true);
    de_gpu_cmd_set_pipeline(cmd, kernel->pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
}
de_result de__probe_capture_init(de__probe_capture *c, de_renderer *r, u32 resolution) {
    memset(c, 0, sizeof *c);
    c->renderer = r;
    c->resolution = resolution;
    for (u32 n = resolution; n; n >>= 1)
        ++c->mips;
    de__vis_shared *vis = de__views_vis(r);
    if (!vis || !vis->enabled)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "probe capture: no renderer shaders");
    const char *entries[] = {"cs_probe_copy", "cs_probe_downsample", "cs_probe_prefilter"};
    de__engine_shader_code code;
    DE_TRY(de__vis_shader_load(vis, "probe_prefilter", entries, 3, &code));
    const u32 group[] = {8, 8, 1};
    de__vis_kernel *kernels[] = {&c->copy, &c->downsample, &c->prefilter};
    de_result result = DE_OK;
    for (u32 i = 0; i < 3 && result == DE_OK; ++i)
        result = de__vis_kernel_create(vis, &code, i, entries[i], group,
                                       sizeof(probe_filter_params), kernels[i]);
    de__engine_shader_code_free(&code);
    if (result != DE_OK)
        goto fail;
    de_gpu_texture_desc desc = {.format = DE_FMT_RGBA16_FLOAT,
                                .width = resolution,
                                .height = resolution,
                                .usage =
                                    DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                .name = "probe.face"};
    for (u32 i = 0; i < 6; ++i) {
        c->targets[i] = de_gpu_texture_create(r->dev, &desc);
        if (!DE_HANDLE_VALID(c->targets[i])) {
            result = DE_ERR_OUT_OF_MEMORY;
            goto fail;
        }
        char name[32];
        (void)snprintf(name, sizeof name, "probe.capture.%u", i);
        c->views[i] = de_view_create(
            r, &(de_view_desc){.target = c->targets[i], .order = INT32_MIN + (i32)i, .name = name});
        if (!DE_HANDLE_VALID(c->views[i])) {
            result = DE_ERR_OUT_OF_MEMORY;
            goto fail;
        }
        de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
        settings.reflections = DE_QUALITY_OFF;
        settings.ao = DE_QUALITY_OFF;
        settings.gi = DE_QUALITY_OFF;
        settings.upscaler = DE_UPSCALE_NONE;
        settings.render_scale = 1;
        de_view_set_settings(r, c->views[i], &settings);
    }
    desc.height = resolution * 6;
    desc.mip_count = c->mips;
    desc.usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST;
    desc.name = "probe.raw_radiance";
    c->raw = de_gpu_texture_create(r->dev, &desc);
    desc.name = "probe.filtered_scratch";
    c->filtered = de_gpu_texture_create(r->dev, &desc);
    c->transfer_bytes = (u64)resolution * resolution * 6 * 8 + 4096;
    c->transfer = de_gpu_buffer_create(
        r->dev, &(de_gpu_buffer_desc){.size = c->transfer_bytes,
                                      .usage = DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                                      .name = "probe.transfer"});
    c->sampler = de_gpu_sampler_create(r->dev, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                                      .mag = DE_FILTER_LINEAR,
                                                                      .mip = DE_FILTER_LINEAR,
                                                                      .u = DE_ADDR_CLAMP,
                                                                      .v = DE_ADDR_CLAMP,
                                                                      .w = DE_ADDR_CLAMP});
    if (!DE_HANDLE_VALID(c->raw) || !DE_HANDLE_VALID(c->filtered) ||
        !DE_HANDLE_VALID(c->transfer) || !DE_HANDLE_VALID(c->sampler)) {
        result = DE_ERR_OUT_OF_MEMORY;
        goto fail;
    }
    c->initialized = true;
    return DE_OK;
fail:
    de__probe_capture_destroy(c);
    return DE_FAIL(result, "probe capture: initialization");
}
void de__probe_capture_destroy(de__probe_capture *c) {
    if (!c || !c->renderer)
        return;
    de_renderer *r = c->renderer;
    for (u32 i = 0; i < 6; ++i) {
        de_view_destroy(r, c->views[i]);
        de_gpu_texture_destroy(r->dev, c->targets[i]);
    }
    de__vis_shared *vis = de__views_vis(r);
    de__vis_kernel_destroy(vis, &c->copy);
    de__vis_kernel_destroy(vis, &c->downsample);
    de__vis_kernel_destroy(vis, &c->prefilter);
    de_gpu_texture_destroy(r->dev, c->raw);
    de_gpu_texture_destroy(r->dev, c->filtered);
    de_gpu_buffer_destroy(r->dev, c->transfer);
    de_gpu_sampler_destroy(r->dev, c->sampler);
    memset(c, 0, sizeof *c);
}
void de__probe_capture_at_position(de__probe_capture *c, de_dvec3 position, de_dvec3 origin,
                                   u32 mask) {
    if (!c || !c->initialized)
        return;
    c->scheduled = mask & 63u;
    for (u32 i = 0; i < 6; ++i) {
        de__rw_view camera;
        de__probe_face_camera(position, origin, i, &camera);
        camera.camera.view = c->views[i];
        de__view_info info = {0};
        (void)de__view_info_get(c->renderer, c->views[i], &info);
        c->expected_executed[i] = info.executed;
        de__view_capture_camera(c->renderer, c->views[i], (mask & (1u << i)) ? &camera : NULL);
    }
}
/* Prefilter scratch stays storage-only. A GPU buffer copies its exact bytes into the
 * canonical cube and a sampled-only mirror, so each bindless view has one consistent layout. */
static void strip_to_cube(de__probe_capture *c, de__probe_store *s, de__probe_handle h,
                          de_gpu_cmd *cmd) {
    const de__probe_slot *slot = &s->slots[h.slot];
    capture_barrier(cmd, c->filtered, c->filtered_access, DE_ACCESS_COPY_SRC);
    capture_barrier(cmd, slot->strip, slot->ready ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE,
                    DE_ACCESS_COPY_DST);
    capture_barrier(cmd, s->cube, s->cube_access, DE_ACCESS_COPY_DST);
    for (u32 m = 0; m < slot->mips; ++m) {
        u32 n = slot->resolution >> m, pitch = (n * 8 + 511u) & ~511u;
        const de_gpu_barrier before = {
            .buffer = c->transfer, .before = c->transfer_access, .after = DE_ACCESS_COPY_DST};
        de_gpu_cmd_barriers(cmd, &before, 1);
        de_gpu_cmd_copy_texture_to_buffer(cmd, c->transfer, 0, pitch, c->filtered, m, 0);
        const de_gpu_barrier after = {
            .buffer = c->transfer, .before = DE_ACCESS_COPY_DST, .after = DE_ACCESS_COPY_SRC};
        de_gpu_cmd_barriers(cmd, &after, 1);
        c->transfer_access = DE_ACCESS_COPY_SRC;
        for (u32 face = 0; face < 6; ++face)
            de_gpu_cmd_copy_buffer_to_texture(cmd, s->cube, m + slot->mip_base, h.slot * 6 + face,
                                              c->transfer, (u64)face * n * pitch, pitch);
        de_gpu_cmd_copy_buffer_to_texture(cmd, slot->strip, m, 0, c->transfer, 0, pitch);
    }
    capture_barrier(cmd, c->filtered, DE_ACCESS_COPY_SRC, DE_ACCESS_SHADER_WRITE);
    c->filtered_access = DE_ACCESS_SHADER_WRITE;
    capture_barrier(cmd, slot->strip, DE_ACCESS_COPY_DST, DE_ACCESS_SHADER_READ);
    capture_barrier(cmd, s->cube, DE_ACCESS_COPY_DST, DE_ACCESS_SHADER_READ);
}
de_result de__probe_capture_complete(de__probe_capture *c, de__probe_store *s, de__probe_handle h,
                                     bool filter) {
    if (!c || !c->initialized ||
        (filter && (!de__probe_store_valid(s, h) || s->slots[h.slot].resolution != c->resolution)))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "probe capture: invalid store handle");
    for (u32 f = 0; f < 6; ++f)
        if (c->scheduled & (1u << f)) {
            de__view_info info = {0};
            if (!de__view_info_get(c->renderer, c->views[f], &info) ||
                info.executed != c->expected_executed[f] + 1)
                return DE_FAIL(DE_ERR_INTERNAL, "probe capture: face view failed");
        }
    de_gpu_device *dev = c->renderer->dev;
    de_gpu_cmd *cmd = de_gpu_cmd_begin(dev, DE_QUEUE_GRAPHICS, "probe.capture_prefilter");
    if (!cmd)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "probe capture: command list");
    if (!filter) {
        // Raw capture uses GPU copies only. Preserve unscheduled faces in the
        // transfer buffer, overwrite scheduled faces, then upload the full strip.
        // This also avoids binding global sampled/storage descriptor arrays for a
        // copy after a live main view. Uncaptured faces are never consumed.
        const u32 pitch = c->resolution * 8;
        capture_barrier(cmd, c->raw, c->raw_access, DE_ACCESS_COPY_SRC);
        de_gpu_barrier transfer = {
            .buffer = c->transfer, .before = c->transfer_access, .after = DE_ACCESS_COPY_DST};
        de_gpu_cmd_barriers(cmd, &transfer, 1);
        de_gpu_cmd_copy_texture_to_buffer(cmd, c->transfer, 0, pitch, c->raw, 0, 0);
        transfer.before = DE_ACCESS_COPY_DST;
        de_gpu_cmd_barriers(cmd, &transfer, 1); // order overlapping transfer writes
        for (u32 f = 0; f < 6; ++f)
            if (c->scheduled & (1u << f)) {
                capture_barrier(cmd, c->targets[f], DE_ACCESS_SHADER_READ, DE_ACCESS_COPY_SRC);
                de_gpu_cmd_copy_texture_to_buffer(cmd, c->transfer, (u64)f * c->resolution * pitch,
                                                  pitch, c->targets[f], 0, 0);
                capture_barrier(cmd, c->targets[f], DE_ACCESS_COPY_SRC, DE_ACCESS_SHADER_READ);
            }
        transfer.before = DE_ACCESS_COPY_DST;
        transfer.after = DE_ACCESS_COPY_SRC;
        de_gpu_cmd_barriers(cmd, &transfer, 1);
        c->transfer_access = DE_ACCESS_COPY_SRC;
        capture_barrier(cmd, c->raw, DE_ACCESS_COPY_SRC, DE_ACCESS_COPY_DST);
        de_gpu_cmd_copy_buffer_to_texture(cmd, c->raw, 0, 0, c->transfer, 0, pitch);
        capture_barrier(cmd, c->raw, DE_ACCESS_COPY_DST,
                        DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE);
    } else {
        capture_barrier(cmd, c->raw, c->raw_access, DE_ACCESS_SHADER_WRITE);
        for (u32 f = 0; f < 6; ++f)
            if (c->scheduled & (1u << f)) {
                probe_filter_params pc = {.src =
                                              de_gpu_texture_bindless(dev, c->targets[f], 0, false),
                                          .dst = de_gpu_texture_bindless(dev, c->raw, 0, true),
                                          .size = c->resolution,
                                          .face = f};
                filter_dispatch(c, cmd, &c->copy, pc, c->resolution, c->resolution);
            }
        capture_barrier(cmd, c->raw, DE_ACCESS_SHADER_WRITE,
                        DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE);
    }
    c->raw_access = DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE;
    if (filter) {
        for (u32 m = 1; m < c->mips; ++m) {
            capture_barrier(cmd, c->raw, c->raw_access,
                            DE_ACCESS_SHADER_WRITE | DE_ACCESS_SHADER_READ);
            probe_filter_params pc = {.src = de_gpu_texture_bindless(dev, c->raw, m - 1, true),
                                      .dst = de_gpu_texture_bindless(dev, c->raw, m, true),
                                      .size = c->resolution >> m,
                                      .mip = m};
            filter_dispatch(c, cmd, &c->downsample, pc, pc.size, pc.size * 6);
            capture_barrier(cmd, c->raw, DE_ACCESS_SHADER_WRITE | DE_ACCESS_SHADER_READ,
                            c->raw_access);
        }
        capture_barrier(cmd, c->filtered, c->filtered_access, DE_ACCESS_SHADER_WRITE);
        c->filtered_access = DE_ACCESS_SHADER_WRITE;
        for (u32 m = 0; m < c->mips; ++m) {
            probe_filter_params pc = {.src = 0,
                                      .dst = de_gpu_texture_bindless(dev, c->filtered, m, true),
                                      .size = c->resolution,
                                      .mip = m,
                                      .mips = c->mips,
                                      .sampler = de_gpu_sampler_bindless(dev, c->sampler),
                                      .samples = 1024};
            filter_dispatch(c, cmd, &c->prefilter, pc, c->resolution >> m,
                            (c->resolution >> m) * 6);
        }
        strip_to_cube(c, s, h, cmd);
        s->cube_access = DE_ACCESS_SHADER_READ;
    }
    de_result result =
        de_gpu_queue_submit(dev, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1});
    if (result == DE_OK && filter)
        s->slots[h.slot].ready = true;
    return result;
}

static u64 padded_bytes(u32 resolution) {
    u64 size = 0;
    for (u32 n = resolution; n; n >>= 1)
        size += (u64)((n * 8 + 511u) & ~511u) * n * 6;
    return size;
}
de_result de__probe_store_upload(de__probe_capture *c, de__probe_store *s, de__probe_handle h,
                                 const void *payload) {
    if (!payload || !de__probe_store_valid(s, h))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "probe upload: invalid slot");
    de_gpu_device *dev = c->renderer->dev;
    u64 size = padded_bytes(s->slots[h.slot].resolution);
    de_gpu_buffer upload =
        de_gpu_buffer_create(dev, &(de_gpu_buffer_desc){.size = size,
                                                        .memory = DE_MEM_UPLOAD,
                                                        .usage = DE_USAGE_COPY_SRC,
                                                        .name = "probe.bake_upload"});
    if (!DE_HANDLE_VALID(upload))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "probe upload: staging");
    u8 *dst = (u8 *)de_gpu_buffer_map(dev, upload);
    const u8 *src = (const u8 *)payload;
    u64 off = 0;
    if (!dst) {
        de_gpu_buffer_destroy(dev, upload);
        return DE_FAIL(DE_ERR_INTERNAL, "probe upload: map");
    }
    for (u32 n = s->slots[h.slot].resolution; n; n >>= 1) {
        u32 pitch = (n * 8 + 511u) & ~511u;
        for (u32 y = 0; y < 6 * n; ++y) {
            memcpy(dst + off + (u64)y * pitch, src, n * 8);
            src += n * 8;
        }
        off += (u64)pitch * n * 6;
    }
    de_gpu_cmd *cmd = de_gpu_cmd_begin(dev, DE_QUEUE_GRAPHICS, "probe.bake_upload");
    if (!cmd) {
        de_gpu_buffer_destroy(dev, upload);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "probe upload: command");
    }
    capture_barrier(cmd, s->cube, s->cube_access, DE_ACCESS_COPY_DST);
    capture_barrier(cmd, s->slots[h.slot].strip,
                    s->slots[h.slot].ready ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE,
                    DE_ACCESS_COPY_DST);
    off = 0;
    for (u32 m = 0; m < s->slots[h.slot].mips; ++m) {
        u32 n = s->slots[h.slot].resolution >> m, pitch = (n * 8 + 511u) & ~511u;
        for (u32 f = 0; f < 6; ++f)
            de_gpu_cmd_copy_buffer_to_texture(cmd, s->cube, m + s->slots[h.slot].mip_base,
                                              h.slot * 6 + f, upload, off + (u64)f * n * pitch,
                                              pitch);
        de_gpu_cmd_copy_buffer_to_texture(cmd, s->slots[h.slot].strip, m, 0, upload, off, pitch);
        off += (u64)pitch * n * 6;
    }
    capture_barrier(cmd, s->cube, DE_ACCESS_COPY_DST, DE_ACCESS_SHADER_READ);
    capture_barrier(cmd, s->slots[h.slot].strip, DE_ACCESS_COPY_DST, DE_ACCESS_SHADER_READ);
    de_result result =
        de_gpu_queue_submit(dev, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1});
    de_gpu_buffer_destroy(dev, upload);
    if (result == DE_OK) {
        s->slots[h.slot].ready = true;
        s->cube_access = DE_ACCESS_SHADER_READ;
    }
    return result;
}
de_result de__probe_store_readback(de__probe_capture *c, de__probe_store *s, de__probe_handle h,
                                   void *payload) {
    if (!payload || !de__probe_store_valid(s, h) || !s->slots[h.slot].ready)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "probe readback: unavailable slot");
    de_gpu_device *dev = c->renderer->dev;
    u64 size = padded_bytes(s->slots[h.slot].resolution);
    de_gpu_buffer rb =
        de_gpu_buffer_create(dev, &(de_gpu_buffer_desc){.size = size,
                                                        .memory = DE_MEM_READBACK,
                                                        .usage = DE_USAGE_COPY_DST,
                                                        .name = "probe.bake_readback"});
    if (!DE_HANDLE_VALID(rb))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "probe readback: staging");
    de_gpu_cmd *cmd = de_gpu_cmd_begin(dev, DE_QUEUE_GRAPHICS, "probe.bake_readback");
    if (!cmd) {
        de_gpu_buffer_destroy(dev, rb);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "probe readback: command");
    }
    capture_barrier(cmd, s->cube, DE_ACCESS_SHADER_READ, DE_ACCESS_COPY_SRC);
    u64 off = 0;
    for (u32 m = 0; m < s->slots[h.slot].mips; ++m) {
        u32 n = s->slots[h.slot].resolution >> m, pitch = (n * 8 + 511u) & ~511u;
        for (u32 f = 0; f < 6; ++f)
            de_gpu_cmd_copy_texture_to_buffer(cmd, rb, off + (u64)f * n * pitch, pitch, s->cube,
                                              m + s->slots[h.slot].mip_base, h.slot * 6 + f);
        off += (u64)pitch * n * 6;
    }
    capture_barrier(cmd, s->cube, DE_ACCESS_COPY_SRC, DE_ACCESS_SHADER_READ);
    de_result result =
        de_gpu_queue_submit(dev, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1});
    de_gpu_wait_idle(dev);
    const u8 *src = (const u8 *)de_gpu_buffer_map(dev, rb);
    u8 *dst = (u8 *)payload;
    off = 0;
    if (!src)
        result = DE_FAIL(DE_ERR_INTERNAL, "probe readback: map");
    else
        for (u32 n = s->slots[h.slot].resolution; n; n >>= 1) {
            u32 pitch = (n * 8 + 511u) & ~511u;
            for (u32 y = 0; y < n * 6; ++y) {
                memcpy(dst, src + off + (u64)y * pitch, n * 8);
                dst += n * 8;
            }
            off += (u64)pitch * n * 6;
        }
    de_gpu_buffer_destroy(dev, rb);
    return result;
}
