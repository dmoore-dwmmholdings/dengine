/*
 * de_cooked_mesh.h: the cooked mesh format DMSH, version 1 (SPEC §11.6; DENG-101). Internal;
 * include as "render/formats/de_cooked_mesh.h".
 *
 * A `mesh` asset: a model's mesh sub-asset (`<model>#<node name>`), written by the model importer
 * (tools/cook/importers/model/model_cook.c) and read by the runtime mesh loader (DENG-105).
 * DENG-103 extends it with meshlets, quantized attributes and the LOD chain (version 2).
 *
 * Every field is little-endian (de_cooked_le.h). Engine coordinates: Z-up, right-handed, metres
 * (SPEC §5.5); the importer has converted the source already. Layout:
 *
 *   header      DE_COOKED_MESH_HEADER_SIZE (80) bytes:
 *                 0  u8[4]  magic "DMSH"
 *                 4  u32    format version (DE_COOKED_MESH_VERSION)
 *                 8  u32    flags (0 in version 1)
 *                12  u32    vertex count V (>= 1)
 *                16  u32    index count I (a multiple of 3: a triangle list)
 *                20  u32    submesh count M (>= 1)
 *                24  u32    stream count N (1 to DE_COOKED_STREAM_COUNT)
 *                28  u32    index format (DE_COOKED_INDEX_U32)
 *                32  u32    stream table offset (80)
 *                36  u32    submesh table offset
 *                40  u32    index data offset
 *                44  u32    total size of the blob
 *                48  f32[3] bounds min (all vertices)
 *                60  f32[3] bounds max
 *                72  u32[2] reserved, 0
 *   streams     N x 16: u32 semantic (de_cooked_stream), u32 format (de_cooked_vertex_format),
 *               u32 data offset, u32 data size (V x the format's size). Sorted by semantic, each
 *               semantic once; POSITION is always there. Streams are not interleaved.
 *   submeshes   M x 48: u32 index first, u32 index count (a multiple of 3), u32 vertex first,
 *               u32 vertex count (the vertices its indices use), u64 material asset ID (the
 *               default material; 0 = the engine's default), f32[3] bounds min, f32[3] bounds max.
 *   data        each stream, then the indices (I x u32, each < V). Every block starts at a
 *               16-byte aligned offset; the gaps are zero.
 *
 * Bump DE_COOKED_MESH_VERSION on any change.
 */
#ifndef DE_RENDER_FORMATS_COOKED_MESH_H
#define DE_RENDER_FORMATS_COOKED_MESH_H

#include "render/formats/de_cooked_le.h"

#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

#define DE_COOKED_MESH_MAGIC        "DMSH"
#define DE_COOKED_MESH_VERSION      1u
#define DE_COOKED_MESH_HEADER_SIZE  80u
#define DE_COOKED_MESH_STREAM_SIZE  16u
#define DE_COOKED_MESH_SUBMESH_SIZE 48u
/** Index format: 32-bit unsigned. */
#define DE_COOKED_INDEX_U32 1u

/** Vertex stream semantics, in stream-table order. */
typedef enum de_cooked_stream {
    DE_COOKED_STREAM_POSITION = 0, /**< F32X3 */
    DE_COOKED_STREAM_NORMAL,       /**< F32X3, unit length */
    DE_COOKED_STREAM_TANGENT,      /**< F32X4: xyz unit length, w = bitangent sign (+1 or -1) */
    DE_COOKED_STREAM_UV0,          /**< F32X2 */
    DE_COOKED_STREAM_UV1,          /**< F32X2 */
    DE_COOKED_STREAM_COLOR0,       /**< F32X4, linear RGBA */
    DE_COOKED_STREAM_JOINTS0,      /**< U16X4, indices into the skin's joint list */
    DE_COOKED_STREAM_WEIGHTS0,     /**< F32X4 */
    DE_COOKED_STREAM_COUNT
} de_cooked_stream;

/** Vertex element formats. */
typedef enum de_cooked_vertex_format {
    DE_COOKED_VF_F32X2 = 1,
    DE_COOKED_VF_F32X3 = 2,
    DE_COOKED_VF_F32X4 = 3,
    DE_COOKED_VF_U16X4 = 4
} de_cooked_vertex_format;

/** Bytes of one element of `format`, 0 for an unknown format. */
static inline u32 de_cooked_vertex_format_size(u32 format) {
    switch (format) {
    case DE_COOKED_VF_F32X2:
        return 8u;
    case DE_COOKED_VF_F32X3:
        return 12u;
    case DE_COOKED_VF_F32X4:
        return 16u;
    case DE_COOKED_VF_U16X4:
        return 8u;
    default:
        return 0u;
    }
}

/** The format version 1 stores semantic `s` in. */
static inline u32 de_cooked_stream_format(u32 s) {
    switch (s) {
    case DE_COOKED_STREAM_POSITION:
    case DE_COOKED_STREAM_NORMAL:
        return DE_COOKED_VF_F32X3;
    case DE_COOKED_STREAM_UV0:
    case DE_COOKED_STREAM_UV1:
        return DE_COOKED_VF_F32X2;
    case DE_COOKED_STREAM_JOINTS0:
        return DE_COOKED_VF_U16X4;
    case DE_COOKED_STREAM_TANGENT:
    case DE_COOKED_STREAM_COLOR0:
    case DE_COOKED_STREAM_WEIGHTS0:
        return DE_COOKED_VF_F32X4;
    default:
        return 0u;
    }
}

/** One submesh as decoded by de_cooked_mesh_submesh. */
typedef struct de_cooked_submesh {
    u32 index_first, index_count;
    u32 vertex_first, vertex_count;
    u64 material_id;
    f32 bounds_min[3], bounds_max[3];
} de_cooked_submesh;

/** A checked view of a DMSH blob (pointers into it). */
typedef struct de_cooked_mesh_view {
    const u8 *base;
    u64 size;
    u32 flags;
    u32 vertex_count, index_count, submesh_count, stream_count;
    f32 bounds_min[3], bounds_max[3];
    /** Per semantic: the stream's bytes, or NULL when the mesh has no such stream. */
    const u8 *streams[DE_COOKED_STREAM_COUNT];
    const u8 *submeshes; /**< M x DE_COOKED_MESH_SUBMESH_SIZE records */
    const u8 *indices;   /**< I x u32, little-endian */
} de_cooked_mesh_view;

/**
 * Checks a DMSH blob and fills `out`: magic, version, every count, offset and size against the
 * blob, the stream table (sorted, unique, known semantics in their version-1 format, POSITION
 * present), the submesh ranges, and every index < V. Allocation-free. False on any error, with a
 * short reason in `*why` (nullable).
 */
static inline bool de_cooked_mesh_parse(const void *data, u64 size, de_cooked_mesh_view *out,
                                        const char **why) {
    const char *w = NULL;
    const u8 *p = (const u8 *)data;
    memset(out, 0, sizeof *out);
    if (p == NULL || size < DE_COOKED_MESH_HEADER_SIZE) {
        w = "shorter than the header";
        goto fail;
    }
    if (memcmp(p, DE_COOKED_MESH_MAGIC, 4) != 0) {
        w = "not a cooked mesh (magic)";
        goto fail;
    }
    if (de_cooked_get_u32(p + 4) != DE_COOKED_MESH_VERSION) {
        w = "unsupported format version";
        goto fail;
    }
    out->base = p;
    out->size = size;
    out->flags = de_cooked_get_u32(p + 8);
    out->vertex_count = de_cooked_get_u32(p + 12);
    out->index_count = de_cooked_get_u32(p + 16);
    out->submesh_count = de_cooked_get_u32(p + 20);
    out->stream_count = de_cooked_get_u32(p + 24);
    u32 index_format = de_cooked_get_u32(p + 28);
    u32 stream_off = de_cooked_get_u32(p + 32);
    u32 sub_off = de_cooked_get_u32(p + 36);
    u32 index_off = de_cooked_get_u32(p + 40);
    u32 total = de_cooked_get_u32(p + 44);
    for (u32 i = 0; i < 3; ++i) {
        out->bounds_min[i] = de_cooked_get_f32(p + 48 + 4 * i);
        out->bounds_max[i] = de_cooked_get_f32(p + 60 + 4 * i);
    }
    if (total != size) {
        w = "size does not match the header";
        goto fail;
    }
    if (out->vertex_count == 0 || out->submesh_count == 0 || out->index_count % 3u != 0 ||
        out->stream_count == 0 || out->stream_count > DE_COOKED_STREAM_COUNT ||
        index_format != DE_COOKED_INDEX_U32) {
        w = "bad counts or index format";
        goto fail;
    }
    if (!de_cooked_range_ok(size, stream_off,
                            (u64)out->stream_count * DE_COOKED_MESH_STREAM_SIZE) ||
        !de_cooked_range_ok(size, sub_off, (u64)out->submesh_count * DE_COOKED_MESH_SUBMESH_SIZE) ||
        !de_cooked_range_ok(size, index_off, (u64)out->index_count * 4u) ||
        stream_off < DE_COOKED_MESH_HEADER_SIZE || index_off % 4u != 0) {
        w = "table or index data outside the blob";
        goto fail;
    }
    u32 prev = 0;
    for (u32 i = 0; i < out->stream_count; ++i) {
        const u8 *r = p + stream_off + (u64)i * DE_COOKED_MESH_STREAM_SIZE;
        u32 sem = de_cooked_get_u32(r), fmt = de_cooked_get_u32(r + 4);
        u32 off = de_cooked_get_u32(r + 8), len = de_cooked_get_u32(r + 12);
        if (sem >= DE_COOKED_STREAM_COUNT || (i > 0 && sem <= prev) ||
            fmt != de_cooked_stream_format(sem) || off % 4u != 0 ||
            (u64)len != (u64)out->vertex_count * de_cooked_vertex_format_size(fmt) ||
            !de_cooked_range_ok(size, off, len)) {
            w = "bad stream record";
            goto fail;
        }
        out->streams[sem] = p + off;
        prev = sem;
    }
    if (out->streams[DE_COOKED_STREAM_POSITION] == NULL) {
        w = "no POSITION stream";
        goto fail;
    }
    out->submeshes = p + sub_off;
    for (u32 i = 0; i < out->submesh_count; ++i) {
        const u8 *r = out->submeshes + (u64)i * DE_COOKED_MESH_SUBMESH_SIZE;
        u32 ifirst = de_cooked_get_u32(r), icount = de_cooked_get_u32(r + 4);
        u32 vfirst = de_cooked_get_u32(r + 8), vcount = de_cooked_get_u32(r + 12);
        if (icount % 3u != 0 || !de_cooked_range_ok(out->index_count, ifirst, icount) ||
            !de_cooked_range_ok(out->vertex_count, vfirst, vcount)) {
            w = "bad submesh range";
            goto fail;
        }
    }
    out->indices = p + index_off;
    for (u32 i = 0; i < out->index_count; ++i)
        if (de_cooked_get_u32(out->indices + 4u * (u64)i) >= out->vertex_count) {
            w = "index out of range";
            goto fail;
        }
    return true;
fail:
    memset(out, 0, sizeof *out);
    if (why != NULL)
        *why = w;
    return false;
}

/** Submesh `i` (< submesh_count) of a parsed mesh. */
static inline de_cooked_submesh de_cooked_mesh_submesh(const de_cooked_mesh_view *v, u32 i) {
    const u8 *r = v->submeshes + (u64)i * DE_COOKED_MESH_SUBMESH_SIZE;
    de_cooked_submesh s;
    s.index_first = de_cooked_get_u32(r);
    s.index_count = de_cooked_get_u32(r + 4);
    s.vertex_first = de_cooked_get_u32(r + 8);
    s.vertex_count = de_cooked_get_u32(r + 12);
    s.material_id = de_cooked_get_u64(r + 16);
    for (u32 k = 0; k < 3; ++k) {
        s.bounds_min[k] = de_cooked_get_f32(r + 24 + 4 * k);
        s.bounds_max[k] = de_cooked_get_f32(r + 36 + 4 * k);
    }
    return s;
}

DE_EXTERN_C_END

#endif /* DE_RENDER_FORMATS_COOKED_MESH_H */
