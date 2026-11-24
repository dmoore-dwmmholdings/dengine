/* DENG-73: CPU coefficient/phase mirrors and retained fog parameter ownership. */
#include "render/pathtracer/pt_internal.h"
#include "render/passes/sss/sss.h"
#include "render/renderer_internal.h"
#include "render/world/render_world.h"
#include "render/scene/gpu_scene.h"
#include "render/materials/material_models.h"
#include <string.h>
#include <math.h>
void de__pt_sss_coefficients(de_vec3 radius, de_vec3 albedo, de_vec3 *sigma_s, de_vec3 *sigma_a) {
    const f32 r[3] = {radius.x, radius.y, radius.z}, a[3] = {albedo.x, albedo.y, albedo.z};
    f32 scatter[3], absorb[3];
    for (u32 i = 0; i < 3; ++i) {
        f32 reflectance = fmaxf(0, fminf(a[i], .999999f));
        f32 d = (f32)de__burley_d(r[i], reflectance);
        d = fmaxf(d, 1e-8f);
        f32 root = (1 - reflectance) / (1 + reflectance), alpha = 1 - root * root;
        f32 extinction = 1 / (d * sqrtf(fmaxf(3 * (1 - alpha), 1e-12f)));
        scatter[i] = extinction * alpha;
        absorb[i] = extinction * (1 - alpha);
    }
    *sigma_s = (de_vec3){scatter[0], scatter[1], scatter[2]};
    *sigma_a = (de_vec3){absorb[0], absorb[1], absorb[2]};
}
f32 de__pt_hg(f32 cosine, f32 g) {
    g = fmaxf(-.99f, fminf(.99f, g));
    f32 d = fmaxf(1 + g * g - 2 * g * cosine, 1e-12f);
    return (1 - g * g) / (12.566370614359172f * d * sqrtf(d));
}

typedef de__pt_fog pt_fog_gpu;
DE_STATIC_ASSERT(sizeof(pt_fog_gpu) == 96, "PtFog shader layout");
static f32 fog_hash(f32 x, f32 y, f32 z) {
    x = x * 0.3183099f + 0.1f;
    y = y * 0.3183099f + 0.1f;
    z = z * 0.3183099f + 0.1f;
    x -= floorf(x);
    y -= floorf(y);
    z -= floorf(z);
    x *= 17.0f;
    y *= 17.0f;
    z *= 17.0f;
    f32 v = x * y * z * (x + y + z);
    return v - floorf(v);
}
static f32 fog_lerp(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}
/* Value noise in [0, 1]; the same as fog_noise of froxel_inject_media.slang. */
static f32 fog_noise(f32 px, f32 py, f32 pz) {
    f32 ix = floorf(px), iy = floorf(py), iz = floorf(pz);
    f32 fx = px - ix, fy = py - iy, fz = pz - iz;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    fz = fz * fz * (3.0f - 2.0f * fz);
    f32 a = fog_lerp(fog_lerp(fog_hash(ix, iy, iz), fog_hash(ix + 1, iy, iz), fx),
                     fog_lerp(fog_hash(ix, iy + 1, iz), fog_hash(ix + 1, iy + 1, iz), fx), fy);
    f32 b = fog_lerp(fog_lerp(fog_hash(ix, iy, iz + 1), fog_hash(ix + 1, iy, iz + 1), fx),
                     fog_lerp(fog_hash(ix, iy + 1, iz + 1), fog_hash(ix + 1, iy + 1, iz + 1), fx),
                     fy);
    return fog_lerp(a, b, fz);
}
static f32 fog_exponent(f32 v) {
    return expf(-fminf(fmaxf(v, -8.0f), 80.0f));
}
bool de__pt_fog_heterogeneous(const de__pt_fog *fog) {
    return fog->params[1] > 0 ||
           (fog->extent_density[0] < 0 ? fog->params[0] != 0 : fog->params[2] > 0);
}
f32 de__pt_fog_density(const de__pt_fog *fog, de_vec3 p) {
    f32 q[3];
    for (u32 r = 0; r < 3; ++r)
        q[r] = fog->inverse[r][0] * p.x + fog->inverse[r][1] * p.y + fog->inverse[r][2] * p.z +
               fog->inverse[r][3];
    f32 d = fmaxf(fog->extent_density[3], 0.0f);
    if (fog->extent_density[0] < 0) {
        d *= fog_exponent(fog->params[0] * (p.z - fog->params[3]));
    } else {
        f32 inside = fminf(fog->extent_density[0] - fabsf(q[0]),
                           fminf(fog->extent_density[1] - fabsf(q[1]),
                                 fog->extent_density[2] - fabsf(q[2])));
        if (inside < 0)
            return 0;
        if (fog->params[2] > 0) {
            f32 t = fminf(fmaxf(inside / fog->params[2], 0.0f), 1.0f);
            d *= t * t * (3.0f - 2.0f * t);
        }
    }
    if (fog->params[1] > 0)
        d *= fog_lerp(1.0f, 2.0f * fog_noise(q[0] * 0.5f, q[1] * 0.5f, q[2] * 0.5f), fog->params[1]);
    return d;
}
f32 de__pt_fog_majorant(const de__pt_fog *fog, de_vec3 o, de_vec3 dir, f32 t0, f32 t1) {
    f32 m = fmaxf(fog->extent_density[3], 0.0f);
    if (fog->extent_density[0] < 0 && fog->params[0] != 0) {
        f32 z = fminf(o.z + dir.z * t0, o.z + dir.z * t1);
        m *= fog_exponent(fog->params[0] * (z - fog->params[3]));
    }
    return m * (1.0f + fmaxf(fog->params[1], 0.0f));
}
void de__pt_fog_pack(const de__fog_snapshot *s, de__pt_fog *v) {
    memset(v, 0, sizeof *v);
    de_mat4 matrix = de_affine_to_m4(&s->transform), inverse = de_m4_inverse_affine(&matrix);
    for (u32 r = 0; r < 3; ++r)
        for (u32 c = 0; c < 4; ++c)
            v->inverse[r][c] = inverse.m[c * 4 + r];
    v->extent_density[0] = fabsf(s->volume.extent.x) * .5f;
    v->extent_density[1] = fabsf(s->volume.extent.y) * .5f;
    v->extent_density[2] = fabsf(s->volume.extent.z) * .5f;
    /* DENG-207: a zero extent is global fog, as in the froxel volumetrics. DENG-848: global fog
     * keeps its height falloff; noise and edge fade follow the raster clamps. */
    bool global = v->extent_density[0] == 0 && v->extent_density[1] == 0 &&
                  v->extent_density[2] == 0;
    if (global)
        v->extent_density[0] = -1;
    v->extent_density[3] = s->volume.density;
    v->albedo_g[0] = s->volume.albedo.x;
    v->albedo_g[1] = s->volume.albedo.y;
    v->albedo_g[2] = s->volume.albedo.z;
    v->albedo_g[3] = s->volume.anisotropy;
    v->params[0] = global ? fmaxf(s->volume.height_falloff, 0.0f) : 0.0f;
    v->params[1] = fminf(fmaxf(s->volume.noise, 0.0f), 1.0f);
    v->params[2] = fmaxf(s->volume.edge_fade, 0.0f);
    v->params[3] = matrix.m[14]; /* the entity's world height */
}
struct de__pt_media {
    de_renderer *r;
    de_cvar density, albedo, g, steps;
    de_gpu_buffer fog[8], counter, readback, zero;
    bool logged_overflow;
    u64 submitted[32 * 8];
};
de_result de__pt_media_create(de_renderer *r, de__pt_media **out) {
    de__pt_media *m = DE_ALLOC(r->alloc, sizeof *m, 16, DE_MEM_RENDER);
    if (!m)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "PT media owner");
    memset(m, 0, sizeof *m);
    m->r = r;
    *out = m;
    m->density = de_cvar_register(
        &(de_cvar_desc){.name = "r.pt.fog.density", .type = DE_CVAR_FLOAT, .min = 0, .max = 1000});
    m->albedo = de_cvar_register(&(de_cvar_desc){
        .name = "r.pt.fog.albedo", .type = DE_CVAR_FLOAT, .default_number = 1, .min = 0, .max = 1});
    m->g = de_cvar_register(&(de_cvar_desc){
        .name = "r.pt.fog.anisotropy", .type = DE_CVAR_FLOAT, .min = -.99, .max = .99});
    m->steps = de_cvar_register(&(de_cvar_desc){.name = "r.pt.sss.max_steps",
                                                .type = DE_CVAR_INT,
                                                .default_number = 64,
                                                .min = 1,
                                                .max = 1024});
    return DE_OK;
}
void de__pt_media_destroy(de__pt_media *m) {
    if (!m)
        return;
    for (u32 i = 0; i < 8; ++i)
        de_gpu_buffer_destroy(m->r->dev, m->fog[i]);
    de_gpu_buffer_destroy(m->r->dev, m->counter);
    de_gpu_buffer_destroy(m->r->dev, m->readback);
    de_gpu_buffer_destroy(m->r->dev, m->zero);
    DE_FREE(m->r->alloc, m, sizeof *m);
}
static de_result media_buffers(de__pt_media *m) {
    if (m->counter.id)
        return DE_OK;
    de_gpu_device *d = m->r->dev;
    m->counter = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = 32 * 8 * 4,
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC,
                                 .name = "PT media safety counters"});
    m->readback =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = 32 * 8 * 4,
                                                      .usage = DE_USAGE_COPY_DST,
                                                      .memory = DE_MEM_READBACK,
                                                      .name = "PT media asynchronous diagnostics"});
    m->zero = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = 4, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD});
    if (!m->counter.id || !m->readback.id || !m->zero.id)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "PT media diagnostics");
    *(u32 *)de_gpu_buffer_map(d, m->zero) = 0;
    return DE_OK;
}
static u64 media_hash(u64 h, const void *data, size_t bytes) {
    const u8 *p = data;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}
de_result de__pt_media_frame(de__pt_media *m, de__view_frame *f, de__pt_media_params *out,
                             u64 *signature) {
    memset(out, 0, sizeof *out);
    out->max_steps = (u32)de_cvar_int(m->steps);
    pt_fog_gpu fog[33];
    memset(fog, 0, sizeof fog);
    u32 count = 0;
    f32 density = (f32)de_cvar_float(m->density), albedo = (f32)de_cvar_float(m->albedo),
        g = (f32)de_cvar_float(m->g);
    if (density > 0) {
        fog[count].extent_density[0] = -1;
        fog[count].extent_density[3] = density;
        fog[count].albedo_g[0] = albedo;
        fog[count].albedo_g[1] = albedo;
        fog[count].albedo_g[2] = albedo;
        fog[count].albedo_g[3] = g;
        ++count;
    }
    for (u32 i = 0; f->world && i < f->world->fog_count && count < 33; ++i) {
        const de__fog_snapshot *s = &f->world->fog[i];
        if (!(s->volume.density > 0))
            continue;
        de__pt_fog_pack(s, &fog[count++]);
    }
    out->count = count;
    out->features = count ? 1u : 0u;
    de__gpu_scene_table_info materials = de__gpu_scene_table(m->r->gpu_scene, DE__GS_MATERIALS);
    for (u32 i = 0; i < materials.count; ++i) {
        const de__gs_material *v = de__gpu_scene_entry(m->r->gpu_scene, DE__GS_MATERIALS, i);
        if (!v)
            continue;
        if (v->features & DE__MATERIAL_FEATURE_VOLUME)
            out->features |= 2;
        if (v->features & DE__MATERIAL_FEATURE_SUBSURFACE)
            out->features |= 4;
    }
    *signature = media_hash(1469598103934665603ull, fog, count * sizeof *fog);
    *signature = media_hash(*signature, &out->max_steps, sizeof out->max_steps);
    if (!out->features)
        return DE_OK;
    DE_TRY(media_buffers(m));
    u32 ring = (u32)(f->frame_index % 8), slot = ((f->view.id & 255u) - 1u) * 8 + ring;
#if defined(DE_DEV) || defined(DE_DEBUG)
    if (m->submitted[slot] && f->frame_index >= 8 && !m->logged_overflow &&
        ((u32 *)de_gpu_buffer_map(m->r->dev, m->readback))[slot]) {
        DE_WARN(m->r->log,
                "path tracer: medium stack capacity 4 exceeded; innermost entries dropped");
        m->logged_overflow = true;
    }
#endif
    if (count && !m->fog[ring].id)
        m->fog[ring] = de_gpu_buffer_create(
            m->r->dev, &(de_gpu_buffer_desc){.size = 33 * sizeof(pt_fog_gpu),
                                             .stride = sizeof(pt_fog_gpu),
                                             .usage = DE_USAGE_STORAGE,
                                             .memory = DE_MEM_UPLOAD,
                                             .name = "PT immutable fog regions"});
    if (count && !m->fog[ring].id)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "PT fog regions");
    if (count) {
        memcpy(de_gpu_buffer_map(m->r->dev, m->fog[ring]), fog, count * sizeof *fog);
        out->fog = de_gpu_buffer_bindless(m->r->dev, m->fog[ring]);
    }
    out->stats = de_gpu_buffer_bindless(m->r->dev, m->counter);
    out->stats_offset = slot * 4;
    m->submitted[slot] = f->frame_index + 1;
    return DE_OK;
}
void de__pt_media_begin(de__pt_media *m, de_gpu_cmd *cmd, const de__pt_media_params *p) {
    if (!p->features)
        return;
    de_gpu_barrier b = {
        .buffer = m->counter, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_buffer(cmd, m->counter, p->stats_offset, m->zero, 0, 4);
    b.before = DE_ACCESS_COPY_DST;
    b.after = DE_ACCESS_SHADER_WRITE;
    de_gpu_cmd_barriers(cmd, &b, 1);
}
void de__pt_media_end(de__pt_media *m, de_gpu_cmd *cmd, const de__pt_media_params *p) {
    if (!p->features)
        return;
    de_gpu_barrier b = {
        .buffer = m->counter, .before = DE_ACCESS_SHADER_WRITE, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_copy_buffer(cmd, m->readback, p->stats_offset, m->counter, p->stats_offset, 4);
    b.before = DE_ACCESS_COPY_SRC;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &b, 1);
}
void de__pt_media_graph(de__pt_media *m, de_rg *g, de_rg_pass *pass) {
    if (!m->counter.id)
        return;
    for (u32 i = 0; i < 8; ++i)
        if (m->fog[i].id)
            de_rg_read(pass, de_rg_import_buffer(g, m->fog[i], DE_ACCESS_SHADER_READ),
                       DE_ACCESS_SHADER_READ);
    de_rg_read(pass, de_rg_import_buffer(g, m->zero, DE_ACCESS_COPY_SRC), DE_ACCESS_COPY_SRC);
    de_rg_write(pass, de_rg_import_buffer(g, m->counter, DE_ACCESS_SHADER_READ),
                DE_ACCESS_SHADER_WRITE);
    de_rg_write(pass, de_rg_import_buffer(g, m->readback, DE_ACCESS_COPY_DST), DE_ACCESS_COPY_DST);
}
