/*
 * loader_test_util.h: helpers of the render loader tests (DENG-105): builders of small cooked
 * blobs (DTEX, DTEX + ENVS, DMSH v2, DMAT) that the format parsers accept, GPU devices of each
 * backend compiled in (or SKIP), and readback through a graphics list that first records the
 * uploader's acquire barriers. Header-only (static functions) for test_upload_ring.c and
 * test_render_loaders.c.
 */
#ifndef DE_TEST_RENDER_LOADER_TEST_UTIL_H
#define DE_TEST_RENDER_LOADER_TEST_UTIL_H

#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "render/formats/de_cooked_env.h"
#include "render/formats/de_cooked_le.h"
#include "render/formats/de_cooked_material.h"
#include "render/formats/de_cooked_mesh.h"
#include "render/formats/de_cooked_texture.h"
#include "render/loaders/render_assets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LT_WAIT_NS ((u64)30 * 1000 * 1000 * 1000)

/* ---- Blobs (heap memory, free with free()) ------------------------------------------------- */

typedef struct lt_blob {
    u8 *data;
    u64 size;
} lt_blob;

static inline u64 lt_align(u64 v, u64 a) {
    return (v + a - 1u) & ~(a - 1u);
}

/* The byte the builders put at index `i` of subresource or section `k` (`seed` varies assets). */
static inline u8 lt_pattern(u64 i, u32 k, u32 seed) {
    return (u8)(i * 13u + 7u + k * 31u + seed * 101u);
}

/** A DTEX blob (dim 2D, 2D_ARRAY or CUBE) with pattern bytes in every row and zero padding. With
 * `env_trailer`, room for an ENVS trailer after it (lt_env_finish writes it). */
static inline lt_blob lt_dtex(u32 dim, u32 format, u32 w, u32 h, u32 layers, u32 mips, u32 usage,
                              u32 seed, bool env_trailer) {
    lt_blob b = {NULL, 0};
    de_cooked_texture_block blk;
    if (!de_cooked_texture_format_block(format, &blk))
        return b;
    const u32 subs = layers * mips;
    u64 at =
        lt_align(DE_COOKED_TEXTURE_HEADER_SIZE + (u64)subs * DE_COOKED_TEXTURE_SUBRESOURCE_SIZE,
                 DE_COOKED_TEXTURE_SUBRESOURCE_ALIGN);
    u64 *off = (u64 *)calloc(subs, sizeof(u64));
    u32 *pitch = (u32 *)calloc(subs, sizeof(u32));
    u32 *rows = (u32 *)calloc(subs, sizeof(u32));
    for (u32 i = 0; i < subs; ++i) {
        const u32 mip = i % mips;
        const u64 row = de_cooked_texture_row_bytes(&blk, de_cooked_texture_mip_extent(w, mip));
        pitch[i] = (u32)lt_align(row, DE_COOKED_TEXTURE_ROW_ALIGN);
        rows[i] = de_cooked_texture_row_count(&blk, de_cooked_texture_mip_extent(h, mip));
        at = lt_align(at, DE_COOKED_TEXTURE_SUBRESOURCE_ALIGN);
        off[i] = at;
        at += (u64)pitch[i] * rows[i];
    }
    const u64 total = at;
    b.size = total + (env_trailer ? DE_COOKED_ENV_TRAILER_SIZE : 0u);
    b.data = (u8 *)calloc(1, (size_t)b.size);
    u8 *p = b.data;
    memcpy(p, DE_COOKED_TEXTURE_MAGIC, 4);
    de_cooked_put_u32(p + 4, DE_COOKED_TEXTURE_VERSION);
    de_cooked_put_u32(p + 8, dim);
    de_cooked_put_u32(p + 12, format);
    de_cooked_put_u32(p + 16, w);
    de_cooked_put_u32(p + 20, h);
    de_cooked_put_u32(p + 24, layers);
    de_cooked_put_u32(p + 28, mips);
    u32 flags = de_cooked_texture_format_is_srgb(format) ? DE_COOKED_TEXTURE_SRGB : 0u;
    if (dim == (u32)DE_TEX_CUBE)
        flags |= DE_COOKED_TEXTURE_CUBE;
    de_cooked_put_u32(p + 32, flags);
    de_cooked_put_u32(p + 36, usage);
    de_cooked_put_u32(p + 40, subs);
    de_cooked_put_u64(p + 48, total);
    for (u32 i = 0; i < subs; ++i) {
        u8 *e = p + DE_COOKED_TEXTURE_HEADER_SIZE + (u64)i * DE_COOKED_TEXTURE_SUBRESOURCE_SIZE;
        de_cooked_put_u64(e, off[i]);
        de_cooked_put_u64(e + 8, (u64)pitch[i] * rows[i]);
        de_cooked_put_u32(e + 16, pitch[i]);
        de_cooked_put_u32(e + 20, rows[i]);
        const u64 row =
            de_cooked_texture_row_bytes(&blk, de_cooked_texture_mip_extent(w, i % mips));
        for (u32 r = 0; r < rows[i]; ++r)
            for (u64 x = 0; x < row; ++x)
                p[off[i] + (u64)r * pitch[i] + x] = lt_pattern((u64)r * row + x, i, seed);
    }
    free(off);
    free(pitch);
    free(rows);
    return b;
}

/** A cooked environment: a BC6H (or RGBA16F) cube of `face` (a power of two, full chain) and its
 * ENVS trailer, with SH coefficient k, channel c = k + c / 10. */
static inline lt_blob lt_env(u32 format, u32 face, u32 seed) {
    const u32 mips = de_cooked_texture_full_mip_count(face, face);
    lt_blob b = lt_dtex(DE_TEX_CUBE, format, face, face, 6, mips,
                        DE_COOKED_TEXTURE_USAGE_ENVIRONMENT, seed, true);
    if (b.data == NULL)
        return b;
    u8 *e = b.data + (b.size - DE_COOKED_ENV_TRAILER_SIZE);
    memcpy(e, DE_COOKED_ENV_MAGIC, 4);
    de_cooked_put_u32(e + DE_COOKED_ENV_OFF_VERSION, DE_COOKED_ENV_VERSION);
    de_cooked_put_u32(e + DE_COOKED_ENV_OFF_SIZE, DE_COOKED_ENV_TRAILER_SIZE);
    de_cooked_put_u32(e + DE_COOKED_ENV_OFF_FACE_SIZE, face);
    de_cooked_put_u32(e + DE_COOKED_ENV_OFF_MIP_COUNT, mips);
    de_cooked_put_u32(e + DE_COOKED_ENV_OFF_ROUGHNESS_MAPPING, DE_COOKED_ENV_ROUGHNESS_LINEAR);
    de_cooked_put_f32(e + DE_COOKED_ENV_OFF_MIP_PER_ROUGHNESS, (f32)(mips - 1u));
    de_cooked_put_u32(e + DE_COOKED_ENV_OFF_SAMPLE_COUNT, 16);
    de_cooked_put_f32(e + DE_COOKED_ENV_OFF_ROTATION_DEG, 30.0f);
    de_cooked_put_f32(e + DE_COOKED_ENV_OFF_MEAN_LUMINANCE, 0.5f);
    de_cooked_put_f32(e + DE_COOKED_ENV_OFF_MAX_LUMINANCE, 8.0f);
    for (u32 k = 0; k < DE_COOKED_ENV_SH_COUNT; ++k)
        for (u32 c = 0; c < 3u; ++c)
            de_cooked_put_f32(e + DE_COOKED_ENV_OFF_SH9 + 12u * k + 4u * c,
                              (f32)k + (f32)c / 10.0f);
    return b;
}

/** Subresource `i` of a DTEX blob (the DTEX part of an environment too). */
static inline de_cooked_texture_subresource lt_dtex_sub(const lt_blob *b, u32 i) {
    de_cooked_texture_view v;
    de_cooked_texture_subresource s;
    memset(&s, 0, sizeof s);
    const u64 dtex = de_cooked_get_u64(b->data + 48);
    if (de_cooked_texture_parse(b->data, dtex, &v, NULL))
        (void)de_cooked_texture_subresource_at(&v, i, &s);
    return s;
}

/** A DMSH v2 mesh of `v` vertices (>= 3) in a triangle strip-like list (triangle i = i, i+1,
 * i+2), with POSITION, NORMAL and UV0 streams of pattern bytes, one submesh with material
 * `material`, one LOD and meshlets of 64 vertices / 62 triangles. */
static inline lt_blob lt_dmsh(u32 v, u64 material, u32 seed) {
    lt_blob b = {NULL, 0};
    const u32 tris = v - 2u, idx = 3u * tris;
    const u32 ml = (tris + 61u) / 62u; /* meshlet m: vertices [62m, 62m + 64), its 62 triangles */
    u32 mlv = 0, mlt = 0;
    for (u32 m = 0; m < ml; ++m) {
        const u32 vc = v - 62u * m < 64u ? v - 62u * m : 64u;
        const u32 tc = tris - 62u * m < 62u ? tris - 62u * m : 62u;
        mlv += vc;
        mlt += (u32)lt_align(3u * tc, 4u);
    }
    const u32 nstreams = 3;
    const u32 sem[3] = {DE_COOKED_STREAM_POSITION, DE_COOKED_STREAM_NORMAL, DE_COOKED_STREAM_UV0};
    const u32 fmt[3] = {DE_COOKED_VF_UNORM16X4, DE_COOKED_VF_OCT16, DE_COOKED_VF_UNORM16X2};
    u64 at = DE_COOKED_MESH_HEADER_SIZE;
    const u64 o_streams = at;
    at = lt_align(at + nstreams * DE_COOKED_MESH_STREAM_SIZE, DE_COOKED_MESH_ALIGN);
    const u64 o_sub = at;
    at = lt_align(at + DE_COOKED_MESH_SUBMESH_SIZE, DE_COOKED_MESH_ALIGN);
    const u64 o_lod = at;
    at = lt_align(at + DE_COOKED_MESH_LOD_SIZE, DE_COOKED_MESH_ALIGN);
    const u64 o_lodsub = at;
    at = lt_align(at + DE_COOKED_MESH_LOD_SUBMESH_SIZE, DE_COOKED_MESH_ALIGN);
    const u64 o_ml = at;
    at = lt_align(at + (u64)ml * DE_COOKED_MESHLET_SIZE, DE_COOKED_MESH_ALIGN);
    u64 o_stream[3];
    for (u32 s = 0; s < nstreams; ++s) {
        o_stream[s] = at;
        at = lt_align(at + (u64)v * de_cooked_vertex_format_size(fmt[s]), DE_COOKED_MESH_ALIGN);
    }
    const u64 o_mlv = at;
    at = lt_align(at + (u64)mlv * 4u, DE_COOKED_MESH_ALIGN);
    const u64 o_mlt = at;
    at = lt_align(at + mlt, DE_COOKED_MESH_ALIGN);
    const u64 o_idx = at;
    at += (u64)idx * 4u;
    b.size = at;
    b.data = (u8 *)calloc(1, (size_t)b.size);
    u8 *p = b.data;
    memcpy(p, DE_COOKED_MESH_MAGIC, 4);
    de_cooked_put_u32(p + 4, DE_COOKED_MESH_VERSION);
    de_cooked_put_u32(p + 8, DE_COOKED_MESH_FLAG_MESHLETS);
    de_cooked_put_u32(p + 12, v);
    de_cooked_put_u32(p + 16, 1);
    de_cooked_put_u32(p + 20, 1);
    de_cooked_put_u32(p + 24, nstreams);
    de_cooked_put_u32(p + 28, DE_COOKED_INDEX_U32);
    de_cooked_put_u32(p + 32, idx);
    de_cooked_put_u32(p + 36, ml);
    de_cooked_put_u32(p + 40, mlv);
    de_cooked_put_u32(p + 44, mlt);
    de_cooked_put_u32(p + 48, (u32)b.size);
    de_cooked_put_u32(p + 56, (u32)o_streams);
    de_cooked_put_u32(p + 60, (u32)o_sub);
    de_cooked_put_u32(p + 64, (u32)o_lod);
    de_cooked_put_u32(p + 68, (u32)o_lodsub);
    de_cooked_put_u32(p + 72, (u32)o_ml);
    de_cooked_put_u32(p + 76, (u32)o_mlv);
    de_cooked_put_u32(p + 80, (u32)o_mlt);
    de_cooked_put_u32(p + 84, (u32)o_idx);
    for (u32 k = 0; k < 3; ++k) {
        de_cooked_put_f32(p + 88 + 4 * k, -1.0f);
        de_cooked_put_f32(p + 100 + 4 * k, 1.0f + (f32)k);
        de_cooked_put_f32(p + 128 + 4 * k, -1.0f);
        de_cooked_put_f32(p + 140 + 4 * k, 2.0f / 65535.0f);
    }
    de_cooked_put_f32(p + 124, 2.0f); /* sphere radius */
    de_cooked_put_f32(p + 184, 0.01f);
    de_cooked_put_f32(p + 188, 0.0001f);
    for (u32 s = 0; s < nstreams; ++s) {
        u8 *r = p + o_streams + s * DE_COOKED_MESH_STREAM_SIZE;
        const u32 len = v * de_cooked_vertex_format_size(fmt[s]);
        de_cooked_put_u32(r, sem[s]);
        de_cooked_put_u32(r + 4, fmt[s]);
        de_cooked_put_u32(r + 8, (u32)o_stream[s]);
        de_cooked_put_u32(r + 12, len);
        for (u32 i = 0; i < len; ++i)
            p[o_stream[s] + i] = lt_pattern(i, s, seed);
    }
    de_cooked_put_u64(p + o_sub, material);
    for (u32 k = 0; k < 3; ++k) {
        de_cooked_put_f32(p + o_sub + 8 + 4 * k, -1.0f);
        de_cooked_put_f32(p + o_sub + 20 + 4 * k, 1.0f);
    }
    u8 *l = p + o_lod;
    de_cooked_put_u32(l, 0);
    de_cooked_put_u32(l + 4, idx);
    de_cooked_put_u32(l + 8, 0);
    de_cooked_put_u32(l + 12, ml);
    de_cooked_put_u32(l + 16, tris);
    de_cooked_put_u32(l + 20, v);
    u8 *ls = p + o_lodsub;
    de_cooked_put_u32(ls + 4, idx);
    de_cooked_put_u32(ls + 12, ml);
    for (u32 t = 0; t < tris; ++t)
        for (u32 c = 0; c < 3; ++c)
            de_cooked_put_u32(p + o_idx + 4u * (3u * t + c), t + c);
    u32 vat = 0, tat = 0;
    for (u32 m = 0; m < ml; ++m) {
        const u32 vc = v - 62u * m < 64u ? v - 62u * m : 64u;
        const u32 tc = tris - 62u * m < 62u ? tris - 62u * m : 62u;
        u8 *r = p + o_ml + (u64)m * DE_COOKED_MESHLET_SIZE;
        de_cooked_put_u32(r, vat);
        de_cooked_put_u32(r + 4, tat);
        de_cooked_put_u32(r + 8, vc | (tc << 8));
        de_cooked_put_f32(r + 28, 1.0f); /* radius */
        de_cooked_put_f32(r + 44, 1.0f); /* cone cutoff: never culled */
        for (u32 j = 0; j < vc; ++j)
            de_cooked_put_u32(p + o_mlv + 4u * (vat + j), 62u * m + j);
        for (u32 t = 0; t < tc; ++t)
            for (u32 c = 0; c < 3; ++c)
                p[o_mlt + tat + 3u * t + c] = (u8)(t + c);
        vat += vc;
        tat += (u32)lt_align(3u * tc, 4u);
    }
    return b;
}

/** A DMAT material with `count` textures (slot, ID) and base colour `r`. */
static inline lt_blob lt_dmat(const u32 *slots, const u64 *ids, u32 count, f32 r) {
    lt_blob b;
    b.size = DE_COOKED_MATERIAL_HEADER_SIZE + DE_COOKED_MATERIAL_PARAMS_SIZE +
             (u64)count * DE_COOKED_MATERIAL_TEX_SIZE;
    b.data = (u8 *)calloc(1, (size_t)b.size);
    u8 *p = b.data;
    memcpy(p, DE_COOKED_MATERIAL_MAGIC, 4);
    de_cooked_put_u32(p + 4, DE_COOKED_MATERIAL_VERSION);
    de_cooked_put_u32(p + 8, DE_COOKED_MAT_DOUBLE_SIDED);
    de_cooked_put_u32(p + 12, DE_COOKED_ALPHA_MASK);
    de_cooked_put_u32(p + 16, DE_COOKED_MATERIAL_PARAMS_SIZE);
    de_cooked_put_u32(p + 20, count);
    de_cooked_put_u32(p + 24, (u32)b.size);
    de_cooked_material_params mp = de_cooked_material_params_default();
    mp.base_color[0] = r;
    mp.roughness = 0.25f;
    const f32 *f = (const f32 *)&mp;
    for (u32 i = 0; i < DE_COOKED_MATERIAL_PARAM_FLOATS; ++i)
        de_cooked_put_f32(p + DE_COOKED_MATERIAL_HEADER_SIZE + 4u * i, f[i]);
    u8 *t = p + DE_COOKED_MATERIAL_HEADER_SIZE + DE_COOKED_MATERIAL_PARAMS_SIZE;
    for (u32 i = 0; i < count; ++i, t += DE_COOKED_MATERIAL_TEX_SIZE) {
        de_cooked_put_u32(t, slots[i]);
        de_cooked_put_u32(t + 4, 0);
        de_cooked_put_u64(t + 8, ids[i]);
    }
    return b;
}

static inline void lt_free(lt_blob *b) {
    free(b->data);
    b->data = NULL;
    b->size = 0;
}

/* ---- GPU devices --------------------------------------------------------------------------- */

static inline const char *lt_errchain(char *buf, size_t cap) {
    size_t used = 0;
    buf[0] = '\0';
    for (const de_error *e = de_error_last(); e && used + 1 < cap; e = e->cause) {
        int w = snprintf(buf + used, cap - used, "%s%s", used ? " <- " : "", e->message);
        if (w < 0)
            break;
        used += (size_t)w < cap - used ? (size_t)w : cap - used - 1;
    }
    return buf;
}

/** True when `b` is compiled into this build. */
static inline bool lt_backend_built(de_gpu_backend b) {
    switch (b) {
    case DE_GPU_NULL:
        return true;
#if DE_HAS_GPU_METAL
    case DE_GPU_METAL:
        return true;
#endif
#if DE_HAS_GPU_VULKAN
    case DE_GPU_VULKAN:
        return true;
#endif
#if DE_HAS_GPU_D3D12
    case DE_GPU_D3D12:
        return true;
#endif
    default:
        return false;
    }
}

/** A device of backend `b` with its debug layer, or NULL with the reason in `why` (Vulkan tries
 * adapter 1 when no device is Base-tier capable: copies need no bindless tables). */
static inline de_gpu_device *lt_device(de_gpu_backend b, const de_allocator *a, char *why,
                                       size_t cap) {
    if (!lt_backend_built(b)) {
        snprintf(why, cap, "the %s backend is not in this build", de_gpu_backend_name(b));
        return NULL;
    }
#if !defined(_WIN32)
    if (b == DE_GPU_METAL)
        setenv("MTL_DEBUG_LAYER", "1", 0);
#endif
    de_gpu_device_desc desc = {.backend = b,
                               .debug_layer = b != DE_GPU_NULL,
                               .gpu_validation = b == DE_GPU_VULKAN,
                               .allocator = a,
                               .pipeline_cache_dir = ""};
    de_gpu_device *d = NULL;
    de_result r = de_gpu_device_create(&desc, &d);
    if (r == DE_ERR_UNSUPPORTED && b == DE_GPU_VULKAN) {
        desc.adapter = 1;
        r = de_gpu_device_create(&desc, &d);
    }
    if (r != DE_OK) {
        char err[900];
        snprintf(why, cap, "no usable %s device: %s", de_gpu_backend_name(b),
                 lt_errchain(err, sizeof err));
        return NULL;
    }
    printf("device: \"%s\" (%s)\n", de_gpu_get_caps(d)->device_name, de_gpu_backend_name(b));
    return d;
}

/* ---- Readback ------------------------------------------------------------------------------ */

/** One subresource to read back. */
typedef struct lt_tex_read {
    u32 mip, layer;
    u32 row_pitch; /**< a multiple of 256 */
    u32 rows;
    u64 dst_off; /**< filled by lt_readback */
} lt_tex_read;

/**
 * Records, on a graphics list: the uploader's acquire barriers, then SHADER_READ -> COPY_SRC on
 * `tex` (if valid) and each buffer of `bufs`, the copies of `reads` (texture) and of the whole
 * buffers into one READBACK buffer, submitted with the acquire's wait; waits for it and copies
 * the readback bytes into `*out` (malloc). Buffer i lands at `buf_off[i]`.
 */
static inline bool lt_readback(de_gpu_device *d, de_render_uploader *u, de_gpu_texture tex,
                               lt_tex_read *reads, u32 nreads, const de_gpu_buffer *bufs,
                               const u64 *buf_size, u64 *buf_off, u32 nbufs, u8 **out,
                               u64 *out_size) {
    u64 at = 0;
    for (u32 i = 0; i < nreads; ++i) {
        at = lt_align(at, 512);
        reads[i].dst_off = at;
        at += (u64)reads[i].row_pitch * reads[i].rows;
    }
    for (u32 i = 0; i < nbufs; ++i) {
        at = lt_align(at, 256);
        buf_off[i] = at;
        at += buf_size[i];
    }
    de_gpu_buffer rb = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = at,
                                                                     .usage = DE_USAGE_COPY_DST,
                                                                     .memory = DE_MEM_READBACK,
                                                                     .name = "loader readback"});
    de_gpu_fence fence = de_gpu_fence_create(d, 0);
    de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "loader readback");
    if (!DE_HANDLE_VALID(rb) || !DE_HANDLE_VALID(fence) || c == NULL)
        return false;
    de_gpu_wait wait;
    (void)de_render_uploader_acquire(u, c, &wait);
    de_gpu_barrier bar[8];
    u32 nb = 0;
    memset(bar, 0, sizeof bar);
    if (DE_HANDLE_VALID(tex)) {
        bar[nb].texture = tex;
        bar[nb].before = DE_ACCESS_SHADER_READ;
        bar[nb++].after = DE_ACCESS_COPY_SRC;
    }
    for (u32 i = 0; i < nbufs && nb < 8; ++i) {
        bar[nb].buffer = bufs[i];
        bar[nb].before = DE_ACCESS_SHADER_READ;
        bar[nb++].after = DE_ACCESS_COPY_SRC;
    }
    de_gpu_cmd_barriers(c, bar, nb);
    for (u32 i = 0; i < nreads; ++i)
        de_gpu_cmd_copy_texture_to_buffer(c, rb, reads[i].dst_off, reads[i].row_pitch, tex,
                                          reads[i].mip, reads[i].layer);
    for (u32 i = 0; i < nbufs; ++i)
        de_gpu_cmd_copy_buffer(c, rb, buf_off[i], bufs[i], 0, buf_size[i]);
    de_gpu_submit s = {.cmds = &c,
                       .cmd_count = 1,
                       .waits = DE_HANDLE_VALID(wait.fence) ? &wait : NULL,
                       .wait_count = DE_HANDLE_VALID(wait.fence) ? 1u : 0u,
                       .signal = fence,
                       .signal_value = 1};
    bool ok = de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &s) == DE_OK &&
              de_gpu_fence_wait(d, fence, 1, LT_WAIT_NS) == DE_OK;
    if (ok) {
        *out = (u8 *)malloc((size_t)at);
        memcpy(*out, de_gpu_buffer_map(d, rb), (size_t)at);
        *out_size = at;
    }
    de_gpu_fence_destroy(d, fence);
    de_gpu_buffer_destroy(d, rb);
    return ok;
}

#endif /* DE_TEST_RENDER_LOADER_TEST_UTIL_H */
