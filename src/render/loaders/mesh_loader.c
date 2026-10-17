/*
 * mesh_loader.c: the runtime `mesh` type (SPEC §11.4, §11.6, §12.5; DENG-105). Parses DMSH v2
 * (render/formats/de_cooked_mesh.h, DENG-103) and uploads its GPU sections into five GPU_ONLY
 * buffers, each with its own bindless index (render_assets.h, de_render_mesh_buffer): the vertex
 * streams (each at a 256-byte offset of one buffer), the meshlets, the meshlet vertices, the
 * meshlet triangles and the indices. Shaders read them, so they are GPU memory filled by copies,
 * never UPLOAD memory (DENG-327). The LOD table, the submeshes, the bounds and the quantization
 * parameters stay on the CPU in the record, for the GPU scene (S12) and culling (S13).
 */

#include "render/loaders/loaders_internal.h"

#include "render/formats/de_cooked_mesh.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_gpu.h>

#include <stdint.h>
#include <string.h>

#define STREAM_ALIGN 256u

static u64 align_up(u64 v, u64 a) {
    return (v + a - 1u) & ~(a - 1u);
}

/* One GPU section: where it is in the blob and where it goes. */
typedef struct section {
    u64 blob_off, size;
    u32 buffer; /* de_render_mesh_buffer */
    u64 dst_off;
} section;

static void mesh_free(de_render_uploader *u, de_render_mesh *m) {
    if (m == NULL)
        return;
    de__upload_release(u, m->upload_);
    m->upload_ = NULL;
    for (u32 i = 0; i < DE_RENDER_MESH_BUFFER_COUNT; ++i)
        if (DE_HANDLE_VALID(m->buffers[i]))
            de_gpu_buffer_destroy(u->dev, m->buffers[i]);
    DE_FREE(u->alloc, m, m->block_size_);
}

static u32 buffer_usage(u32 which) {
    u32 usage = DE_USAGE_STORAGE | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC;
    if (which == DE_RENDER_MESH_VERTICES)
        usage |= DE_USAGE_ACCEL_INPUT; /* BLAS builds read positions (S16) */
    if (which == DE_RENDER_MESH_INDICES)
        usage |= DE_USAGE_INDEX | DE_USAGE_ACCEL_INPUT;
    return usage;
}

de_result de__mesh_load(de_asset_load_ctx *ctx) {
    de_render_uploader *u = de__loader_uploader(ctx);
    if (u == NULL)
        return DE_ERR_INTERNAL;
    u64 size = 0;
    const u8 *p = (const u8 *)de_asset_load_ctx_data(ctx, &size);
    de_cooked_mesh_view v;
    const char *why = NULL;
    if (!de_cooked_mesh_parse(p, size, &v, &why))
        return de_asset_load_ctx_fail(ctx, DE_ERR_PARSE, "not a cooked mesh: %s", why);

    /* The record, then its submesh and LOD-submesh tables, in one block. */
    const size_t head = (size_t)align_up(sizeof(de_render_mesh), 16u);
    const size_t subs = (size_t)align_up(sizeof(de_render_submesh) * v.submesh_count, 16u);
    const size_t ranges = sizeof(de_cooked_mesh_range) * (size_t)v.lod_count * v.submesh_count;
    const size_t block = head + subs + ranges;
    de_render_mesh *m =
        (de_render_mesh *)DE_ALLOC(u->alloc, block, DE_ALIGNOF(de_render_mesh), DE_MEM_RENDER);
    if (m == NULL)
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "no memory for the mesh");
    memset(m, 0, block);
    m->block_size_ = block;
    m->asset_id_ = de_asset_load_ctx_id(ctx);
    de_render_submesh *sm = (de_render_submesh *)((u8 *)m + head);
    de_cooked_mesh_range *lr = (de_cooked_mesh_range *)((u8 *)m + head + subs);
    m->submeshes = sm;
    m->lod_submeshes = lr;
    m->vertex_count = v.vertex_count;
    m->index_count = v.index_count;
    m->meshlet_count = v.meshlet_count;
    m->meshlet_vertex_count = v.meshlet_vertex_count;
    m->meshlet_triangle_bytes = v.meshlet_triangle_bytes;
    m->submesh_count = v.submesh_count;
    m->lod_count = v.lod_count;
    memcpy(m->bounds_min, v.bounds_min, sizeof m->bounds_min);
    memcpy(m->bounds_max, v.bounds_max, sizeof m->bounds_max);
    memcpy(m->sphere, v.sphere, sizeof m->sphere);
    memcpy(m->position_offset, v.position_offset, sizeof m->position_offset);
    memcpy(m->position_scale, v.position_scale, sizeof m->position_scale);
    memcpy(m->uv_offset, v.uv_offset, sizeof m->uv_offset);
    memcpy(m->uv_scale, v.uv_scale, sizeof m->uv_scale);
    m->lod_error = v.lod_error;
    m->max_position_error = v.max_position_error;
    for (u32 l = 0; l < v.lod_count; ++l) {
        m->lods[l] = de_cooked_mesh_get_lod(&v, l);
        for (u32 s = 0; s < v.submesh_count; ++s)
            lr[(size_t)l * v.submesh_count + s] = de_cooked_mesh_lod_submesh(&v, l, s);
    }
    for (u32 s = 0; s < v.submesh_count; ++s) {
        de_cooked_submesh c = de_cooked_mesh_submesh(&v, s);
        sm[s].material.hash = c.material_id;
        memcpy(sm[s].bounds_min, c.bounds_min, sizeof sm[s].bounds_min);
        memcpy(sm[s].bounds_max, c.bounds_max, sizeof sm[s].bounds_max);
    }

    /* The GPU sections: every stream into the vertex buffer, then the meshlet sections and the
     * indices into their own buffers. */
    section sec[DE_COOKED_STREAM_COUNT + 4];
    u32 nsec = 0;
    u64 vb = 0;
    for (u32 s = 0; s < DE_COOKED_STREAM_COUNT; ++s) {
        m->stream_offset[s] = UINT32_MAX;
        if (v.streams[s] == NULL)
            continue;
        const u64 len = (u64)v.vertex_count * de_cooked_vertex_format_size(v.stream_format[s]);
        vb = align_up(vb, STREAM_ALIGN);
        m->stream_offset[s] = (u32)vb;
        m->stream_format[s] = v.stream_format[s];
        sec[nsec++] = (section){(u64)(v.streams[s] - v.base), len, DE_RENDER_MESH_VERTICES, vb};
        vb += len;
    }
    m->buffer_bytes[DE_RENDER_MESH_VERTICES] = vb;
    const struct {
        const u8 *at;
        u64 len;
        u32 buffer;
    } rest[] = {
        {v.meshlets, (u64)v.meshlet_count * DE_COOKED_MESHLET_SIZE, DE_RENDER_MESH_MESHLETS},
        {v.meshlet_vertices, (u64)v.meshlet_vertex_count * 4u, DE_RENDER_MESH_MESHLET_VERTICES},
        {v.meshlet_triangles, v.meshlet_triangle_bytes, DE_RENDER_MESH_MESHLET_TRIANGLES},
        {v.indices, (u64)v.index_count * 4u, DE_RENDER_MESH_INDICES},
    };
    for (u32 i = 0; i < 4u; ++i) {
        if (rest[i].at == NULL || rest[i].len == 0)
            continue;
        m->buffer_bytes[rest[i].buffer] = rest[i].len;
        sec[nsec++] = (section){(u64)(rest[i].at - v.base), rest[i].len, rest[i].buffer, 0};
    }

    char name[48];
    static const char *const kind[DE_RENDER_MESH_BUFFER_COUNT] = {
        "mesh vertices", "mesh meshlets", "mesh meshlet vertices", "mesh meshlet triangles",
        "mesh indices"};
    u32 target_of[DE_RENDER_MESH_BUFFER_COUNT];
    de__upload_target targets[DE_RENDER_MESH_BUFFER_COUNT];
    u32 ntargets = 0;
    for (u32 b = 0; b < DE_RENDER_MESH_BUFFER_COUNT; ++b) {
        target_of[b] = UINT32_MAX;
        if (m->buffer_bytes[b] == 0)
            continue;
        de__loader_name(name, sizeof name, kind[b], de_asset_load_ctx_id(ctx));
        m->buffers[b] =
            de_gpu_buffer_create(u->dev, &(de_gpu_buffer_desc){.size = m->buffer_bytes[b],
                                                               .usage = buffer_usage(b),
                                                               .memory = DE_MEM_GPU_ONLY,
                                                               .name = name});
        if (!DE_HANDLE_VALID(m->buffers[b])) {
            const de_result r = de__loader_rhi_error();
            char msg[DE_ERROR_MESSAGE_MAX];
            snprintf(msg, sizeof msg, "%s", de__loader_rhi_message());
            mesh_free(u, m);
            return de_asset_load_ctx_fail(ctx, r, "cannot create the %s buffer: %s", kind[b], msg);
        }
        m->bindless[b] = de_gpu_buffer_bindless(u->dev, m->buffers[b]);
        m->gpu_bytes += m->buffer_bytes[b];
        target_of[b] = ntargets;
        memset(&targets[ntargets], 0, sizeof targets[ntargets]);
        targets[ntargets++].buffer = m->buffers[b];
    }

    /* Pieces: each section in ranges of at most DE__UPLOAD_MAX_PIECE (and the ring). */
    u64 max_piece = DE__UPLOAD_MAX_PIECE < u->ring_size ? DE__UPLOAD_MAX_PIECE : u->ring_size;
    max_piece &= ~(u64)(DE__UPLOAD_BUFFER_ALIGN - 1u);
    u32 npieces = 0;
    for (u32 i = 0; i < nsec; ++i)
        npieces += (u32)((sec[i].size + max_piece - 1u) / max_piece);
    de__upload *up = de__upload_new(u, npieces);
    if (up == NULL) {
        mesh_free(u, m);
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "no memory for the upload");
    }
    m->upload_ = up;
    memcpy(up->targets, targets, sizeof targets[0] * ntargets);
    up->target_count = ntargets;
    u32 k = 0;
    for (u32 i = 0; i < nsec; ++i)
        for (u64 at = 0; at < sec[i].size; at += max_piece) {
            de__upload_piece *pc = &up->pieces[k++];
            pc->blob_off = sec[i].blob_off + at;
            pc->size = sec[i].size - at < max_piece ? sec[i].size - at : max_piece;
            pc->target = target_of[sec[i].buffer];
            pc->dst_off = sec[i].dst_off + at;
        }
    de_result r = de__upload_layout(u, up);
    if (r == DE_OK)
        r = de__upload_submit(u, up, p);
    if (r != DE_OK) {
        char msg[DE_ERROR_MESSAGE_MAX];
        snprintf(msg, sizeof msg, "%s", de__loader_rhi_message());
        mesh_free(u, m);
        return de_asset_load_ctx_fail(ctx, r, "%s", msg);
    }
    de_asset_load_ctx_set_object(ctx, m, m->gpu_bytes);
    return DE_OK;
}

/* The retire list's free (DENG-422): the upload was released at the unload. */
static void mesh_retired(de_render_uploader *u, void *object) {
    mesh_free(u, (de_render_mesh *)object);
}

void de__mesh_unload(void *object, void *user) {
    de_render_uploader *u = (de_render_uploader *)user;
    de_render_mesh *m = (de_render_mesh *)object;
    if (m == NULL)
        return;
    de_mutex_lock(&u->lock);
    de_render_mesh **at = &u->published_meshes;
    while (*at && *at != m) at = &(*at)->published_next_;
    if (*at) *at = m->published_next_;
    m->published_ = false;
    de_mutex_unlock(&u->lock);
    /* Now: no more copies into it. Later (frame fence, DENG-422): the buffers and the record. */
    de__upload_release(u, m->upload_);
    m->upload_ = NULL;
    de__uploader_retire(u, &m->retire_, m, mesh_retired);
}

de_result de__mesh_ready(void *object, void *user) {
    de_render_uploader *u = user;
    de_render_mesh *m = object;
    de_result result = de__upload_poll(u, m->upload_);
    if (result == DE_OK) {
        de_mutex_lock(&u->lock);
        if (!m->published_) {
            m->published_next_ = u->published_meshes;
            u->published_meshes = m;
            m->published_ = true;
        }
        de_mutex_unlock(&u->lock);
    }
    return result;
}
