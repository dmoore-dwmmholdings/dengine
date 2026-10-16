#include "render/probes/irradiance_internal.h"
#include "render/formats/de_cooked_le.h"
#include <math.h>
#include <float.h>
#include <string.h>
#include "platform/de_atomic.h"
#include "gpu/gpu_backend.h"
static u64 gi_hash(const void *src, u64 size) {
    const u8 *b = src;
    u64 h = 14695981039346656037ull;
    for (u64 i = 0; i < size; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}
static void gi_put64(u8 *p, u64 v) {
    de_cooked_put_u32(p, (u32)v);
    de_cooked_put_u32(p + 4, (u32)(v >> 32));
}
static u64 gi_get64(const u8 *p) {
    return de_cooked_get_u32(p) | ((u64)de_cooked_get_u32(p + 4) << 32);
}
de_result de__gi_layout_make(const DeIrradianceVolume *v, de__gi_layout *out) {
    if (!v || !out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GI: null layout");
    memset(out, 0, sizeof *out);
    const f32 e[3] = {v->extent.x, v->extent.y, v->extent.z};
    f32 spacing = v->spacing == 0 ? 2 : v->spacing;
    if (!isfinite(spacing) || spacing <= 0 || !isfinite(v->intensity) || v->intensity < 0 ||
        !isfinite(v->blend_distance) || v->blend_distance < 0 || v->bounces > 8)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GI: invalid settings");
    de__gi_layout l = {0};
    u64 total = 1;
    for (u32 i = 0; i < 3; ++i) {
        if (!isfinite(e[i]) || e[i] <= 0 || e[i] > 100000)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GI: invalid extent");
        f64 derived = fmax(2, ceil(2 * (f64)e[i] / spacing));
        if (!v->counts[i] && derived > DE__GI_PROBES)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GI: grid exceeds cap");
        l.counts[i] = v->counts[i] ? v->counts[i] : (u32)derived;
        if (l.counts[i] < 2 || l.counts[i] > DE__GI_PROBES)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GI: invalid counts");
        total *= l.counts[i];
        if (total > DE__GI_PROBES)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GI: probe cap4096");
    }
    l.probes = (u32)total;
    l.columns = l.probes < 64 ? l.probes : 64;
    l.rows = (l.probes + l.columns - 1) / l.columns;
    l.iw = l.columns * 10;
    l.ih = l.rows * 10;
    l.dw = l.columns * 18;
    l.dh = l.rows * 18;
    f32 max_step = 0;
    for (u32 i = 0; i < 3; ++i) {
        f32 step = 2 * e[i] / (f32)l.counts[i];
        if (!isnormal(step) || step < 1e-6f)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GI: grid step below one micrometre");
        max_step = fmaxf(max_step, step);
    }
    /* An exact power-of-two metre range avoids cross-host sqrt rounding in shipping bakes.
     * Twice the L1 half-extent bounds every grid pair; add one actual grid step for bias. */
    f64 bound = 2 * ((f64)e[0] + e[1] + e[2]) + max_step;
    l.range = 1;
    while ((f64)l.range < bound)
        l.range *= 2;
    l.bounces = v->bounces ? v->bounces : 2;
    l.payload_bytes = ((u64)l.iw * l.ih + (u64)l.dw * l.dh) * 4 + (u64)l.probes * 16;
    *out = l;
    return DE_OK;
}
u64 de__gi_settings_hash(const de__irradiance_snapshot *v) {
    u8 b[112] = {0};
    u32 off = 4;
    de_cooked_put_u32(b, 0x32434347u); /* cell-centered, normal-only packed quantizer v2 */
    const f64 p[3] = {v->position.x, v->position.y, v->position.z};
    for (u32 i = 0; i < 3; ++i) {
        u64 x;
        memcpy(&x, &p[i], 8);
        gi_put64(b + off, x);
        off += 8;
    }
    const f32 a[] = {v->rotation.x,           v->rotation.y,      v->rotation.z,
                     v->rotation.w,           v->volume.extent.x, v->volume.extent.y,
                     v->volume.extent.z,      v->volume.spacing,  v->volume.intensity,
                     v->volume.blend_distance};
    for (u32 i = 0; i < sizeof a / sizeof a[0]; ++i) {
        u32 x;
        memcpy(&x, &a[i], 4);
        de_cooked_put_u32(b + off, x);
        off += 4;
    }
    for (u32 i = 0; i < 3; ++i) {
        de_cooked_put_u32(b + off, v->volume.counts[i]);
        off += 4;
    }
    de_cooked_put_u32(b + off, v->volume.bounces ? v->volume.bounces : 2);
    off += 4;
    de_cooked_put_u32(b + off, (u32)v->volume.priority);
    off += 4;
    return gi_hash(b, off);
}
static bool gi_layout_valid(const de__gi_layout *l) {
    if (!l || l->counts[0] < 2 || l->counts[1] < 2 || l->counts[2] < 2)
        return false;
    DeIrradianceVolume v = {.extent = {1, 1, 1}, .intensity = 1, .bounces = l->bounces};
    memcpy(v.counts, l->counts, sizeof v.counts);
    de__gi_layout canonical;
    if (de__gi_layout_make(&v, &canonical) != DE_OK || !l->bounces)
        return false;
    int exponent = 0;
    return l->probes == canonical.probes && l->columns == canonical.columns &&
           l->rows == canonical.rows && l->iw == canonical.iw && l->ih == canonical.ih &&
           l->dw == canonical.dw && l->dh == canonical.dh &&
           l->payload_bytes == canonical.payload_bytes && isfinite(l->range) && l->range >= 1 &&
           l->range <= 1048576 && frexpf(l->range, &exponent) == .5f;
}
static bool gi_payload_valid(const u8 *data, const de__gi_layout *l) {
    u64 irradiance = (u64)l->iw * l->ih, moments = (u64)l->dw * l->dh;
    for (u64 i = 0; i < irradiance; ++i) {
        u32 value = de_cooked_get_u32(data + i * 4);
        if (((value >> 6) & 31u) == 31u || ((value >> 17) & 31u) == 31u ||
            ((value >> 27) & 31u) == 31u)
            return false;
    }
    const u8 *d = data + irradiance * 4;
    for (u64 i = 0; i < moments * 2; ++i) {
        u16 h = (u16)(d[i * 2] | (u16)d[i * 2 + 1] << 8);
        // Normalized distances/moments lie in [0,1]; positive half encoding is monotonic.
        if (h > 0x3c00u)
            return false;
    }
    d += moments * 4;
    for (u64 i = 0; i < (u64)l->probes * 4; ++i) {
        u32 bits = de_cooked_get_u32(d + i * 4);
        f32 x;
        memcpy(&x, &bits, 4);
        if (!isfinite(x) || (i % 4 == 3 ? (x != 0 && x != 1) : fabsf(x) > l->range))
            return false;
    }
    return true;
}
de_result de__gi_blob_write(void *dst, u64 size, u64 hash, const de__gi_layout *l,
                            const void *payload) {
    if (!dst || !gi_layout_valid(l) || !payload || size < DE__GI_BLOB_HEADER ||
        size - DE__GI_BLOB_HEADER != l->payload_bytes || !gi_payload_valid(payload, l))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GI bake: payload");
    u8 *b = dst, *metadata = b + DE__PROBE_BLOB_HEADER;
    // Move first: callers commonly serialize an in-place readback beginning after the metadata.
    memmove(b + DE__GI_BLOB_HEADER, payload, (size_t)l->payload_bytes);
    memset(metadata, 0, 32);
    for (u32 i = 0; i < 3; ++i)
        de_cooked_put_u32(metadata + i * 4, l->counts[i]);
    de_cooked_put_u32(metadata + 12, l->bounces);
    u32 range;
    memcpy(&range, &l->range, 4);
    de_cooked_put_u32(metadata + 16, range);
    de_cooked_put_u32(metadata + 20, 1);
    return de__probe_envelope_write(dst, size, "DEGI", 1, hash, metadata, 32 + l->payload_bytes);
}
de_result de__gi_blob_read(const void *src, u64 size, u64 hash, de__gi_layout *l,
                           const void **payload) {
    if (payload)
        *payload = NULL;
    if (!src || !l || !payload || size < DE__GI_BLOB_HEADER)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GI bake: truncated");
    const void *body = NULL;
    u64 bytes = 0;
    u32 tag = 0;
    DE_TRY(de__probe_envelope_read(src, size, "DEGI", hash, &tag, &body, &bytes));
    const u8 *b = body;
    if (tag != 1 || bytes < 32 || de_cooked_get_u32(b + 20) != 1 || gi_get64(b + 24))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GI bake: layout version");
    DeIrradianceVolume v = {.extent = {1, 1, 1}, .intensity = 1};
    for (u32 i = 0; i < 3; ++i) {
        v.counts[i] = de_cooked_get_u32(b + i * 4);
        if (v.counts[i] < 2)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GI bake: explicit counts required");
    }
    v.bounces = de_cooked_get_u32(b + 12);
    de__gi_layout parsed;
    if (de__gi_layout_make(&v, &parsed) != DE_OK || !v.bounces)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GI bake: grid");
    u32 bits = de_cooked_get_u32(b + 16);
    memcpy(&parsed.range, &bits, 4);
    if (!isfinite(parsed.range) || parsed.range < 1 || parsed.range > 1048576 ||
        (frexpf(parsed.range, &(int){0}) != .5f) || parsed.payload_bytes != bytes - 32 ||
        !gi_payload_valid(b + 32, &parsed))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GI bake: payload");
    *l = parsed;
    *payload = b + 32;
    return DE_OK;
}

#include "render/renderer_internal.h"
#include "render/lighting/lighting.h"
#include "render/graph/rg_internal.h"
#include "asset/slots_internal.h"
#include <stdio.h>
typedef struct gi_record {
    de__irradiance_snapshot snapshot;
    de__gi_layout layout;
    u64 hash;
    de_blob bake;
    de_gpu_texture irradiance[2], distance[2];
    de_gpu_buffer classification;
    u32 access[2], last, bounce, probe, faces, captures;
    bool live, ready, baked, load_checked;
} gi_record;
typedef struct gi_bake_params {
    u32 raw, distances, irradiance, moments, probe, columns, face, depth;
    f32 near_z, range;
    u32 operation, probes;
} gi_bake_params;
DE_STATIC_ASSERT(sizeof(gi_bake_params) == 48, "GiBakeParams 48 bytes");
struct de__irradiance {
    de_renderer *renderer;
    de__vis_shared *vis;
    gi_record records[DE__GI_VOLUMES];
    u32 order[DE__GI_VOLUMES], count, active;
    de__probe_capture capture;
    de_gpu_texture distances;
    u32 distance_access;
    de__vis_kernel integrate, depth, clear;
    de_atomic_u32 depth_recorded;
    u32 depth_success, skip_depth;
    de_gpu_buffer metadata[DE__VIEW_MAX][4];
    de__gi_gpu *mapped[DE__VIEW_MAX][4];
    de_cvar faces;
    u64 resource_drains;
    bool command_owner;
    de_dvec3 origin;
};
static de_result gi_bake_command(int argc, const char *const *argv, void *user);
static void gi_stage(de__view_frame *vf, void *user);
static void gi_barrier(de_gpu_cmd *cmd, de_gpu_texture texture, u32 before, u32 after);
static void gi_release(struct de__irradiance *s, gi_record *rec) {
    for (u32 i = 0; i < 2; ++i) {
        de_gpu_texture_destroy(s->renderer->dev, rec->irradiance[i]);
        de_gpu_texture_destroy(s->renderer->dev, rec->distance[i]);
    }
    de_gpu_buffer_destroy(s->renderer->dev, rec->classification);
    memset(rec, 0, sizeof *rec);
}
static de_gpu_cmd *gi_begin(struct de__irradiance *s, const char *name) {
    de_gpu_device *dev = s->renderer->dev;
    // Like the view path, standalone GI encoders must remove retired storage descriptors
    // before recording. Metal retains every live bindless storage view on an encoder, so
    // submit-time collection can otherwise release a resource that this new list references.
    // This conservative drain can stall streaming/baking; DENG-432 owns the global fix.
    if (de_gpu_get_caps(dev)->backend == DE_GPU_METAL && de__gpu_deferred_count(dev) != 0) {
        de_gpu_wait_idle(dev);
        ++s->resource_drains;
    }
    return de_gpu_cmd_begin(dev, DE_QUEUE_GRAPHICS, name);
}
u64 de__irradiance_resource_drains(const de_renderer *r) {
    return r && r->irradiance ? r->irradiance->resource_drains : 0;
}
static de_result gi_reserve(struct de__irradiance *s, gi_record *rec,
                            const de__irradiance_snapshot *snapshot) {
    de__gi_layout l;
    DE_TRY(de__gi_layout_make(&snapshot->volume, &l));
    u64 bytes = l.payload_bytes * 2;
    for (u32 i = 0; i < DE__GI_VOLUMES; ++i)
        if (&s->records[i] != rec)
            bytes += s->records[i].layout.payload_bytes * 2;
    if (bytes > 64ull * 1024 * 1024)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "GI: total atlas cap64MiB");
    gi_record candidate = {
        .snapshot = *snapshot, .layout = l, .hash = de__gi_settings_hash(snapshot), .live = true};
    de_gpu_texture_desc d = {.width = l.iw,
                             .height = l.ih,
                             .format = DE_FMT_R11G11B10_FLOAT,
                             .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                             .name = "gi.irradiance"};
    for (u32 i = 0; i < 2; ++i) {
        d.width = l.iw;
        d.height = l.ih;
        d.format = DE_FMT_R11G11B10_FLOAT;
        d.name = "gi.irradiance";
        candidate.irradiance[i] = de_gpu_texture_create(s->renderer->dev, &d);
        d.width = l.dw;
        d.height = l.dh;
        d.format = DE_FMT_RG16_FLOAT;
        d.name = "gi.distance_moments";
        candidate.distance[i] = de_gpu_texture_create(s->renderer->dev, &d);
        if (!candidate.irradiance[i].id || !candidate.distance[i].id)
            goto fail;
    }
    candidate.classification = de_gpu_buffer_create(
        s->renderer->dev, &(de_gpu_buffer_desc){.size = (u64)l.probes * 16,
                                                .memory = DE_MEM_UPLOAD,
                                                .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                .name = "gi.offset_classification"});
    if (!candidate.classification.id)
        goto fail;
    void *map = de_gpu_buffer_map(s->renderer->dev, candidate.classification);
    if (!map)
        goto fail;
    memset(map, 0, (size_t)l.probes * 16);
    de_gpu_cmd *cmd = gi_begin(s, "gi.initialize");
    if (!cmd)
        goto fail;
    // Establish every candidate storage image before the first bindless dispatch.
    // All storage descriptors are visible together, including the second atlas.
    for (u32 i = 0; i < 2; ++i) {
        gi_barrier(cmd, candidate.irradiance[i], DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE);
        gi_barrier(cmd, candidate.distance[i], DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE);
    }
    for (u32 i = 0; i < 2; ++i)
        for (u32 t = 0; t < 2; ++t) {
            de_gpu_texture texture = t ? candidate.distance[i] : candidate.irradiance[i];
            gi_bake_params pc = {.irradiance =
                                     de_gpu_texture_bindless(s->renderer->dev, texture, 0, true),
                                 .columns = t ? l.dw : l.iw,
                                 .probes = t ? l.dh : l.ih,
                                 .operation = t};
            de_gpu_cmd_set_pipeline(cmd, s->clear.pipeline);
            de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
            de_gpu_cmd_dispatch(cmd, (pc.columns + 7) / 8, (pc.probes + 7) / 8, 1);
            gi_barrier(cmd, texture, DE_ACCESS_SHADER_WRITE, DE__GI_ACCESS);
        }
    if (de_gpu_queue_submit(s->renderer->dev, DE_QUEUE_GRAPHICS,
                            &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}) != DE_OK)
        goto fail;
    candidate.access[0] = candidate.access[1] = DE__GI_ACCESS;
    gi_release(s, rec);
    *rec = candidate;
    return DE_OK;
fail:
    gi_release(s, &candidate);
    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "GI: atlas allocation");
}
static de__gi_gpu gi_gpu(const gi_record *rec, de_dvec3 origin, bool prior) {
    de__gi_gpu g = {0};
    de_quat rotation = de_quat_normalize(rec->snapshot.rotation);
    const de_vec3 axes[] = {de_quat_rotate(rotation, (de_vec3){1, 0, 0}),
                            de_quat_rotate(rotation, (de_vec3){0, 1, 0}),
                            de_quat_rotate(rotation, (de_vec3){0, 0, 1})};
    de_vec3 center = de_dv3_relative(rec->snapshot.position, origin);
    for (u32 i = 0; i < 3; ++i) {
        g.local[i][0] = axes[i].x;
        g.local[i][1] = axes[i].y;
        g.local[i][2] = axes[i].z;
        g.local[i][3] = -(axes[i].x * center.x + axes[i].y * center.y + axes[i].z * center.z);
    }
    const DeIrradianceVolume *v = &rec->snapshot.volume;
    g.extent[0] = v->extent.x;
    g.extent[1] = v->extent.y;
    g.extent[2] = v->extent.z;
    f32 stepmin = INFINITY;
    for (u32 i = 0; i < 3; ++i) {
        g.counts[i] = rec->layout.counts[i];
        g.step[i] = 2 * g.extent[i] / (f32)g.counts[i];
        stepmin = fminf(stepmin, g.step[i]);
    }
    g.step[3] = fminf(.02f, stepmin * .01f);
    g.extent[3] = g.step[3] * .5f;
    g.rotation[0] = rotation.x;
    g.rotation[1] = rotation.y;
    g.rotation[2] = rotation.z;
    g.rotation[3] = rotation.w;
    g.counts[3] = rec->layout.columns;
    g.textures[3] = 1; /* cell-centered placement */

    g.params[0] = v->intensity;
    g.params[1] = v->blend_distance;
    g.params[2] = rec->layout.range;
    g.params[3] = (prior ? rec->bounce > 0 : rec->ready) ? 1.0f : 0.0f;
    return g;
}
typedef struct gi_depth_pass {
    struct de__irradiance *s;
    de_rg_resource depth, distances;
    gi_bake_params pc;
} gi_depth_pass;
static void gi_depth_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    gi_depth_pass *p = user;
    p->pc.depth =
        de_gpu_texture_bindless(p->s->renderer->dev, de_rg_texture(pass, p->depth), 0, false);
    p->pc.distances =
        de_gpu_texture_bindless(p->s->renderer->dev, de_rg_texture(pass, p->distances), 0, true);
    de_gpu_cmd_set_pipeline(cmd, p->s->depth.pipeline);
    de_gpu_cmd_push_constants(cmd, &p->pc, sizeof p->pc);
    de_gpu_cmd_dispatch(cmd, 4, 4, 1);
    de_atomic_fetch_or_u32(&p->s->depth_recorded, 1u << p->pc.face, DE_ATOMIC_RELEASE);
}
static void gi_stage(de__view_frame *vf, void *user) {
    struct de__irradiance *s = user;
    u32 slot = (vf->view.id & 255u) - 1u;
    if (slot >= DE__VIEW_MAX)
        return;
    bool own_capture = false;
    if (s->active < DE__GI_VOLUMES)
        for (u32 face = 0; face < 6; ++face)
            if (vf->view.id == s->capture.views[face].id) {
                own_capture = true;
                const de__vis_context *vc =
                    de__vis_frame_context(s->vis, vf->view, vf->graph, vf->frame_index);
                if (vc && DE_HANDLE_VALID(vc->depth) && (s->capture.scheduled & (1u << face)) &&
                    !(s->skip_depth & (1u << face))) {
                    gi_depth_pass *p = de_arena_push_zero(vf->graph->arena, sizeof *p, 16);
                    if (!p)
                        return;
                    *p = (gi_depth_pass){.s = s,
                                         .depth = vc->depth,
                                         .distances = de_rg_import_texture(vf->graph, s->distances,
                                                                           s->distance_access),
                                         .pc = {.face = face,
                                                .near_z = .05f,
                                                .range = s->records[s->active].layout.range}};
                    de_rg_pass *pass = de_rg_add_pass(vf->graph, "gi.capture_depth",
                                                      DE_QUEUE_GRAPHICS, gi_depth_execute, p);
                    de_rg_read(pass, p->depth, DE_ACCESS_SHADER_READ);
                    de_rg_write(pass, p->distances, DE_ACCESS_SHADER_WRITE);
                    de_rg_export(vf->graph, p->distances, DE__GI_ACCESS);
                }
            }
    if (!own_capture && (vf->probe_capture || vf->settings->gi == DE_QUALITY_OFF))
        return;
    u32 ring = (u32)(vf->frame_index % 4);
    if (!s->metadata[slot][ring].id) {
        de_gpu_buffer b = de_gpu_buffer_create(s->renderer->dev,
                                               &(de_gpu_buffer_desc){.size = sizeof(de__gi_gpu) * 9,
                                                                     .usage = DE_USAGE_STORAGE,
                                                                     .memory = DE_MEM_UPLOAD,
                                                                     .name = "gi.volume_metadata"});
        de__gi_gpu *map = b.id ? de_gpu_buffer_map(s->renderer->dev, b) : NULL;
        if (!map) {
            de_gpu_buffer_destroy(s->renderer->dev, b);
            return;
        }
        s->metadata[slot][ring] = b;
        s->mapped[slot][ring] = map;
    }
    de__gi_gpu *map = s->mapped[slot][ring];
    memset(map, 0, sizeof(*map) * 9);
    de__lighting_inputs *in = de__lighting_view_inputs(de__views_lighting(s->renderer), slot);
    in->gi_records = de_rg_import_buffer(vf->graph, s->metadata[slot][ring], DE_ACCESS_SHADER_READ);
    in->gi_texture_count = 0;
    in->gi_buffer_count = 0;
    for (u32 i = 0; i < s->count; ++i) {
        gi_record *rec = &s->records[s->order[i]];
        if (!rec->layout.probes || !(own_capture ? rec->bounce > 0 : rec->ready))
            continue;
        u32 at = ++map[0].counts[0];
        map[at] = gi_gpu(rec, s->origin, own_capture);
        map[at].textures[0] =
            de_gpu_texture_bindless(s->renderer->dev, rec->irradiance[rec->last], 0, true);
        map[at].textures[1] =
            de_gpu_texture_bindless(s->renderer->dev, rec->distance[rec->last], 0, true);
        map[at].textures[2] = de_gpu_buffer_bindless(s->renderer->dev, rec->classification);
        in->gi_textures[in->gi_texture_count++] =
            de_rg_import_texture(vf->graph, rec->irradiance[rec->last], rec->access[rec->last]);
        in->gi_textures[in->gi_texture_count++] =
            de_rg_import_texture(vf->graph, rec->distance[rec->last], rec->access[rec->last]);
        in->gi_buffers[in->gi_buffer_count++] =
            de_rg_import_buffer(vf->graph, rec->classification, DE_ACCESS_SHADER_READ);
    }
}
de_result de__irradiance_create(de_renderer *r, struct de__irradiance **out) {
    *out = NULL;
    struct de__irradiance *s = DE_NEW(r->alloc, struct de__irradiance, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "GI: state");
    memset(s, 0, sizeof *s);
    s->renderer = r;
    s->vis = de__views_vis(r);
    s->active = DE__PROBE_NONE;
    s->faces = de_cvar_register(&(de_cvar_desc){.name = "r.gi_faces_per_frame",
                                                .type = DE_CVAR_INT,
                                                .min = 1,
                                                .max = 6,
                                                .default_number = 6});
    de_result result = DE_OK;
    if (s->vis && s->vis->enabled) {
        const char *entries[] = {"cs_gi_distance", "cs_gi_integrate", "cs_gi_clear"};
        de__engine_shader_code code;
        result = de__vis_shader_load(s->vis, "irradiance_bake", entries, 3, &code);
        if (result == DE_OK) {
            const u32 group[] = {8, 8, 1};
            result = de__vis_kernel_create(s->vis, &code, 0, entries[0], group,
                                           sizeof(gi_bake_params), &s->depth);
            if (result == DE_OK)
                result = de__vis_kernel_create(s->vis, &code, 1, entries[1], group,
                                               sizeof(gi_bake_params), &s->integrate);
            if (result == DE_OK)
                result = de__vis_kernel_create(s->vis, &code, 2, entries[2], group,
                                               sizeof(gi_bake_params), &s->clear);
            de__engine_shader_code_free(&code);
        }
    }
    if (result == DE_OK)
        result = de__view_register_stage(r, DE__VIEW_SLOT_LIGHTING, -15, "gi.volumes", gi_stage, s);
    if (result != DE_OK) {
        de__irradiance_destroy(s);
        return result;
    }
    s->command_owner = de_command_register("r.gi_bake", "Persist ready DDGI atlases to dev:/",
                                           gi_bake_command, s) == DE_OK;
    if (!s->command_owner)
        de_error_clear();
    *out = s;
    return DE_OK;
}
void de__irradiance_destroy(struct de__irradiance *s) {
    if (!s)
        return;
    de__view_unregister_stage(s->renderer, gi_stage, s);
    if (s->command_owner)
        de_command_unregister("r.gi_bake");
    de__probe_capture_destroy(&s->capture);
    de_gpu_texture_destroy(s->renderer->dev, s->distances);
    for (u32 i = 0; i < DE__GI_VOLUMES; ++i)
        gi_release(s, &s->records[i]);
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        for (u32 j = 0; j < 4; ++j)
            de_gpu_buffer_destroy(s->renderer->dev, s->metadata[i][j]);
    de__vis_kernel_destroy(s->vis, &s->depth);
    de__vis_kernel_destroy(s->vis, &s->integrate);
    de__vis_kernel_destroy(s->vis, &s->clear);
    DE_FREE(s->renderer->alloc, s, sizeof *s);
}
static void gi_barrier(de_gpu_cmd *cmd, de_gpu_texture texture, u32 before, u32 after) {
    const de_gpu_barrier b = {.texture = texture, .before = before, .after = after};
    de_gpu_cmd_barriers(cmd, &b, 1);
}
static u64 gi_staging_size(const de__gi_layout *l) {
    u64 a = (u64)((l->iw * 4 + 511u) & ~511u) * l->ih;
    return a + (u64)((l->dw * 4 + 511u) & ~511u) * l->dh;
}
static de_result gi_transfer(struct de__irradiance *s, gi_record *rec, void *payload, bool upload) {
    const de__gi_layout *l = &rec->layout;
    de_gpu_device *dev = s->renderer->dev;
    u64 bytes = gi_staging_size(l);
    de_gpu_buffer buffer = de_gpu_buffer_create(
        dev, &(de_gpu_buffer_desc){.size = bytes,
                                   .memory = upload ? DE_MEM_UPLOAD : DE_MEM_READBACK,
                                   .usage = upload ? DE_USAGE_COPY_SRC : DE_USAGE_COPY_DST,
                                   .name = upload ? "gi.bake_upload" : "gi.bake_readback"});
    if (!buffer.id)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "GI: staging");
    u8 *map = de_gpu_buffer_map(dev, buffer);
    u32 widths[] = {l->iw, l->dw}, heights[] = {l->ih, l->dh};
    if (!map) {
        de_gpu_buffer_destroy(dev, buffer);
        return DE_FAIL(DE_ERR_INTERNAL, "GI: staging map");
    }
    u64 off = 0, packed = 0;
    if (upload)
        for (u32 t = 0; t < 2; ++t) {
            u32 pitch = (widths[t] * 4 + 511u) & ~511u;
            for (u32 y = 0; y < heights[t]; ++y)
                memcpy(map + off + (u64)y * pitch, (u8 *)payload + packed + (u64)y * widths[t] * 4,
                       widths[t] * 4);
            off += (u64)pitch * heights[t];
            packed += (u64)widths[t] * heights[t] * 4;
        }
    de_gpu_buffer new_classification = {0};
    if (upload) {
        new_classification = de_gpu_buffer_create(
            dev, &(de_gpu_buffer_desc){.size = (u64)l->probes * 16,
                                       .memory = DE_MEM_UPLOAD,
                                       .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                       .name = "gi.bake_classification"});
        void *state = new_classification.id ? de_gpu_buffer_map(dev, new_classification) : NULL;
        if (!state) {
            de_gpu_buffer_destroy(dev, new_classification);
            de_gpu_buffer_destroy(dev, buffer);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "GI: classification replacement");
        }
        memcpy(state, (u8 *)payload + ((u64)l->iw * l->ih + (u64)l->dw * l->dh) * 4,
               (size_t)l->probes * 16);
    }
    de_gpu_cmd *cmd = gi_begin(s, upload ? "gi.load" : "gi.readback");
    if (!cmd) {
        de_gpu_buffer_destroy(dev, new_classification);
        de_gpu_buffer_destroy(dev, buffer);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "GI: transfer command");
    }
    de_gpu_texture textures[] = {rec->irradiance[rec->last], rec->distance[rec->last]};
    off = 0;
    for (u32 t = 0; t < 2; ++t) {
        u32 pitch = (widths[t] * 4 + 511u) & ~511u;
        gi_barrier(cmd, textures[t], rec->access[rec->last],
                   upload ? DE_ACCESS_COPY_DST : DE_ACCESS_COPY_SRC);
        if (upload)
            de_gpu_cmd_copy_buffer_to_texture(cmd, textures[t], 0, 0, buffer, off, pitch);
        else
            de_gpu_cmd_copy_texture_to_buffer(cmd, buffer, off, pitch, textures[t], 0, 0);
        gi_barrier(cmd, textures[t], upload ? DE_ACCESS_COPY_DST : DE_ACCESS_COPY_SRC,
                   DE__GI_ACCESS);
        off += (u64)pitch * heights[t];
    }
    de_result result =
        de_gpu_queue_submit(dev, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1});
    if (!upload) {
        de_gpu_wait_idle(dev);
        off = packed = 0;
        if (result == DE_OK)
            for (u32 t = 0; t < 2; ++t) {
                u32 pitch = (widths[t] * 4 + 511u) & ~511u;
                for (u32 y = 0; y < heights[t]; ++y)
                    memcpy((u8 *)payload + packed + (u64)y * widths[t] * 4,
                           map + off + (u64)y * pitch, widths[t] * 4);
                off += (u64)pitch * heights[t];
                packed += (u64)widths[t] * heights[t] * 4;
            }
    }
    u64 class_offset = ((u64)l->iw * l->ih + (u64)l->dw * l->dh) * 4;
    if (result == DE_OK && !upload) {
        const void *state = de_gpu_buffer_map(dev, rec->classification);
        memcpy((u8 *)payload + class_offset, state, (size_t)l->probes * 16);
    }
    if (result == DE_OK && upload) {
        de_gpu_buffer_destroy(dev, rec->classification);
        rec->classification = new_classification;
        new_classification = (de_gpu_buffer){0};
    }
    de_gpu_buffer_destroy(dev, new_classification);
    if (result == DE_OK)
        rec->access[rec->last] = DE__GI_ACCESS;
    de_gpu_buffer_destroy(dev, buffer);
    return result;
}
void de__irradiance_prepare(de_renderer *r, const de__render_world_frame *world) {
    struct de__irradiance *s = r->irradiance;
    if (!s)
        return;
    s->depth_success = 0;
    de_atomic_store_u32(&s->depth_recorded, 0, DE_ATOMIC_RELAXED);
    if (s->capture.initialized)
        de__probe_capture_at_position(&s->capture, (de_dvec3){0}, (de_dvec3){0}, 0);
    for (u32 i = 0; i < DE__GI_VOLUMES; ++i)
        s->records[i].live = false;
    s->count = 0;
    s->active = DE__PROBE_NONE;
    if (!world)
        return;
    s->origin = world->origin;
    for (u32 n = 0; n < world->irradiance_count && n < DE__GI_VOLUMES; ++n) {
        const de__irradiance_snapshot *v = &world->irradiance[n];
        u32 at = DE__PROBE_NONE;
        for (u32 i = 0; i < DE__GI_VOLUMES; ++i)
            if (s->records[i].snapshot.entity == v->entity) {
                at = i;
                break;
            }
        if (at == DE__PROBE_NONE)
            for (u32 i = 0; i < DE__GI_VOLUMES; ++i) {
                bool remains = false;
                for (u32 k = 0; k < world->irradiance_count; ++k)
                    if (s->records[i].snapshot.entity == world->irradiance[k].entity)
                        remains = true;
                if (!remains) {
                    gi_release(s, &s->records[i]);
                    at = i;
                    break;
                }
            }
        if (at == DE__PROBE_NONE)
            continue;
        gi_record *rec = &s->records[at];
        const f64 position[] = {v->position.x, v->position.y, v->position.z};
        const f64 origin[] = {s->origin.x, s->origin.y, s->origin.z};
        const f32 quaternion[] = {v->rotation.x, v->rotation.y, v->rotation.z, v->rotation.w};
        bool finite = true;
        f64 qnorm = 0;
        for (u32 i = 0; i < 3; ++i)
            finite = finite && isfinite(position[i]) && isfinite(origin[i]) &&
                     isfinite(position[i] - origin[i]) && fabs(position[i] - origin[i]) <= 100000;
        for (u32 i = 0; i < 4; ++i) {
            finite = finite && isfinite(quaternion[i]);
            qnorm += (f64)quaternion[i] * quaternion[i];
        }
        finite = finite && qnorm > 1e-12 && qnorm < 1e12;
        if (!finite) {
            gi_release(s, rec);
            DE_WARN(r->log, "GI volume rejected: invalid transform");
            continue;
        }
        if (rec->hash != de__gi_settings_hash(v) || !rec->layout.probes) {
            if (!s->clear.pipeline.id)
                continue;
            if (gi_reserve(s, rec, v) != DE_OK) {
                DE_WARN(r->log,
                        "GI volume %llu rejected: layout/allocation; preserving prior ready record",
                        (unsigned long long)v->entity);
                if (rec->layout.probes) {
                    rec->live = true;
                    s->order[s->count++] = at;
                }
                continue;
            }
        }
        if (rec->bake.id != v->volume.bake.id) {
            rec->bake = v->volume.bake;
            rec->load_checked = false;
        }
        rec->snapshot = *v;
        rec->live = true;
        s->order[s->count++] = at;
    }
    for (u32 i = 0; i < DE__GI_VOLUMES; ++i)
        if (!s->records[i].live)
            gi_release(s, &s->records[i]);
    if (!s->integrate.pipeline.id)
        return;
    for (u32 n = 0; n < s->count; ++n) {
        gi_record *rec = &s->records[s->order[n]];
        if (!rec->load_checked && rec->bake.id && r->assets) {
            de__slot *slot = de__slot_get(r->assets, rec->bake.id);
            if (slot && (slot->state == DE_LOAD_PENDING || slot->state == DE_LOAD_LOADING))
                continue;
            u64 size = 0;
            const void *payload = NULL, *blob = de_blob_data(r->assets, rec->bake, &size);
            de__gi_layout parsed;
            bool compatible =
                blob && de__gi_blob_read(blob, size, rec->hash, &parsed, &payload) == DE_OK &&
                parsed.payload_bytes == rec->layout.payload_bytes &&
                parsed.range == rec->layout.range && parsed.bounces == rec->layout.bounces &&
                memcmp(parsed.counts, rec->layout.counts, sizeof parsed.counts) == 0;
            if (compatible) {
                /* Resource/submit failure is retryable. Preserve prior ready data and never
                 * start a replacement capture merely because the shipping upload failed. */
                if (gi_transfer(s, rec, (void *)payload, true) != DE_OK) {
                    de_error_clear();
                    continue;
                }
                rec->ready = true;
                rec->baked = true;
                rec->bounce = rec->layout.bounces;
            } else
                de_error_clear();
            rec->load_checked = true;
        }
        if (!rec->ready && s->active == DE__PROBE_NONE)
            s->active = s->order[n];
    }
    if (s->active == DE__PROBE_NONE)
        return;
    if (!s->capture.initialized) {
        if (de__probe_capture_init(&s->capture, r, 32) != DE_OK)
            return;
    }
    if (!s->distances.id) {
        s->distances = de_gpu_texture_create(
            r->dev, &(de_gpu_texture_desc){.width = 32,
                                           .height = 192,
                                           .format = DE_FMT_R32_FLOAT,
                                           .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                           .name = "gi.capture_distance"});
        if (!s->distances.id)
            return;
    }
    gi_record *rec = &s->records[s->active];
    u32 index = rec->probe;
    u32 x = index % rec->layout.counts[0],
        y = index / rec->layout.counts[0] % rec->layout.counts[1],
        z = index / (rec->layout.counts[0] * rec->layout.counts[1]);
    de_vec3 local = {
        -rec->snapshot.volume.extent.x +
            2 * rec->snapshot.volume.extent.x * ((f32)x + .5f) / (f32)rec->layout.counts[0],
        -rec->snapshot.volume.extent.y +
            2 * rec->snapshot.volume.extent.y * ((f32)y + .5f) / (f32)rec->layout.counts[1],
        -rec->snapshot.volume.extent.z +
            2 * rec->snapshot.volume.extent.z * ((f32)z + .5f) / (f32)rec->layout.counts[2]};
    de_vec3 rotated = de_quat_rotate(de_quat_normalize(rec->snapshot.rotation), local);
    de_dvec3 position = {rec->snapshot.position.x + rotated.x, rec->snapshot.position.y + rotated.y,
                         rec->snapshot.position.z + rotated.z};
    u32 mask = 0, left = (u32)de_cvar_int(s->faces);
    if (!left)
        left = 6;
    for (u32 f = 0; f < 6 && left; ++f)
        if (!(rec->faces & (1u << f))) {
            mask |= 1u << f;
            --left;
        }
    de__probe_capture_at_position(&s->capture, position, s->origin, mask);
}
void de__irradiance_after_views(de_renderer *r, u64 frame) {
    (void)frame;
    struct de__irradiance *s = r->irradiance;
    if (!s || s->active >= DE__GI_VOLUMES || !s->distances.id)
        return;
    gi_record *rec = &s->records[s->active];
    if (s->capture.scheduled) {
        if ((s->depth_success & s->capture.scheduled) != s->capture.scheduled)
            return;
        if (de__probe_capture_complete(&s->capture, NULL, (de__probe_handle){DE__PROBE_NONE, 0},
                                       false) != DE_OK)
            return;
        rec->faces |= s->capture.scheduled;
        ++rec->captures;
    }
    if (rec->faces != 63)
        return;
    u32 write = rec->last ^ 1u;
    de_gpu_cmd *cmd = gi_begin(s, "gi.integrate");
    if (!cmd)
        return;
    gi_barrier(cmd, s->capture.raw, s->capture.raw_access, DE__GI_ACCESS);
    gi_barrier(cmd, s->distances, s->distance_access, DE__GI_ACCESS);
    gi_barrier(cmd, rec->irradiance[write], rec->access[write], DE_ACCESS_SHADER_WRITE);
    gi_barrier(cmd, rec->distance[write], rec->access[write], DE_ACCESS_SHADER_WRITE);
    gi_bake_params pc = {.raw = de_gpu_texture_bindless(r->dev, s->capture.raw, 0, true),
                         .distances = de_gpu_texture_bindless(r->dev, s->distances, 0, true),
                         .irradiance =
                             de_gpu_texture_bindless(r->dev, rec->irradiance[write], 0, true),
                         .moments = de_gpu_texture_bindless(r->dev, rec->distance[write], 0, true),
                         .probe = rec->probe,
                         .columns = rec->layout.columns,
                         .range = rec->layout.range};
    de_gpu_cmd_set_pipeline(cmd, s->integrate.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, 3, 3, 1);
    gi_barrier(cmd, rec->irradiance[write], DE_ACCESS_SHADER_WRITE, DE__GI_ACCESS);
    gi_barrier(cmd, rec->distance[write], DE_ACCESS_SHADER_WRITE, DE__GI_ACCESS);
    if (de_gpu_queue_submit(r->dev, DE_QUEUE_GRAPHICS,
                            &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}) != DE_OK)
        return;
    rec->access[write] = DE__GI_ACCESS;
    rec->faces = 0;
    if (++rec->probe == rec->layout.probes) {
        rec->probe = 0;
        rec->last = write;
        ++rec->bounce;
        if (rec->bounce == rec->layout.bounces)
            rec->ready = true;
    }
}
bool de__irradiance_get(const de_renderer *r, u64 entity, de__irradiance_info *out) {
    if (!r || !r->irradiance || !out)
        return false;
    struct de__irradiance *s = r->irradiance;
    for (u32 i = 0; i < DE__GI_VOLUMES; ++i)
        if (s->records[i].snapshot.entity == entity) {
            const gi_record *v = &s->records[i];
            *out = (de__irradiance_info){.layout = v->layout,
                                         .bounce = v->bounce,
                                         .probe = v->probe,
                                         .faces = v->faces,
                                         .captures = v->captures,
                                         .ready = v->ready,
                                         .baked = v->baked,
                                         .irradiance = v->irradiance[v->last],
                                         .distance = v->distance[v->last],
                                         .raw_radiance = s->capture.raw,
                                         .raw_distance = s->distances,
                                         .classification = v->classification};
            out->gpu = gi_gpu(v, s->origin, true);
            out->gpu.textures[0] = de_gpu_texture_bindless(r->dev, out->irradiance, 0, true);
            out->gpu.textures[1] = de_gpu_texture_bindless(r->dev, out->distance, 0, true);
            out->gpu.textures[2] = de_gpu_buffer_bindless(r->dev, out->classification);
            return true;
        }
    return false;
}
de_result de__irradiance_readback(de_renderer *r, u64 entity, void *payload, u64 bytes) {
    if (!r || !r->irradiance || !payload)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GI: readback arguments");
    de__render_world_wait_idle(r);
    for (u32 i = 0; i < DE__GI_VOLUMES; ++i) {
        gi_record *rec = &r->irradiance->records[i];
        if (rec->snapshot.entity == entity && rec->ready && bytes == rec->layout.payload_bytes)
            return gi_transfer(r->irradiance, rec, payload, false);
    }
    return DE_FAIL(DE_ERR_NOT_FOUND, "GI: ready volume not found");
}
static de_result gi_bake_command(int argc, const char *const *argv, void *user) {
    (void)argv;
    if (argc != 1)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "usage: r.gi_bake");
    struct de__irradiance *s = user;
    de_renderer *r = s->renderer;
    de__render_world_wait_idle(r);
    for (u32 n = 0; n < s->count; ++n) {
        gi_record *rec = &s->records[s->order[n]];
        if (!rec->ready)
            return DE_FAIL(DE_ERR_BUSY, "GI: bake incomplete");
        u64 bytes = DE__GI_BLOB_HEADER + rec->layout.payload_bytes;
        u8 *blob = DE_ALLOC(r->alloc, (size_t)bytes, 16, DE_MEM_RENDER);
        if (!blob)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "GI: bake blob");
        de_result result = gi_transfer(s, rec, blob + DE__GI_BLOB_HEADER, false);
        if (result == DE_OK)
            result =
                de__gi_blob_write(blob, bytes, rec->hash, &rec->layout, blob + DE__GI_BLOB_HEADER);
        char path[96];
        (void)snprintf(path, sizeof path, "dev:/gi-%016llx.degi",
                       (unsigned long long)rec->snapshot.entity);
        if (result == DE_OK)
            result = de_file_write_all_atomic(path, blob, bytes);
        DE_FREE(r->alloc, blob, (size_t)bytes);
        if (result != DE_OK)
            return result;
    }
    return DE_OK;
}

void de__irradiance_view_complete(de_renderer *r, de_view view, bool success) {
    struct de__irradiance *s = r->irradiance;
    if (!s)
        return;
    for (u32 face = 0; face < 6; ++face)
        if (s->capture.views[face].id == view.id && success &&
            (de_atomic_load_u32(&s->depth_recorded, DE_ATOMIC_ACQUIRE) & (1u << face))) {
            s->depth_success |= 1u << face;
            s->distance_access = DE__GI_ACCESS;
        }
}
void de__irradiance_debug_skip_depth(de_renderer *r, u32 mask) {
    de__render_world_wait_idle(r);
    if (r->irradiance)
        r->irradiance->skip_depth = mask;
}
