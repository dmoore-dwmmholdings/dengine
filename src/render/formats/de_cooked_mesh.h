/*
 * de_cooked_mesh.h: the cooked mesh format DMSH, version 2 (SPEC §11.6, §11.8, §12.5; DENG-101
 * made version 1, DENG-103 version 2). Internal; include as "render/formats/de_cooked_mesh.h".
 *
 * A `mesh` asset: a model's mesh sub-asset (`<model>#<node name>`), written by the model importer
 * (tools/cook/importers/model/mesh_cook.c) and read by the runtime mesh loader (DENG-105). One
 * blob holds the GPU-ready mesh: quantized vertex streams, a LOD chain with object-space errors,
 * meshlets with culling bounds per LOD for the mesh-shader and compute-raster paths, and a plain
 * u32 index buffer per LOD (fallback draws, BLAS builds in S16, debug).
 *
 * Every field is little-endian (de_cooked_le.h). Engine coordinates: Z-up, right-handed, metres
 * (SPEC §5.5). Every section starts at a DE_COOKED_MESH_ALIGN (256) byte offset, so a loader can
 * upload each one as a storage buffer range as it is; the gaps are zero. Layout:
 *
 *   header      DE_COOKED_MESH_HEADER_SIZE (256) bytes:
 *                 0  u8[4]  magic "DMSH"
 *                 4  u32    format version (DE_COOKED_MESH_VERSION)
 *                 8  u32    flags (DE_COOKED_MESH_FLAG_*)
 *                12  u32    vertex count V (>= 1)
 *                16  u32    submesh count M (1 to 65535)
 *                20  u32    LOD count L (1 to DE_COOKED_MESH_MAX_LODS)
 *                24  u32    stream count N (1 to DE_COOKED_STREAM_COUNT)
 *                28  u32    index format (DE_COOKED_INDEX_U32)
 *                32  u32    index count I (all LODs; a multiple of 3)
 *                36  u32    meshlet count C (0 without DE_COOKED_MESH_FLAG_MESHLETS)
 *                40  u32    meshlet vertex count MV (u32 entries)
 *                44  u32    meshlet triangle bytes MT (a multiple of 4)
 *                48  u32    total size of the blob
 *                52  u32    reserved, 0
 *                56  u32    stream table offset
 *                60  u32    submesh table offset
 *                64  u32    LOD table offset
 *                68  u32    LOD-submesh table offset
 *                72  u32    meshlet table offset (0 when C = 0)
 *                76  u32    meshlet vertex offset (0 when C = 0)
 *                80  u32    meshlet triangle offset (0 when C = 0)
 *                84  u32    index offset
 *                88  f32[3] AABB min of the decoded positions
 *               100  f32[3] AABB max
 *               112  f32[4] bounding sphere of the decoded positions: centre xyz, radius
 *               128  f32[3] position offset   (UNORM16X4 positions: p = offset + q * scale;
 *               140  f32[3] position scale     F32X3: offset 0, scale 1)
 *               152  f32[4] UV0 offset xy, scale xy (UNORM16X2: uv = offset + q * scale)
 *               168  f32[4] UV1 offset xy, scale xy
 *               184  f32    LOD error target: the screen-space error, as a fraction of the
 *                           viewport height, at which the runtime switches to the next LOD by
 *                           default (the `lod_error` setting)
 *               188  f32    max position error: the `max_position_error` setting (metres)
 *               192  u32[16] reserved, 0
 *   streams     N x 16: u32 semantic (de_cooked_stream), u32 format (de_cooked_vertex_format),
 *               u32 data offset, u32 data size (V x the format's size). Sorted by semantic, each
 *               semantic once; POSITION is always there. Streams are not interleaved; each one
 *               is a section of its own.
 *   submeshes   M x 32: u64 material asset ID (the default material; 0 = the engine's default),
 *               f32[3] bounds min, f32[3] bounds max (decoded positions of its LOD 0 triangles).
 *   LODs        L x 32, finest first: u32 index first, u32 index count (into the index buffer),
 *               u32 meshlet first, u32 meshlet count, u32 triangle count, u32 vertex count (the
 *               LOD uses vertices [0, vertex count) only: vertices are ordered coarsest LOD
 *               first, so a coarse LOD needs a prefix of every stream), f32 error, u32 reserved.
 *               error is the object-space error in metres (0 for LOD 0, non-decreasing);
 *               triangle counts strictly decrease.
 *   LOD-submesh L x M x 16, LOD-major (record l * M + s): u32 index first, u32 index count,
 *               u32 meshlet first, u32 meshlet count: submesh s within LOD l (inside the LOD's
 *               ranges, in submesh order). A submesh may have no triangles in a coarse LOD.
 *   meshlets    C x 64 (DE_COOKED_MESHLET_SIZE):
 *                 0  u32    vertex offset (into the meshlet vertex buffer)
 *                 4  u32    triangle offset (bytes into the meshlet triangle buffer; 4-aligned)
 *                 8  u32    vertex count (bits 0-7, <= 64) | triangle count (bits 8-15, <= 124)
 *                           << 8 | submesh index (bits 16-31) << 16
 *                12  u8[4]  normal cone as snorm8: axis xyz, cutoff (decode x / 127.0)
 *                16  f32[4] bounding sphere: centre xyz, radius (contains every vertex)
 *                32  f32[4] cone apex xyz, cone cutoff (= cos of the half angle)
 *                48  f32[4] cone axis xyz, reserved 0
 *               Backface test (perspective): the meshlet faces away from a camera at c when
 *               dot(normalize(apex - c), axis) >= cutoff. A meshlet whose triangles do not fit
 *               in a cone has cutoff 1 and axis 0 (never culled).
 *   meshlet vertices  MV x u32: per meshlet local vertex, the mesh vertex index (< V).
 *   meshlet triangles MT bytes: per meshlet, triangle count x 3 u8 local vertex indices (each <
 *               the meshlet's vertex count), padded with zeros to a multiple of 4.
 *   indices     I x u32: the triangle lists of every LOD (each < V), LOD by LOD.
 *
 * Vertex formats per semantic (de_cooked_stream_format_ok):
 *   POSITION   UNORM16X4 (x, y, z in the header's offset/scale, w = 0), or F32X3 when a 16-bit
 *              step would exceed max position error
 *   NORMAL     OCT16 (octahedral, 2 x snorm16)
 *   TANGENT    OCT16_SIGN (octahedral, 2 x snorm16; bit 0 of y set = bitangent sign -1)
 *   UV0, UV1   UNORM16X2 in the header's per-mesh UV bounds, or F32X2 when the UV range is too
 *              large for 16-bit steps (DE_COOKED_UV_UNORM_MAX_RANGE)
 *   COLOR0     UNORM8X4 (linear RGBA)
 *   JOINTS0    U8X4, or U16X4 when an index is above 255
 *   WEIGHTS0   UNORM8X4, the four weights sum to exactly 255
 *
 * Bump DE_COOKED_MESH_VERSION on any change.
 */
#ifndef DE_RENDER_FORMATS_COOKED_MESH_H
#define DE_RENDER_FORMATS_COOKED_MESH_H

#include "render/formats/de_cooked_le.h"

#include <dengine/de_base.h>

#include <math.h>

DE_EXTERN_C_BEGIN

#define DE_COOKED_MESH_MAGIC            "DMSH"
#define DE_COOKED_MESH_VERSION          2u
#define DE_COOKED_MESH_HEADER_SIZE      256u
#define DE_COOKED_MESH_ALIGN            256u
#define DE_COOKED_MESH_STREAM_SIZE      16u
#define DE_COOKED_MESH_SUBMESH_SIZE     32u
#define DE_COOKED_MESH_LOD_SIZE         32u
#define DE_COOKED_MESH_LOD_SUBMESH_SIZE 16u
#define DE_COOKED_MESHLET_SIZE          64u
#define DE_COOKED_MESH_MAX_LODS         8u
#define DE_COOKED_MESH_MAX_SUBMESHES    65535u
/** Meshlet limits (SPEC §11.6): 124 triangles keep a meshlet's triangle bytes 4-aligned
 * (3 x 124 = 372) and the triangle index inside 7 bits of the vis-buffer ID (DENG-2 addendum). */
#define DE_COOKED_MESHLET_MAX_VERTICES  64u
#define DE_COOKED_MESHLET_MAX_TRIANGLES 124u
/** UVs whose range (max - min, per axis) is above this are stored as F32X2: a 16-bit step of a
 * larger range is coarser than 1/2048, a texel of a 2048 texture. */
#define DE_COOKED_UV_UNORM_MAX_RANGE 32.0f
/** Index format: 32-bit unsigned. */
#define DE_COOKED_INDEX_U32 1u

/** Header flags. */
#define DE_COOKED_MESH_FLAG_MESHLETS 0x1u /**< the meshlet sections are present (C >= 1) */

/** Vertex stream semantics, in stream-table order. */
typedef enum de_cooked_stream {
    DE_COOKED_STREAM_POSITION = 0,
    DE_COOKED_STREAM_NORMAL,
    DE_COOKED_STREAM_TANGENT,
    DE_COOKED_STREAM_UV0,
    DE_COOKED_STREAM_UV1,
    DE_COOKED_STREAM_COLOR0,
    DE_COOKED_STREAM_JOINTS0, /**< indices into the skin's joint list */
    DE_COOKED_STREAM_WEIGHTS0,
    DE_COOKED_STREAM_COUNT
} de_cooked_stream;

/** Vertex element formats. */
typedef enum de_cooked_vertex_format {
    DE_COOKED_VF_F32X2 = 1,      /**< 8 bytes */
    DE_COOKED_VF_F32X3 = 2,      /**< 12 bytes */
    DE_COOKED_VF_F32X4 = 3,      /**< 16 bytes (not used by version 2) */
    DE_COOKED_VF_U16X4 = 4,      /**< 8 bytes */
    DE_COOKED_VF_UNORM16X4 = 5,  /**< 8 bytes: q / 65535 per component */
    DE_COOKED_VF_OCT16 = 6,      /**< 4 bytes: octahedral unit vector, 2 x snorm16 */
    DE_COOKED_VF_OCT16_SIGN = 7, /**< 4 bytes: OCT16, bit 0 of y = sign (set: -1) */
    DE_COOKED_VF_UNORM16X2 = 8,  /**< 4 bytes */
    DE_COOKED_VF_UNORM8X4 = 9,   /**< 4 bytes */
    DE_COOKED_VF_U8X4 = 10       /**< 4 bytes */
} de_cooked_vertex_format;

/** Bytes of one element of `format`, 0 for an unknown format. */
static inline u32 de_cooked_vertex_format_size(u32 format) {
    switch (format) {
    case DE_COOKED_VF_F32X2:
    case DE_COOKED_VF_U16X4:
    case DE_COOKED_VF_UNORM16X4:
        return 8u;
    case DE_COOKED_VF_F32X3:
        return 12u;
    case DE_COOKED_VF_F32X4:
        return 16u;
    case DE_COOKED_VF_OCT16:
    case DE_COOKED_VF_OCT16_SIGN:
    case DE_COOKED_VF_UNORM16X2:
    case DE_COOKED_VF_UNORM8X4:
    case DE_COOKED_VF_U8X4:
        return 4u;
    default:
        return 0u;
    }
}

/** True when version 2 may store semantic `s` in `format`. */
static inline bool de_cooked_stream_format_ok(u32 s, u32 format) {
    switch (s) {
    case DE_COOKED_STREAM_POSITION:
        return format == DE_COOKED_VF_UNORM16X4 || format == DE_COOKED_VF_F32X3;
    case DE_COOKED_STREAM_NORMAL:
        return format == DE_COOKED_VF_OCT16;
    case DE_COOKED_STREAM_TANGENT:
        return format == DE_COOKED_VF_OCT16_SIGN;
    case DE_COOKED_STREAM_UV0:
    case DE_COOKED_STREAM_UV1:
        return format == DE_COOKED_VF_UNORM16X2 || format == DE_COOKED_VF_F32X2;
    case DE_COOKED_STREAM_COLOR0:
    case DE_COOKED_STREAM_WEIGHTS0:
        return format == DE_COOKED_VF_UNORM8X4;
    case DE_COOKED_STREAM_JOINTS0:
        return format == DE_COOKED_VF_U8X4 || format == DE_COOKED_VF_U16X4;
    default:
        return false;
    }
}

/** One submesh as decoded by de_cooked_mesh_submesh. */
typedef struct de_cooked_submesh {
    u64 material_id;
    f32 bounds_min[3], bounds_max[3];
} de_cooked_submesh;

/** One LOD record (de_cooked_mesh_lod). */
typedef struct de_cooked_mesh_lod {
    u32 index_first, index_count;
    u32 meshlet_first, meshlet_count;
    u32 triangle_count;
    u32 vertex_count; /**< the LOD's vertices are [0, vertex_count) */
    f32 error;        /**< object-space error, metres */
} de_cooked_mesh_lod;

/** One submesh inside one LOD (de_cooked_mesh_lod_submesh). */
typedef struct de_cooked_mesh_range {
    u32 index_first, index_count;
    u32 meshlet_first, meshlet_count;
} de_cooked_mesh_range;

/** One meshlet record (de_cooked_mesh_meshlet). */
typedef struct de_cooked_meshlet {
    u32 vertex_offset;   /**< into the meshlet vertex buffer */
    u32 triangle_offset; /**< bytes into the meshlet triangle buffer */
    u32 vertex_count, triangle_count, submesh;
    f32 center[3], radius;
    f32 cone_apex[3], cone_axis[3], cone_cutoff;
    i8 cone_s8[4]; /**< axis xyz, cutoff as snorm8 */
} de_cooked_meshlet;

/** A checked view of a DMSH blob (pointers into it). */
typedef struct de_cooked_mesh_view {
    const u8 *base;
    u64 size;
    u32 flags;
    u32 vertex_count, submesh_count, lod_count, stream_count;
    u32 index_count, meshlet_count, meshlet_vertex_count, meshlet_triangle_bytes;
    f32 bounds_min[3], bounds_max[3];
    f32 sphere[4];
    f32 position_offset[3], position_scale[3];
    f32 uv_offset[2][2], uv_scale[2][2];
    f32 lod_error, max_position_error;
    /** Per semantic: the stream's bytes and format, or NULL / 0 when the mesh has none. */
    const u8 *streams[DE_COOKED_STREAM_COUNT];
    u32 stream_format[DE_COOKED_STREAM_COUNT];
    const u8 *submeshes;         /**< M x DE_COOKED_MESH_SUBMESH_SIZE */
    const u8 *lods;              /**< L x DE_COOKED_MESH_LOD_SIZE */
    const u8 *lod_submeshes;     /**< L x M x DE_COOKED_MESH_LOD_SUBMESH_SIZE */
    const u8 *meshlets;          /**< C x DE_COOKED_MESHLET_SIZE, NULL when C = 0 */
    const u8 *meshlet_vertices;  /**< MV x u32 */
    const u8 *meshlet_triangles; /**< MT bytes */
    const u8 *indices;           /**< I x u32 */
} de_cooked_mesh_view;

static inline u16 de__cooked_get_u16(const u8 *p) {
    return (u16)(p[0] | (p[1] << 8));
}

/* True when `off` is a section start: DE_COOKED_MESH_ALIGN aligned and [off, off + len) inside
 * the blob. */
static inline bool de__cooked_mesh_section_ok(u64 size, u64 off, u64 len) {
    return off % DE_COOKED_MESH_ALIGN == 0 && off >= DE_COOKED_MESH_HEADER_SIZE &&
           de_cooked_range_ok(size, off, len);
}

/**
 * Checks a DMSH blob and fills `out`: magic, version, every count, offset and size against the
 * blob, section alignment, the stream table (sorted, unique, known semantics in an allowed
 * format, POSITION present), the LOD and LOD-submesh ranges, the meshlet records (limits,
 * ranges, submesh), every index and meshlet vertex < V and every meshlet triangle index < the
 * meshlet's vertex count. Allocation-free. False on any error, with a short reason in `*why`
 * (nullable).
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
    out->submesh_count = de_cooked_get_u32(p + 16);
    out->lod_count = de_cooked_get_u32(p + 20);
    out->stream_count = de_cooked_get_u32(p + 24);
    u32 index_format = de_cooked_get_u32(p + 28);
    out->index_count = de_cooked_get_u32(p + 32);
    out->meshlet_count = de_cooked_get_u32(p + 36);
    out->meshlet_vertex_count = de_cooked_get_u32(p + 40);
    out->meshlet_triangle_bytes = de_cooked_get_u32(p + 44);
    u32 total = de_cooked_get_u32(p + 48);
    u32 stream_off = de_cooked_get_u32(p + 56), sub_off = de_cooked_get_u32(p + 60);
    u32 lod_off = de_cooked_get_u32(p + 64), lodsub_off = de_cooked_get_u32(p + 68);
    u32 ml_off = de_cooked_get_u32(p + 72), mlv_off = de_cooked_get_u32(p + 76);
    u32 mlt_off = de_cooked_get_u32(p + 80), index_off = de_cooked_get_u32(p + 84);
    for (u32 i = 0; i < 3; ++i) {
        out->bounds_min[i] = de_cooked_get_f32(p + 88 + 4 * i);
        out->bounds_max[i] = de_cooked_get_f32(p + 100 + 4 * i);
        out->position_offset[i] = de_cooked_get_f32(p + 128 + 4 * i);
        out->position_scale[i] = de_cooked_get_f32(p + 140 + 4 * i);
    }
    for (u32 i = 0; i < 4; ++i)
        out->sphere[i] = de_cooked_get_f32(p + 112 + 4 * i);
    for (u32 s = 0; s < 2; ++s)
        for (u32 i = 0; i < 2; ++i) {
            out->uv_offset[s][i] = de_cooked_get_f32(p + 152 + 16 * s + 4 * i);
            out->uv_scale[s][i] = de_cooked_get_f32(p + 160 + 16 * s + 4 * i);
        }
    out->lod_error = de_cooked_get_f32(p + 184);
    out->max_position_error = de_cooked_get_f32(p + 188);
    bool has_meshlets = (out->flags & DE_COOKED_MESH_FLAG_MESHLETS) != 0;
    if (total != size) {
        w = "size does not match the header";
        goto fail;
    }
    if (out->vertex_count == 0 || out->submesh_count == 0 ||
        out->submesh_count > DE_COOKED_MESH_MAX_SUBMESHES || out->lod_count == 0 ||
        out->lod_count > DE_COOKED_MESH_MAX_LODS || out->stream_count == 0 ||
        out->stream_count > DE_COOKED_STREAM_COUNT || index_format != DE_COOKED_INDEX_U32 ||
        out->index_count % 3u != 0 || (out->flags & ~DE_COOKED_MESH_FLAG_MESHLETS) != 0 ||
        has_meshlets != (out->meshlet_count != 0) || out->meshlet_triangle_bytes % 4u != 0) {
        w = "bad counts, flags or index format";
        goto fail;
    }
    if (!has_meshlets && (out->meshlet_vertex_count != 0 || out->meshlet_triangle_bytes != 0 ||
                          ml_off != 0 || mlv_off != 0 || mlt_off != 0)) {
        w = "meshlet sections without meshlets";
        goto fail;
    }
    u64 lodsub_count = (u64)out->lod_count * out->submesh_count;
    if (!de__cooked_mesh_section_ok(size, stream_off,
                                    (u64)out->stream_count * DE_COOKED_MESH_STREAM_SIZE) ||
        !de__cooked_mesh_section_ok(size, sub_off,
                                    (u64)out->submesh_count * DE_COOKED_MESH_SUBMESH_SIZE) ||
        !de__cooked_mesh_section_ok(size, lod_off, (u64)out->lod_count * DE_COOKED_MESH_LOD_SIZE) ||
        !de__cooked_mesh_section_ok(size, lodsub_off,
                                    lodsub_count * DE_COOKED_MESH_LOD_SUBMESH_SIZE) ||
        !de__cooked_mesh_section_ok(size, index_off, (u64)out->index_count * 4u) ||
        (has_meshlets &&
         (!de__cooked_mesh_section_ok(size, ml_off,
                                      (u64)out->meshlet_count * DE_COOKED_MESHLET_SIZE) ||
          !de__cooked_mesh_section_ok(size, mlv_off, (u64)out->meshlet_vertex_count * 4u) ||
          !de__cooked_mesh_section_ok(size, mlt_off, out->meshlet_triangle_bytes)))) {
        w = "a section is outside the blob or not aligned";
        goto fail;
    }
    u32 prev = 0;
    for (u32 i = 0; i < out->stream_count; ++i) {
        const u8 *r = p + stream_off + (u64)i * DE_COOKED_MESH_STREAM_SIZE;
        u32 sem = de_cooked_get_u32(r), fmt = de_cooked_get_u32(r + 4);
        u32 off = de_cooked_get_u32(r + 8), len = de_cooked_get_u32(r + 12);
        if (sem >= DE_COOKED_STREAM_COUNT || (i > 0 && sem <= prev) ||
            !de_cooked_stream_format_ok(sem, fmt) ||
            (u64)len != (u64)out->vertex_count * de_cooked_vertex_format_size(fmt) ||
            !de__cooked_mesh_section_ok(size, off, len)) {
            w = "bad stream record";
            goto fail;
        }
        out->streams[sem] = p + off;
        out->stream_format[sem] = fmt;
        prev = sem;
    }
    if (out->streams[DE_COOKED_STREAM_POSITION] == NULL) {
        w = "no POSITION stream";
        goto fail;
    }
    out->submeshes = p + sub_off;
    out->lods = p + lod_off;
    out->lod_submeshes = p + lodsub_off;
    out->indices = p + index_off;
    if (has_meshlets) {
        out->meshlets = p + ml_off;
        out->meshlet_vertices = p + mlv_off;
        out->meshlet_triangles = p + mlt_off;
    }
    u32 prev_tris = 0;
    f32 prev_error = 0.0f;
    for (u32 l = 0; l < out->lod_count; ++l) {
        const u8 *r = out->lods + (u64)l * DE_COOKED_MESH_LOD_SIZE;
        u32 ifirst = de_cooked_get_u32(r), icount = de_cooked_get_u32(r + 4);
        u32 mfirst = de_cooked_get_u32(r + 8), mcount = de_cooked_get_u32(r + 12);
        u32 tris = de_cooked_get_u32(r + 16), verts = de_cooked_get_u32(r + 20);
        f32 err = de_cooked_get_f32(r + 24);
        if (icount % 3u != 0 || tris != icount / 3u ||
            !de_cooked_range_ok(out->index_count, ifirst, icount) ||
            !de_cooked_range_ok(out->meshlet_count, mfirst, mcount) ||
            (has_meshlets != (mcount != 0) && tris != 0) || verts > out->vertex_count ||
            !(err >= prev_error) || (l > 0 && tris >= prev_tris)) {
            w = "bad LOD record";
            goto fail;
        }
        for (u32 s = 0; s < out->submesh_count; ++s) {
            const u8 *q = out->lod_submeshes +
                          ((u64)l * out->submesh_count + s) * DE_COOKED_MESH_LOD_SUBMESH_SIZE;
            u32 sif = de_cooked_get_u32(q), sic = de_cooked_get_u32(q + 4);
            u32 smf = de_cooked_get_u32(q + 8), smc = de_cooked_get_u32(q + 12);
            if (sic % 3u != 0 || sif < ifirst || !de_cooked_range_ok(ifirst + icount, sif, sic) ||
                smf < mfirst || !de_cooked_range_ok(mfirst + mcount, smf, smc)) {
                w = "bad LOD-submesh record";
                goto fail;
            }
        }
        for (u32 i = 0; i < icount; ++i)
            if (de_cooked_get_u32(out->indices + 4u * ((u64)ifirst + i)) >= verts) {
                w = "index outside the LOD's vertices";
                goto fail;
            }
        prev_tris = tris;
        prev_error = err;
    }
    for (u32 i = 0; i < out->index_count; ++i)
        if (de_cooked_get_u32(out->indices + 4u * (u64)i) >= out->vertex_count) {
            w = "index out of range";
            goto fail;
        }
    for (u32 i = 0; i < out->meshlet_vertex_count; ++i)
        if (de_cooked_get_u32(out->meshlet_vertices + 4u * (u64)i) >= out->vertex_count) {
            w = "meshlet vertex out of range";
            goto fail;
        }
    for (u32 i = 0; i < out->meshlet_count; ++i) {
        const u8 *r = out->meshlets + (u64)i * DE_COOKED_MESHLET_SIZE;
        u32 voff = de_cooked_get_u32(r), toff = de_cooked_get_u32(r + 4);
        u32 counts = de_cooked_get_u32(r + 8);
        u32 vc = counts & 0xFFu, tc = (counts >> 8) & 0xFFu, sm = counts >> 16;
        if (vc == 0 || vc > DE_COOKED_MESHLET_MAX_VERTICES || tc == 0 ||
            tc > DE_COOKED_MESHLET_MAX_TRIANGLES || sm >= out->submesh_count || toff % 4u != 0 ||
            !de_cooked_range_ok(out->meshlet_vertex_count, voff, vc) ||
            !de_cooked_range_ok(out->meshlet_triangle_bytes, toff, 3u * tc)) {
            w = "bad meshlet record";
            goto fail;
        }
        for (u32 k = 0; k < 3u * tc; ++k)
            if (out->meshlet_triangles[toff + k] >= vc) {
                w = "meshlet triangle index out of range";
                goto fail;
            }
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
    s.material_id = de_cooked_get_u64(r);
    for (u32 k = 0; k < 3; ++k) {
        s.bounds_min[k] = de_cooked_get_f32(r + 8 + 4 * k);
        s.bounds_max[k] = de_cooked_get_f32(r + 20 + 4 * k);
    }
    return s;
}

/** LOD `l` (< lod_count). */
static inline de_cooked_mesh_lod de_cooked_mesh_get_lod(const de_cooked_mesh_view *v, u32 l) {
    const u8 *r = v->lods + (u64)l * DE_COOKED_MESH_LOD_SIZE;
    de_cooked_mesh_lod o;
    o.index_first = de_cooked_get_u32(r);
    o.index_count = de_cooked_get_u32(r + 4);
    o.meshlet_first = de_cooked_get_u32(r + 8);
    o.meshlet_count = de_cooked_get_u32(r + 12);
    o.triangle_count = de_cooked_get_u32(r + 16);
    o.vertex_count = de_cooked_get_u32(r + 20);
    o.error = de_cooked_get_f32(r + 24);
    return o;
}

/** Submesh `s` within LOD `l`. */
static inline de_cooked_mesh_range de_cooked_mesh_lod_submesh(const de_cooked_mesh_view *v, u32 l,
                                                              u32 s) {
    const u8 *r =
        v->lod_submeshes + ((u64)l * v->submesh_count + s) * DE_COOKED_MESH_LOD_SUBMESH_SIZE;
    de_cooked_mesh_range o;
    o.index_first = de_cooked_get_u32(r);
    o.index_count = de_cooked_get_u32(r + 4);
    o.meshlet_first = de_cooked_get_u32(r + 8);
    o.meshlet_count = de_cooked_get_u32(r + 12);
    return o;
}

/** Meshlet `i` (< meshlet_count). */
static inline de_cooked_meshlet de_cooked_mesh_meshlet(const de_cooked_mesh_view *v, u32 i) {
    const u8 *r = v->meshlets + (u64)i * DE_COOKED_MESHLET_SIZE;
    de_cooked_meshlet m;
    m.vertex_offset = de_cooked_get_u32(r);
    m.triangle_offset = de_cooked_get_u32(r + 4);
    u32 counts = de_cooked_get_u32(r + 8);
    m.vertex_count = counts & 0xFFu;
    m.triangle_count = (counts >> 8) & 0xFFu;
    m.submesh = counts >> 16;
    for (u32 k = 0; k < 4; ++k)
        m.cone_s8[k] = (i8)r[12 + k];
    for (u32 k = 0; k < 3; ++k) {
        m.center[k] = de_cooked_get_f32(r + 16 + 4 * k);
        m.cone_apex[k] = de_cooked_get_f32(r + 32 + 4 * k);
        m.cone_axis[k] = de_cooked_get_f32(r + 48 + 4 * k);
    }
    m.radius = de_cooked_get_f32(r + 28);
    m.cone_cutoff = de_cooked_get_f32(r + 44);
    return m;
}

/** Index `i` (< index_count) of the index buffer. */
static inline u32 de_cooked_mesh_index(const de_cooked_mesh_view *v, u32 i) {
    return de_cooked_get_u32(v->indices + 4u * (u64)i);
}

/** Entry `i` (< meshlet_vertex_count) of the meshlet vertex buffer: a mesh vertex index. */
static inline u32 de_cooked_mesh_meshlet_vertex(const de_cooked_mesh_view *v, u32 i) {
    return de_cooked_get_u32(v->meshlet_vertices + 4u * (u64)i);
}

/* ---- Vertex decoding ------------------------------------------------------------------------- */

/** snorm16 → [-1, 1]. */
static inline f32 de_cooked_snorm16(i16 q) {
    f32 f = (f32)q / 32767.0f;
    return f < -1.0f ? -1.0f : f;
}

/** Octahedral (2 x snorm16) → unit vector. */
static inline void de_cooked_oct_decode(i16 qx, i16 qy, f32 out[3]) {
    f32 x = de_cooked_snorm16(qx), y = de_cooked_snorm16(qy);
    f32 z = 1.0f - fabsf(x) - fabsf(y);
    f32 t = z < 0.0f ? -z : 0.0f;
    x += x >= 0.0f ? -t : t;
    y += y >= 0.0f ? -t : t;
    f32 len = sqrtf(x * x + y * y + z * z);
    out[0] = x / len;
    out[1] = y / len;
    out[2] = z / len;
}

/** Position of vertex `i` (< vertex_count), dequantized. */
static inline void de_cooked_mesh_position(const de_cooked_mesh_view *v, u32 i, f32 out[3]) {
    const u8 *s = v->streams[DE_COOKED_STREAM_POSITION];
    if (v->stream_format[DE_COOKED_STREAM_POSITION] == DE_COOKED_VF_F32X3) {
        for (u32 k = 0; k < 3; ++k)
            out[k] = de_cooked_get_f32(s + 12u * (u64)i + 4u * k);
        return;
    }
    for (u32 k = 0; k < 3; ++k)
        out[k] = v->position_offset[k] +
                 (f32)de__cooked_get_u16(s + 8u * (u64)i + 2u * k) * v->position_scale[k];
}

/** Normal of vertex `i`; false when the mesh has no normals. */
static inline bool de_cooked_mesh_normal(const de_cooked_mesh_view *v, u32 i, f32 out[3]) {
    const u8 *s = v->streams[DE_COOKED_STREAM_NORMAL];
    if (s == NULL)
        return false;
    de_cooked_oct_decode((i16)de__cooked_get_u16(s + 4u * (u64)i),
                         (i16)de__cooked_get_u16(s + 4u * (u64)i + 2u), out);
    return true;
}

/** Tangent of vertex `i`: xyz unit length, w = bitangent sign (+1 or -1); false when none. */
static inline bool de_cooked_mesh_tangent(const de_cooked_mesh_view *v, u32 i, f32 out[4]) {
    const u8 *s = v->streams[DE_COOKED_STREAM_TANGENT];
    if (s == NULL)
        return false;
    u16 qy = de__cooked_get_u16(s + 4u * (u64)i + 2u);
    de_cooked_oct_decode((i16)de__cooked_get_u16(s + 4u * (u64)i), (i16)(u16)(qy & 0xFFFEu), out);
    out[3] = (qy & 1u) != 0 ? -1.0f : 1.0f;
    return true;
}

/** UV set `set` (0 or 1) of vertex `i`; false when the mesh has no such set. */
static inline bool de_cooked_mesh_uv(const de_cooked_mesh_view *v, u32 set, u32 i, f32 out[2]) {
    u32 sem = set == 0 ? DE_COOKED_STREAM_UV0 : DE_COOKED_STREAM_UV1;
    const u8 *s = v->streams[sem];
    if (s == NULL)
        return false;
    if (v->stream_format[sem] == DE_COOKED_VF_F32X2) {
        out[0] = de_cooked_get_f32(s + 8u * (u64)i);
        out[1] = de_cooked_get_f32(s + 8u * (u64)i + 4u);
        return true;
    }
    for (u32 k = 0; k < 2; ++k)
        out[k] = v->uv_offset[set][k] +
                 (f32)de__cooked_get_u16(s + 4u * (u64)i + 2u * k) * v->uv_scale[set][k];
    return true;
}

/** Linear RGBA colour of vertex `i`; false when none. */
static inline bool de_cooked_mesh_color(const de_cooked_mesh_view *v, u32 i, f32 out[4]) {
    const u8 *s = v->streams[DE_COOKED_STREAM_COLOR0];
    if (s == NULL)
        return false;
    for (u32 k = 0; k < 4; ++k)
        out[k] = (f32)s[4u * (u64)i + k] / 255.0f;
    return true;
}

/** Joint indices of vertex `i`; false when none. */
static inline bool de_cooked_mesh_joints(const de_cooked_mesh_view *v, u32 i, u16 out[4]) {
    const u8 *s = v->streams[DE_COOKED_STREAM_JOINTS0];
    if (s == NULL)
        return false;
    bool wide = v->stream_format[DE_COOKED_STREAM_JOINTS0] == DE_COOKED_VF_U16X4;
    for (u32 k = 0; k < 4; ++k)
        out[k] = wide ? de__cooked_get_u16(s + 8u * (u64)i + 2u * k) : s[4u * (u64)i + k];
    return true;
}

/** Skin weights of vertex `i` as stored (unorm8, the four sum to 255); false when none. */
static inline bool de_cooked_mesh_weights(const de_cooked_mesh_view *v, u32 i, u8 out[4]) {
    const u8 *s = v->streams[DE_COOKED_STREAM_WEIGHTS0];
    if (s == NULL)
        return false;
    memcpy(out, s + 4u * (u64)i, 4);
    return true;
}

DE_EXTERN_C_END

#endif /* DE_RENDER_FORMATS_COOKED_MESH_H */
