/*
 * gltf_import.c: the glTF 2.0 front end of the model importer (DENG-101, SPEC §11.6, §12.6).
 *
 * cgltf parses the JSON (or the GLB container); this file checks everything it reads and turns
 * the document into the model IR (model_ir.h), in glTF coordinates. In order:
 *   1. parse (cgltf, our allocator), resolve buffers (data URIs, the GLB chunk, files through the
 *      source's read callback), check the extensions (Draco and meshopt compression rejected when
 *      required, unknown required extensions rejected, unknown used ones warned about);
 *   2. check every accessor's bounds and alignment ourselves, then cgltf_validate (node cycles,
 *      attribute counts, index bounds);
 *   3. materials (core + KHR_materials_*), and their textures: one IR texture per (image, usage),
 *      in order of first use;
 *   4. meshes: each primitive is decoded on its own (sparse and normalized accessors,
 *      KHR_mesh_quantization, strips and fans to lists, points and lines skipped), gets generated
 *      normals and MikkTSpace tangents when it needs them and the baked KHR_texture_transform,
 *      then the primitives of a mesh are merged into one vertex/index buffer with one submesh
 *      each;
 *   5. nodes (TRS, or a decomposed matrix), the scene's roots, skins and animations (raw).
 *
 * Accessor data is decoded with our own reader (memcpy, little-endian), so a malformed file can
 * neither read out of bounds nor trip alignment checks, and sparse accessors are applied by hand.
 */
#include "model_internal.h"
#include "model_ir.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include <cgltf.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Limits that keep a hostile file from asking for absurd amounts of memory. */
#define GLTF_MAX_COUNT   (1u << 26) /* elements per accessor */
#define GLTF_MAX_UV_SETS 2u

typedef struct gltf_ctx {
    const de_model_source *src;
    de_model_ir *ir;
    cgltf_data *data;
    /* Per material: the bake plan of KHR_texture_transform per UV set. */
    struct uv_bake *bakes;
    bool warned_uv_sets, warned_attrs, warned_targets;
} gltf_ctx;

typedef struct uv_xform {
    f32 offset[2], rotation, scale[2];
} uv_xform;

typedef struct uv_bake {
    bool apply[GLTF_MAX_UV_SETS];
    uv_xform xf[GLTF_MAX_UV_SETS];
} uv_bake;

/* ---- cgltf callbacks ------------------------------------------------------------------------- */

typedef struct alloc_head {
    size_t size;
    size_t pad;
} alloc_head;

static void *cg_alloc(void *user, cgltf_size size) {
    const de_allocator *a = ((const de_model_ir *)user)->alloc;
    if (size > SIZE_MAX / 2u)
        return NULL;
    size_t total = sizeof(alloc_head) + (size_t)size;
    alloc_head *h = (alloc_head *)DE_ALLOC(a, total, 16, DE_MEM_ASSET);
    if (h == NULL)
        return NULL;
    h->size = total;
    return h + 1;
}

static void cg_free(void *user, void *ptr) {
    if (ptr == NULL)
        return;
    const de_allocator *a = ((const de_model_ir *)user)->alloc;
    alloc_head *h = (alloc_head *)ptr - 1;
    DE_FREE(a, h, h->size);
}

static cgltf_result cg_read(const struct cgltf_memory_options *mem,
                            const struct cgltf_file_options *file, const char *path,
                            cgltf_size *size, void **data) {
    (void)mem;
    gltf_ctx *g = (gltf_ctx *)file->user_data;
    const void *bytes = NULL;
    u64 n = 0;
    if (g->src->read_file == NULL)
        return cgltf_result_file_not_found;
    de_result r = g->src->read_file(g->src->read_user, path, &bytes, &n);
    if (r != DE_OK)
        return r == DE_ERR_NOT_FOUND ? cgltf_result_file_not_found : cgltf_result_io_error;
    /* cgltf asks for the declared buffer size; a shorter file would be read past its end. */
    if (*size != 0 && n < (u64)*size)
        return cgltf_result_data_too_short;
    *size = (cgltf_size)n;
    *data = (void *)(uintptr_t)bytes; /* owned by the reader until the import ends */
    return cgltf_result_success;
}

static void cg_release(const struct cgltf_memory_options *mem,
                       const struct cgltf_file_options *file, void *data) {
    (void)mem;
    (void)file;
    (void)data; /* the reader owns the bytes */
}

static const char *cg_result_str(cgltf_result r) {
    switch (r) {
    case cgltf_result_success:
        return "success";
    case cgltf_result_data_too_short:
        return "the data is truncated (shorter than the file or a buffer says)";
    case cgltf_result_unknown_format:
        return "not a glTF file, or a URI scheme that is not supported";
    case cgltf_result_invalid_json:
        return "invalid JSON";
    case cgltf_result_invalid_gltf:
        return "invalid glTF";
    case cgltf_result_invalid_options:
        return "invalid options";
    case cgltf_result_file_not_found:
        return "a referenced file was not found";
    case cgltf_result_io_error:
        return "a referenced file could not be read (or a bad data URI)";
    case cgltf_result_out_of_memory:
        return "out of memory";
    case cgltf_result_legacy_gltf:
        return "glTF 1.0 is not supported";
    default:
        return "unknown error";
    }
}

static de_result cg_error(const gltf_ctx *g, cgltf_result r, const char *stage) {
    de_result code = DE_ERR_PARSE;
    if (r == cgltf_result_out_of_memory)
        code = DE_ERR_OUT_OF_MEMORY;
    else if (r == cgltf_result_file_not_found)
        code = DE_ERR_NOT_FOUND;
    else if (r == cgltf_result_io_error)
        code = DE_ERR_IO;
    return DE_FAIL(code, "%s: %s: %s", g->src->path, stage, cg_result_str(r));
}

/* ---- Extensions ------------------------------------------------------------------------------ */

static const char *const k_supported_ext[] = {
    "KHR_materials_emissive_strength",
    "KHR_materials_ior",
    "KHR_materials_specular",
    "KHR_materials_clearcoat",
    "KHR_materials_sheen",
    "KHR_materials_transmission",
    "KHR_materials_volume",
    "KHR_materials_diffuse_transmission",
    "KHR_materials_anisotropy",
    "KHR_materials_iridescence",
    "KHR_materials_unlit",
    "KHR_mesh_quantization",
    "KHR_texture_transform",
    "KHR_texture_basisu",
};

static bool ext_supported(const char *name) {
    for (size_t i = 0; i < sizeof k_supported_ext / sizeof k_supported_ext[0]; ++i)
        if (strcmp(name, k_supported_ext[i]) == 0)
            return true;
    return false;
}

static bool ext_is_compression(const char *name) {
    return strcmp(name, "KHR_draco_mesh_compression") == 0 ||
           strcmp(name, "EXT_meshopt_compression") == 0 ||
           strcmp(name, "KHR_meshopt_compression") == 0;
}

static de_result check_extensions(gltf_ctx *g) {
    const cgltf_data *d = g->data;
    for (cgltf_size i = 0; i < d->extensions_required_count; ++i) {
        const char *e = d->extensions_required[i];
        if (ext_is_compression(e))
            return DE_FAIL(DE_ERR_UNSUPPORTED,
                           "%s: the file requires %s, which the importer does not decode; export "
                           "it without mesh compression",
                           g->src->path, e);
        if (!ext_supported(e))
            return DE_FAIL(DE_ERR_UNSUPPORTED,
                           "%s: the file requires the extension %s, which the importer does not "
                           "support",
                           g->src->path, e);
    }
    for (cgltf_size i = 0; i < d->extensions_used_count; ++i) {
        const char *e = d->extensions_used[i];
        if (ext_is_compression(e))
            de_model_ir_warn(g->ir,
                             "%s is used but not required: the uncompressed data is imported", e);
        else if (!ext_supported(e))
            de_model_ir_warn(g->ir, "extension %s is not supported and is ignored", e);
    }
    return DE_OK;
}

/* ---- Accessors ------------------------------------------------------------------------------- */

static u64 comp_size(cgltf_component_type t) {
    return (u64)cgltf_component_size(t);
}

/* Bytes of one element as stored (glTF matrix columns of 1- and 2-byte components are padded to
 * 4 bytes). */
static u64 elem_size(const cgltf_accessor *a) {
    return (u64)cgltf_calc_size(a->type, a->component_type);
}

static bool range_ok(u64 size, u64 off, u64 len) {
    return off <= size && len <= size - off;
}

/* The bytes of a buffer view, checked against its buffer, or NULL. */
static const u8 *view_bytes(const cgltf_buffer_view *bv, u64 *size) {
    if (bv == NULL || bv->buffer == NULL || bv->buffer->data == NULL)
        return NULL;
    if (!range_ok((u64)bv->buffer->size, (u64)bv->offset, (u64)bv->size))
        return NULL;
    *size = (u64)bv->size;
    return (const u8 *)bv->buffer->data + bv->offset;
}

static de_result check_accessor(const gltf_ctx *g, const cgltf_accessor *a, u64 index) {
    const char *path = g->src->path;
    if (a->component_type == cgltf_component_type_invalid || a->type == cgltf_type_invalid ||
        elem_size(a) == 0)
        return DE_FAIL(DE_ERR_PARSE, "%s: accessor %llu: bad component type or type", path,
                       (unsigned long long)index);
    if (a->count == 0 || a->count > GLTF_MAX_COUNT)
        return DE_FAIL(DE_ERR_PARSE, "%s: accessor %llu: count %llu out of range", path,
                       (unsigned long long)index, (unsigned long long)a->count);
    u64 es = elem_size(a);
    if (a->buffer_view != NULL) {
        u64 bsize = 0;
        if (view_bytes(a->buffer_view, &bsize) == NULL)
            return DE_FAIL(DE_ERR_PARSE,
                           "%s: accessor %llu: its buffer view lies outside its buffer, or the "
                           "buffer has no data",
                           path, (unsigned long long)index);
        u64 stride = (u64)a->stride;
        if (stride < es || stride > 252u + es)
            return DE_FAIL(DE_ERR_PARSE, "%s: accessor %llu: bad byte stride %llu", path,
                           (unsigned long long)index, (unsigned long long)stride);
        if (!range_ok(bsize, (u64)a->offset, stride * (a->count - 1u) + es))
            return DE_FAIL(DE_ERR_PARSE, "%s: accessor %llu: data outside its buffer view", path,
                           (unsigned long long)index);
        /* cgltf_validate reads index data through typed pointers: keep them aligned. */
        u64 cs = comp_size(a->component_type);
        if (((u64)a->buffer_view->offset + (u64)a->offset) % cs != 0 || stride % cs != 0 ||
            (uintptr_t)a->buffer_view->buffer->data % 4u != 0)
            return DE_FAIL(DE_ERR_PARSE, "%s: accessor %llu: data is not aligned", path,
                           (unsigned long long)index);
    }
    if (a->is_sparse) {
        const cgltf_accessor_sparse *s = &a->sparse;
        u64 isz = 0, vsz = 0;
        u64 ics = comp_size(s->indices_component_type);
        if (s->count == 0 || s->count > a->count ||
            (s->indices_component_type != cgltf_component_type_r_8u &&
             s->indices_component_type != cgltf_component_type_r_16u &&
             s->indices_component_type != cgltf_component_type_r_32u) ||
            view_bytes(s->indices_buffer_view, &isz) == NULL ||
            view_bytes(s->values_buffer_view, &vsz) == NULL ||
            !range_ok(isz, (u64)s->indices_byte_offset, ics * s->count) ||
            !range_ok(vsz, (u64)s->values_byte_offset, es * s->count) ||
            ((u64)s->indices_buffer_view->offset + (u64)s->indices_byte_offset) % ics != 0 ||
            (uintptr_t)s->indices_buffer_view->buffer->data % 4u != 0)
            return DE_FAIL(DE_ERR_PARSE, "%s: accessor %llu: bad sparse data", path,
                           (unsigned long long)index);
    }
    return DE_OK;
}

static u32 rd_u32le(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* One component as a float (glTF normalisation rules). */
static f32 read_comp(const u8 *p, cgltf_component_type t, bool normalized) {
    switch (t) {
    case cgltf_component_type_r_32f: {
        u32 b = rd_u32le(p);
        f32 f;
        memcpy(&f, &b, 4);
        return f;
    }
    case cgltf_component_type_r_8: {
        f32 v = (f32)(i8)p[0];
        return normalized ? (v / 127.0f < -1.0f ? -1.0f : v / 127.0f) : v;
    }
    case cgltf_component_type_r_8u:
        return normalized ? (f32)p[0] / 255.0f : (f32)p[0];
    case cgltf_component_type_r_16: {
        f32 v = (f32)(i16)(u16)((u32)p[0] | ((u32)p[1] << 8));
        return normalized ? (v / 32767.0f < -1.0f ? -1.0f : v / 32767.0f) : v;
    }
    case cgltf_component_type_r_16u: {
        f32 v = (f32)(u16)((u32)p[0] | ((u32)p[1] << 8));
        return normalized ? v / 65535.0f : v;
    }
    case cgltf_component_type_r_32u:
        return (f32)rd_u32le(p);
    default:
        return 0.0f;
    }
}

static u32 read_comp_u32(const u8 *p, cgltf_component_type t) {
    switch (t) {
    case cgltf_component_type_r_8:
    case cgltf_component_type_r_8u:
        return p[0];
    case cgltf_component_type_r_16:
    case cgltf_component_type_r_16u:
        return (u32)p[0] | ((u32)p[1] << 8);
    case cgltf_component_type_r_32u:
        return rd_u32le(p);
    case cgltf_component_type_r_32f: {
        u32 b = rd_u32le(p);
        f32 f;
        memcpy(&f, &b, 4);
        return f >= 0.0f && f < 4294967296.0f ? (u32)f : 0u;
    }
    default:
        return 0u;
    }
}

/* Byte offset of component `k` inside an element (matrix column padding of the glTF spec). */
static u64 comp_offset(const cgltf_accessor *a, u32 k) {
    u64 cs = comp_size(a->component_type);
    u32 rows = a->type == cgltf_type_mat2 ? 2u : a->type == cgltf_type_mat3 ? 3u : 0u;
    if (rows != 0 && cs < 4u) {
        u64 col_bytes = ((cs * rows + 3u) / 4u) * 4u;
        return (k / rows) * col_bytes + (k % rows) * cs;
    }
    return cs * k;
}

/* Decodes an element at `p` into `out[0 .. n)` (n = the type's component count). */
static void read_elem(const cgltf_accessor *a, const u8 *p, f32 *out) {
    u32 n = (u32)cgltf_num_components(a->type);
    for (u32 k = 0; k < n; ++k)
        out[k] = read_comp(p + comp_offset(a, k), a->component_type, a->normalized != 0);
}

static void read_elem_u32(const cgltf_accessor *a, const u8 *p, u32 *out) {
    u32 n = (u32)cgltf_num_components(a->type);
    for (u32 k = 0; k < n; ++k)
        out[k] = read_comp_u32(p + comp_offset(a, k), a->component_type);
}

/* The sparse index of entry `j`. */
static u32 sparse_index(const cgltf_accessor *a, u64 j) {
    const cgltf_accessor_sparse *s = &a->sparse;
    u64 n = 0;
    const u8 *ib = view_bytes(s->indices_buffer_view, &n) + s->indices_byte_offset;
    return read_comp_u32(ib + comp_size(s->indices_component_type) * j, s->indices_component_type);
}

/*
 * All elements of a checked accessor as floats, `out_comps` per element: the accessor's
 * components first, then `fill` for the rest (a VEC3 colour read as 4 gets alpha `fill`).
 */
static f32 *read_floats(gltf_ctx *g, const cgltf_accessor *a, u32 out_comps, f32 fill) {
    u32 n = (u32)cgltf_num_components(a->type);
    if (n > 16u)
        n = 16u;
    f32 *out = (f32 *)de_model_ir_array(g->ir, (u64)a->count * out_comps, sizeof(f32));
    if (out == NULL)
        return NULL;
    f32 tmp[16];
    u64 bsize = 0;
    const u8 *base = a->buffer_view != NULL ? view_bytes(a->buffer_view, &bsize) : NULL;
    for (u64 i = 0; i < a->count; ++i) {
        memset(tmp, 0, sizeof tmp);
        if (base != NULL)
            read_elem(a, base + a->offset + (u64)a->stride * i, tmp);
        for (u32 k = 0; k < out_comps; ++k)
            out[i * out_comps + k] = k < n ? tmp[k] : fill;
    }
    if (a->is_sparse) {
        u64 vsize = 0;
        const u8 *vb =
            view_bytes(a->sparse.values_buffer_view, &vsize) + a->sparse.values_byte_offset;
        for (u64 j = 0; j < a->sparse.count; ++j) {
            u32 at = sparse_index(a, j);
            if (at >= a->count) {
                (void)DE_FAIL(DE_ERR_PARSE, "%s: sparse index %u out of range (count %llu)",
                              g->src->path, at, (unsigned long long)a->count);
                return NULL;
            }
            memset(tmp, 0, sizeof tmp);
            read_elem(a, vb + elem_size(a) * j, tmp); /* sparse values are tightly packed */
            for (u32 k = 0; k < out_comps && k < n; ++k)
                out[(u64)at * out_comps + k] = tmp[k];
        }
    }
    return out;
}

/* All elements as u32 (scalar indices, JOINTS), `out_comps` per element, missing ones 0. */
static u32 *read_uints(gltf_ctx *g, const cgltf_accessor *a, u32 out_comps) {
    u32 n = (u32)cgltf_num_components(a->type);
    if (n > 4u)
        n = 4u;
    u32 *out = (u32 *)de_model_ir_array(g->ir, (u64)a->count * out_comps, sizeof(u32));
    if (out == NULL)
        return NULL;
    u32 tmp[16];
    u64 bsize = 0;
    const u8 *base = a->buffer_view != NULL ? view_bytes(a->buffer_view, &bsize) : NULL;
    for (u64 i = 0; i < a->count; ++i) {
        memset(tmp, 0, sizeof tmp);
        if (base != NULL)
            read_elem_u32(a, base + a->offset + (u64)a->stride * i, tmp);
        for (u32 k = 0; k < out_comps; ++k)
            out[i * out_comps + k] = k < n ? tmp[k] : 0u;
    }
    if (a->is_sparse) {
        u64 vsize = 0;
        const u8 *vb =
            view_bytes(a->sparse.values_buffer_view, &vsize) + a->sparse.values_byte_offset;
        for (u64 j = 0; j < a->sparse.count; ++j) {
            u32 at = sparse_index(a, j);
            if (at >= a->count) {
                (void)DE_FAIL(DE_ERR_PARSE, "%s: sparse index %u out of range (count %llu)",
                              g->src->path, at, (unsigned long long)a->count);
                return NULL;
            }
            memset(tmp, 0, sizeof tmp);
            read_elem_u32(a, vb + elem_size(a) * j, tmp);
            for (u32 k = 0; k < out_comps && k < n; ++k)
                out[(u64)at * out_comps + k] = tmp[k];
        }
    }
    return out;
}

/* ---- Images and textures --------------------------------------------------------------------- */

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

/* Decodes the base64 payload of a data URI (after the comma). */
static de_result decode_base64(gltf_ctx *g, const char *s, const void **data, u64 *size) {
    size_t len = strlen(s);
    u8 *out = (u8 *)de_model_ir_alloc(g->ir, (u64)len / 4u * 3u + 3u);
    if (out == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    u32 acc = 0, bits = 0;
    u64 n = 0;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] == '=')
            break;
        int v = b64_value(s[i]);
        if (v < 0)
            return DE_FAIL(DE_ERR_PARSE, "%s: bad base64 in a data URI", g->src->path);
        acc = (acc << 6) | (u32)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (u8)(acc >> bits);
        }
    }
    *data = out;
    *size = n;
    return DE_OK;
}

static de_result image_bytes(gltf_ctx *g, const cgltf_image *img, const void **data, u64 *size) {
    if (img->buffer_view != NULL) {
        u64 n = 0;
        const u8 *p = view_bytes(img->buffer_view, &n);
        if (p == NULL)
            return DE_FAIL(DE_ERR_PARSE, "%s: image \"%s\": its buffer view has no data",
                           g->src->path, img->name != NULL ? img->name : "");
        /* Copied: buffers decoded from data URIs are freed with the cgltf document. */
        u8 *copy = (u8 *)de_model_ir_alloc(g->ir, n);
        if (copy == NULL)
            return DE_ERR_OUT_OF_MEMORY;
        memcpy(copy, p, (size_t)n);
        *data = copy;
        *size = n;
        return DE_OK;
    }
    if (img->uri == NULL)
        return DE_FAIL(DE_ERR_PARSE, "%s: image \"%s\" has neither a URI nor a buffer view",
                       g->src->path, img->name != NULL ? img->name : "");
    if (strncmp(img->uri, "data:", 5) == 0) {
        const char *comma = strchr(img->uri, ',');
        if (comma == NULL || comma - img->uri < 7 || strncmp(comma - 7, ";base64", 7) != 0)
            return DE_FAIL(DE_ERR_PARSE, "%s: image data URI is not base64", g->src->path);
        return decode_base64(g, comma + 1, data, size);
    }
    if (strstr(img->uri, "://") != NULL)
        return DE_FAIL(DE_ERR_UNSUPPORTED,
                       "%s: image URI \"%s\": only relative paths and data "
                       "URIs are supported",
                       g->src->path, img->uri);
    char *path = (char *)de_model_ir_strdup(g->ir, img->uri);
    if (path == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    cgltf_decode_uri(path);
    if (g->src->read_file == NULL)
        return DE_FAIL(DE_ERR_NOT_FOUND, "%s: image \"%s\": no file reader", g->src->path, path);
    return g->src->read_file(g->src->read_user, path, data, size);
}

/* The IR texture for (image, usage), created on first use. -1 with the error context set. */
static i32 texture_for(gltf_ctx *g, const cgltf_texture *tex, de_model_tex_usage usage,
                       de_result *err) {
    const cgltf_image *img = tex->image;
    if (img == NULL && tex->has_basisu)
        img = tex->basisu_image;
    if (img == NULL) {
        de_model_ir_warn(g->ir, "texture \"%s\" has no image the importer can read; ignored",
                         tex->name != NULL ? tex->name : "");
        return -1;
    }
    u32 image = (u32)cgltf_image_index(g->data, img);
    de_model_ir *ir = g->ir;
    for (u32 i = 0; i < ir->texture_count; ++i)
        if (ir->textures[i].image == image && ir->textures[i].usage == usage)
            return (i32)i;
    de_model_texture *t = &ir->textures[ir->texture_count];
    const void *bytes = NULL;
    u64 size = 0;
    de_result r = image_bytes(g, img, &bytes, &size);
    if (r != DE_OK) {
        *err = r;
        return -1;
    }
    t->name = de_model_ir_strdup(ir, img->name);
    t->mime = de_model_ir_strdup(ir, img->mime_type);
    if (t->name == NULL || t->mime == NULL) {
        *err = DE_ERR_OUT_OF_MEMORY;
        return -1;
    }
    t->image = image;
    t->usage = usage;
    t->data = bytes;
    t->size = size;
    return (i32)ir->texture_count++;
}

/* ---- Materials ------------------------------------------------------------------------------- */

static uv_xform xform_of(const cgltf_texture_view *v) {
    uv_xform x = {{0.0f, 0.0f}, 0.0f, {1.0f, 1.0f}};
    if (v->has_transform) {
        x.offset[0] = v->transform.offset[0];
        x.offset[1] = v->transform.offset[1];
        x.rotation = v->transform.rotation;
        x.scale[0] = v->transform.scale[0];
        x.scale[1] = v->transform.scale[1];
    }
    return x;
}

static bool xform_identity(const uv_xform *x) {
    return x->offset[0] == 0.0f && x->offset[1] == 0.0f && x->rotation == 0.0f &&
           x->scale[0] == 1.0f && x->scale[1] == 1.0f;
}

static bool xform_equal(const uv_xform *a, const uv_xform *b) {
    return memcmp(a, b, sizeof *a) == 0;
}

/* Sets slot `slot` of material `m` from a texture view; records its transform per UV set. */
static de_result set_slot(gltf_ctx *g, de_model_material *m, u32 mat_index, u32 slot,
                          const cgltf_texture_view *v, bool seen[GLTF_MAX_UV_SETS],
                          bool conflict[GLTF_MAX_UV_SETS]) {
    if (v->texture == NULL)
        return DE_OK;
    i32 set = v->has_transform && v->transform.has_texcoord ? v->transform.texcoord : v->texcoord;
    if (set < 0 || (u32)set >= GLTF_MAX_UV_SETS) {
        de_model_ir_warn(g->ir,
                         "material \"%s\": a texture uses UV set %d; only sets 0 and 1 are "
                         "imported, so the texture is dropped",
                         m->name, (int)set);
        return DE_OK;
    }
    de_result err = DE_OK;
    i32 t = texture_for(g, v->texture, de_model_tex_usage_of_slot(slot), &err);
    if (err != DE_OK)
        return err;
    if (t < 0)
        return DE_OK;
    m->tex[slot].texture = t;
    m->tex[slot].texcoord = (u32)set;
    uv_bake *bk = &g->bakes[mat_index];
    uv_xform x = xform_of(v);
    if (!seen[set]) {
        seen[set] = true;
        bk->xf[set] = x;
    } else if (!xform_equal(&bk->xf[set], &x)) {
        conflict[set] = true;
    }
    return DE_OK;
}

static de_result import_material(gltf_ctx *g, u32 index) {
    const cgltf_material *src = &g->data->materials[index];
    de_model_material *m = &g->ir->materials[index];
    m->name = de_model_ir_strdup(g->ir, src->name);
    if (m->name == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    for (u32 s = 0; s < DE_COOKED_TEX_COUNT; ++s)
        m->tex[s].texture = -1;
    de_cooked_material_params *p = &m->params;
    *p = de_cooked_material_params_default();
    const cgltf_pbr_metallic_roughness *pbr = &src->pbr_metallic_roughness;
    for (u32 k = 0; k < 4; ++k)
        p->base_color[k] = pbr->base_color_factor[k];
    p->metallic = pbr->metallic_factor;
    p->roughness = pbr->roughness_factor;
    p->normal_scale = src->normal_texture.texture != NULL ? src->normal_texture.scale : 1.0f;
    p->occlusion_strength =
        src->occlusion_texture.texture != NULL ? src->occlusion_texture.scale : 1.0f;
    for (u32 k = 0; k < 3; ++k)
        p->emissive[k] = src->emissive_factor[k];
    p->alpha_cutoff = src->alpha_cutoff;
    m->alpha_mode = src->alpha_mode == cgltf_alpha_mode_mask    ? DE_COOKED_ALPHA_MASK
                    : src->alpha_mode == cgltf_alpha_mode_blend ? DE_COOKED_ALPHA_BLEND
                                                                : DE_COOKED_ALPHA_OPAQUE;
    if (src->double_sided)
        m->flags |= DE_COOKED_MAT_DOUBLE_SIDED;
    if (src->unlit)
        m->flags |= DE_COOKED_MAT_UNLIT;
    if (src->has_emissive_strength) {
        m->flags |= DE_COOKED_MAT_EMISSIVE_STRENGTH;
        p->emissive_strength = src->emissive_strength.emissive_strength;
    }
    if (src->has_ior) {
        m->flags |= DE_COOKED_MAT_IOR;
        p->ior = src->ior.ior;
    }
    if (src->has_specular) {
        m->flags |= DE_COOKED_MAT_SPECULAR;
        p->specular = src->specular.specular_factor;
        for (u32 k = 0; k < 3; ++k)
            p->specular_color[k] = src->specular.specular_color_factor[k];
    }
    if (src->has_clearcoat) {
        m->flags |= DE_COOKED_MAT_CLEARCOAT;
        p->clearcoat = src->clearcoat.clearcoat_factor;
        p->clearcoat_roughness = src->clearcoat.clearcoat_roughness_factor;
        if (src->clearcoat.clearcoat_normal_texture.texture != NULL)
            p->clearcoat_normal_scale = src->clearcoat.clearcoat_normal_texture.scale;
    }
    if (src->has_sheen) {
        m->flags |= DE_COOKED_MAT_SHEEN;
        for (u32 k = 0; k < 3; ++k)
            p->sheen_color[k] = src->sheen.sheen_color_factor[k];
        p->sheen_roughness = src->sheen.sheen_roughness_factor;
    }
    if (src->has_transmission) {
        m->flags |= DE_COOKED_MAT_TRANSMISSION;
        p->transmission = src->transmission.transmission_factor;
    }
    if (src->has_volume) {
        m->flags |= DE_COOKED_MAT_VOLUME;
        p->thickness = src->volume.thickness_factor;
        for (u32 k = 0; k < 3; ++k)
            p->attenuation_color[k] = src->volume.attenuation_color[k];
        /* cgltf uses FLT_MAX for "absent"; the spec's default is +infinity. */
        p->attenuation_distance = src->volume.attenuation_distance >= 3.0e38f
                                      ? INFINITY
                                      : src->volume.attenuation_distance;
    }
    if (src->has_diffuse_transmission) {
        m->flags |= DE_COOKED_MAT_DIFFUSE_TRANSMISSION;
        p->diffuse_transmission = src->diffuse_transmission.diffuse_transmission_factor;
        for (u32 k = 0; k < 3; ++k)
            p->diffuse_transmission_color[k] =
                src->diffuse_transmission.diffuse_transmission_color_factor[k];
    }
    if (src->has_anisotropy) {
        m->flags |= DE_COOKED_MAT_ANISOTROPY;
        p->anisotropy = src->anisotropy.anisotropy_strength;
        p->anisotropy_rotation = src->anisotropy.anisotropy_rotation;
    }
    if (src->has_iridescence) {
        m->flags |= DE_COOKED_MAT_IRIDESCENCE;
        p->iridescence = src->iridescence.iridescence_factor;
        p->iridescence_ior = src->iridescence.iridescence_ior;
        p->iridescence_thickness_min = src->iridescence.iridescence_thickness_min;
        p->iridescence_thickness_max = src->iridescence.iridescence_thickness_max;
    }
    if (src->has_pbr_specular_glossiness)
        de_model_ir_warn(g->ir,
                         "material \"%s\": KHR_materials_pbrSpecularGlossiness is not supported; "
                         "the metallic-roughness values are used",
                         m->name);

    /* Texture slots, in slot order (so textures are created in a fixed order). */
    const struct {
        u32 slot;
        const cgltf_texture_view *view;
        bool on;
    } slots[] = {
        {DE_COOKED_TEX_BASE_COLOR, &pbr->base_color_texture, true},
        {DE_COOKED_TEX_METAL_ROUGH, &pbr->metallic_roughness_texture, true},
        {DE_COOKED_TEX_NORMAL, &src->normal_texture, true},
        {DE_COOKED_TEX_OCCLUSION, &src->occlusion_texture, true},
        {DE_COOKED_TEX_EMISSIVE, &src->emissive_texture, true},
        {DE_COOKED_TEX_CLEARCOAT, &src->clearcoat.clearcoat_texture, src->has_clearcoat != 0},
        {DE_COOKED_TEX_CLEARCOAT_ROUGHNESS, &src->clearcoat.clearcoat_roughness_texture,
         src->has_clearcoat != 0},
        {DE_COOKED_TEX_CLEARCOAT_NORMAL, &src->clearcoat.clearcoat_normal_texture,
         src->has_clearcoat != 0},
        {DE_COOKED_TEX_SHEEN_COLOR, &src->sheen.sheen_color_texture, src->has_sheen != 0},
        {DE_COOKED_TEX_SHEEN_ROUGHNESS, &src->sheen.sheen_roughness_texture, src->has_sheen != 0},
        {DE_COOKED_TEX_SPECULAR, &src->specular.specular_texture, src->has_specular != 0},
        {DE_COOKED_TEX_SPECULAR_COLOR, &src->specular.specular_color_texture,
         src->has_specular != 0},
        {DE_COOKED_TEX_TRANSMISSION, &src->transmission.transmission_texture,
         src->has_transmission != 0},
        {DE_COOKED_TEX_THICKNESS, &src->volume.thickness_texture, src->has_volume != 0},
        {DE_COOKED_TEX_DIFFUSE_TRANSMISSION,
         &src->diffuse_transmission.diffuse_transmission_texture,
         src->has_diffuse_transmission != 0},
        {DE_COOKED_TEX_DIFFUSE_TRANSMISSION_COLOR,
         &src->diffuse_transmission.diffuse_transmission_color_texture,
         src->has_diffuse_transmission != 0},
        {DE_COOKED_TEX_ANISOTROPY, &src->anisotropy.anisotropy_texture, src->has_anisotropy != 0},
        {DE_COOKED_TEX_IRIDESCENCE, &src->iridescence.iridescence_texture,
         src->has_iridescence != 0},
        {DE_COOKED_TEX_IRIDESCENCE_THICKNESS, &src->iridescence.iridescence_thickness_texture,
         src->has_iridescence != 0},
    };
    bool seen[GLTF_MAX_UV_SETS] = {false, false};
    bool conflict[GLTF_MAX_UV_SETS] = {false, false};
    for (size_t i = 0; i < sizeof slots / sizeof slots[0]; ++i)
        if (slots[i].on)
            DE_TRY(set_slot(g, m, index, slots[i].slot, slots[i].view, seen, conflict));
    uv_bake *bk = &g->bakes[index];
    for (u32 set = 0; set < GLTF_MAX_UV_SETS; ++set) {
        if (!seen[set])
            continue;
        if (conflict[set]) {
            de_model_ir_warn(g->ir,
                             "material \"%s\": textures on UV set %u have different "
                             "KHR_texture_transform values; the transforms are not applied",
                             m->name, set);
            continue;
        }
        bk->apply[set] = !xform_identity(&bk->xf[set]);
    }
    return DE_OK;
}

/* ---- Meshes ---------------------------------------------------------------------------------- */

/* One decoded primitive, before the primitives of a mesh are merged. */
typedef struct prim_data {
    u32 vcount, icount, mask;
    i32 material;
    f32 *attr[DE_MODEL_ATTR_COUNT]; /* JOINTS0 is in `joints` */
    u32 *joints;                    /* 4 per vertex */
    u32 *idx;                       /* local indices */
} prim_data;

static const u32 k_attr_comps[DE_MODEL_ATTR_COUNT] = {3, 3, 4, 2, 2, 4, 4, 4};

static const cgltf_accessor *find_attr(const cgltf_primitive *p, cgltf_attribute_type t, int set) {
    for (cgltf_size i = 0; i < p->attributes_count; ++i)
        if (p->attributes[i].type == t && p->attributes[i].index == set)
            return p->attributes[i].data;
    return NULL;
}

/* Triangle list from a list, strip or fan; degenerate triangles of strips and fans are dropped. */
static u32 *triangulate(gltf_ctx *g, cgltf_primitive_type type, const u32 *in, u32 n, u32 *out_n,
                        const char *mesh_name) {
    u32 tris = 0;
    if (type == cgltf_primitive_type_triangles) {
        if (n % 3u != 0)
            de_model_ir_warn(g->ir,
                             "mesh \"%s\": %u indices are not a whole number of triangles; "
                             "the last %u are dropped",
                             mesh_name, n, n % 3u);
        tris = n / 3u;
    } else {
        tris = n >= 3u ? n - 2u : 0u;
    }
    u32 *out = (u32 *)de_model_ir_array(g->ir, (u64)tris * 3u + 1u, sizeof(u32));
    if (out == NULL)
        return NULL;
    u32 k = 0;
    for (u32 t = 0; t < tris; ++t) {
        u32 a, b, c;
        if (type == cgltf_primitive_type_triangles) {
            a = in[3 * t];
            b = in[3 * t + 1];
            c = in[3 * t + 2];
        } else if (type == cgltf_primitive_type_triangle_strip) {
            /* glTF: p_i = {v_i, v_(i+1+i%2), v_(i+2-i%2)} keeps the winding. */
            a = in[t];
            b = in[t + 1u + (t % 2u)];
            c = in[t + 2u - (t % 2u)];
        } else { /* fan: {v_(i+1), v_(i+2), v_0} */
            a = in[t + 1u];
            b = in[t + 2u];
            c = in[0];
        }
        if (type != cgltf_primitive_type_triangles && (a == b || b == c || a == c))
            continue;
        out[k++] = a;
        out[k++] = b;
        out[k++] = c;
    }
    *out_n = k;
    return out;
}

static void apply_uv_xform(f32 *uv, u32 count, const uv_xform *x) {
    f64 c = cos((f64)x->rotation), s = sin((f64)x->rotation);
    for (u32 i = 0; i < count; ++i) {
        f64 u = (f64)uv[2 * i] * x->scale[0], v = (f64)uv[2 * i + 1] * x->scale[1];
        uv[2 * i] = (f32)(c * u + s * v + x->offset[0]);
        uv[2 * i + 1] = (f32)(-s * u + c * v + x->offset[1]);
    }
}

static de_result decode_primitive(gltf_ctx *g, const cgltf_mesh *mesh, u32 pi, prim_data *out,
                                  bool *skip) {
    const cgltf_primitive *p = &mesh->primitives[pi];
    const char *mname = mesh->name != NULL ? mesh->name : "";
    memset(out, 0, sizeof *out);
    *skip = false;
    if (p->type != cgltf_primitive_type_triangles &&
        p->type != cgltf_primitive_type_triangle_strip &&
        p->type != cgltf_primitive_type_triangle_fan) {
        de_model_ir_warn(g->ir, "mesh \"%s\" primitive %u: points and lines are not imported",
                         mname, pi);
        *skip = true;
        return DE_OK;
    }
    const cgltf_accessor *pos = find_attr(p, cgltf_attribute_type_position, 0);
    if (pos == NULL || cgltf_num_components(pos->type) != 3) {
        de_model_ir_warn(g->ir, "mesh \"%s\" primitive %u has no VEC3 POSITION; skipped", mname,
                         pi);
        *skip = true;
        return DE_OK;
    }
    out->vcount = (u32)pos->count;
    out->material = p->material != NULL ? (i32)cgltf_material_index(g->data, p->material) : -1;
    if (p->targets_count != 0 && !g->warned_targets) {
        g->warned_targets = true;
        de_model_ir_warn(g->ir, "mesh \"%s\": morph targets are not imported", mname);
    }
    struct {
        cgltf_attribute_type t;
        int set;
        u32 attr;
        f32 fill;
    } want[] = {
        {cgltf_attribute_type_position, 0, DE_MODEL_ATTR_POSITION, 0.0f},
        {cgltf_attribute_type_normal, 0, DE_MODEL_ATTR_NORMAL, 0.0f},
        {cgltf_attribute_type_tangent, 0, DE_MODEL_ATTR_TANGENT, 1.0f},
        {cgltf_attribute_type_texcoord, 0, DE_MODEL_ATTR_UV0, 0.0f},
        {cgltf_attribute_type_texcoord, 1, DE_MODEL_ATTR_UV1, 0.0f},
        {cgltf_attribute_type_color, 0, DE_MODEL_ATTR_COLOR0, 1.0f},
        {cgltf_attribute_type_weights, 0, DE_MODEL_ATTR_WEIGHTS0, 0.0f},
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; ++i) {
        const cgltf_accessor *a = find_attr(p, want[i].t, want[i].set);
        if (a == NULL)
            continue;
        if (a->count != pos->count)
            return DE_FAIL(DE_ERR_PARSE, "%s: mesh \"%s\" primitive %u: attribute counts differ",
                           g->src->path, mname, pi);
        out->attr[want[i].attr] = read_floats(g, a, k_attr_comps[want[i].attr], want[i].fill);
        if (out->attr[want[i].attr] == NULL)
            return de_error_last() != NULL ? de_error_last()->result : DE_ERR_OUT_OF_MEMORY;
        out->mask |= DE_MODEL_ATTR_BIT(want[i].attr);
    }
    const cgltf_accessor *j = find_attr(p, cgltf_attribute_type_joints, 0);
    if (j != NULL) {
        if (j->count != pos->count)
            return DE_FAIL(DE_ERR_PARSE, "%s: mesh \"%s\" primitive %u: attribute counts differ",
                           g->src->path, mname, pi);
        out->joints = read_uints(g, j, 4);
        if (out->joints == NULL)
            return de_error_last() != NULL ? de_error_last()->result : DE_ERR_OUT_OF_MEMORY;
        for (u64 i = 0; i < (u64)out->vcount * 4u; ++i)
            if (out->joints[i] > 0xFFFFu)
                return DE_FAIL(DE_ERR_PARSE, "%s: mesh \"%s\": joint index %u out of range",
                               g->src->path, mname, out->joints[i]);
        out->mask |= DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_JOINTS0);
    }
    for (cgltf_size i = 0; i < p->attributes_count && !g->warned_attrs; ++i) {
        const cgltf_attribute *a = &p->attributes[i];
        bool known =
            (a->type == cgltf_attribute_type_texcoord && a->index < 2) ||
            ((a->type == cgltf_attribute_type_position || a->type == cgltf_attribute_type_normal ||
              a->type == cgltf_attribute_type_tangent || a->type == cgltf_attribute_type_color ||
              a->type == cgltf_attribute_type_joints || a->type == cgltf_attribute_type_weights) &&
             a->index == 0);
        if (!known) {
            g->warned_attrs = true;
            de_model_ir_warn(g->ir, "mesh \"%s\": attribute %s is not imported", mname,
                             a->name != NULL ? a->name : "?");
        }
    }

    /* Indices: the accessor's, or 0 .. V-1. */
    u32 n = 0;
    u32 *raw = NULL;
    if (p->indices != NULL) {
        if (cgltf_num_components(p->indices->type) != 1)
            return DE_FAIL(DE_ERR_PARSE, "%s: mesh \"%s\": indices are not scalars", g->src->path,
                           mname);
        raw = read_uints(g, p->indices, 1);
        if (raw == NULL)
            return de_error_last() != NULL ? de_error_last()->result : DE_ERR_OUT_OF_MEMORY;
        n = (u32)p->indices->count;
    } else {
        raw = (u32 *)de_model_ir_array(g->ir, out->vcount, sizeof(u32));
        if (raw == NULL)
            return DE_ERR_OUT_OF_MEMORY;
        for (u32 i = 0; i < out->vcount; ++i)
            raw[i] = i;
        n = out->vcount;
    }
    for (u32 i = 0; i < n; ++i)
        if (raw[i] >= out->vcount)
            return DE_FAIL(DE_ERR_PARSE,
                           "%s: mesh \"%s\" primitive %u: index %u out of range "
                           "(%u vertices)",
                           g->src->path, mname, pi, raw[i], out->vcount);
    out->idx = triangulate(g, p->type, raw, n, &out->icount, mname);
    if (out->idx == NULL)
        return DE_ERR_OUT_OF_MEMORY;

    /* KHR_texture_transform, baked per UV set (see import_material). */
    const de_model_material *mat = out->material >= 0 ? &g->ir->materials[out->material] : NULL;
    if (mat != NULL) {
        const uv_bake *bk = &g->bakes[out->material];
        for (u32 set = 0; set < GLTF_MAX_UV_SETS; ++set)
            if (bk->apply[set] && out->attr[DE_MODEL_ATTR_UV0 + set] != NULL)
                apply_uv_xform(out->attr[DE_MODEL_ATTR_UV0 + set], out->vcount, &bk->xf[set]);
    }

    /* Normals, then tangents for normal maps. */
    if (out->attr[DE_MODEL_ATTR_NORMAL] == NULL) {
        out->attr[DE_MODEL_ATTR_NORMAL] =
            (f32 *)de_model_ir_array(g->ir, (u64)out->vcount * 3u, sizeof(f32));
        if (out->attr[DE_MODEL_ATTR_NORMAL] == NULL)
            return DE_ERR_OUT_OF_MEMORY;
        de_model_generate_normals(out->attr[DE_MODEL_ATTR_POSITION],
                                  out->attr[DE_MODEL_ATTR_NORMAL], 0, out->vcount, out->idx,
                                  out->icount);
        out->mask |= DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_NORMAL);
    } else {
        /* Source normals may be unnormalised (quantized data): normalise them. */
        f32 *nr = out->attr[DE_MODEL_ATTR_NORMAL];
        for (u32 i = 0; i < out->vcount; ++i) {
            f64 l = sqrt((f64)nr[3 * i] * nr[3 * i] + (f64)nr[3 * i + 1] * nr[3 * i + 1] +
                         (f64)nr[3 * i + 2] * nr[3 * i + 2]);
            if (l > 1e-20)
                for (u32 k = 0; k < 3; ++k)
                    nr[3 * i + k] = (f32)((f64)nr[3 * i + k] / l);
        }
    }
    if (mat != NULL && out->attr[DE_MODEL_ATTR_TANGENT] == NULL) {
        i32 set = -1;
        if (mat->tex[DE_COOKED_TEX_NORMAL].texture >= 0)
            set = (i32)mat->tex[DE_COOKED_TEX_NORMAL].texcoord;
        else if (mat->tex[DE_COOKED_TEX_CLEARCOAT_NORMAL].texture >= 0)
            set = (i32)mat->tex[DE_COOKED_TEX_CLEARCOAT_NORMAL].texcoord;
        if (set >= 0) {
            const f32 *uv = out->attr[DE_MODEL_ATTR_UV0 + set];
            if (uv == NULL) {
                de_model_ir_warn(g->ir,
                                 "mesh \"%s\" primitive %u: the normal map uses UV set %d, which "
                                 "the primitive does not have; no tangents",
                                 mname, pi, (int)set);
            } else {
                f32 *tan = (f32 *)de_model_ir_array(g->ir, (u64)out->vcount * 4u, sizeof(f32));
                if (tan == NULL)
                    return DE_ERR_OUT_OF_MEMORY;
                for (u32 i = 0; i < out->vcount; ++i) {
                    tan[4 * i] = 1.0f;
                    tan[4 * i + 3] = 1.0f;
                }
                if (!de_model_generate_tangents(out->attr[DE_MODEL_ATTR_POSITION],
                                                out->attr[DE_MODEL_ATTR_NORMAL], uv, tan, out->idx,
                                                out->icount))
                    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "%s: MikkTSpace failed (out of memory)",
                                   g->src->path);
                out->attr[DE_MODEL_ATTR_TANGENT] = tan;
                out->mask |= DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_TANGENT);
            }
        }
    }
    return DE_OK;
}

static de_result import_mesh(gltf_ctx *g, u32 index) {
    const cgltf_mesh *src = &g->data->meshes[index];
    de_model_ir *ir = g->ir;
    de_model_mesh *m = &ir->meshes[index];
    m->name = de_model_ir_strdup(ir, src->name);
    if (m->name == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    m->morph_target_count = src->primitives_count != 0 ? (u32)src->primitives[0].targets_count : 0;
    prim_data *prims =
        (prim_data *)de_model_ir_array(ir, src->primitives_count + 1u, sizeof(prim_data));
    if (prims == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    u32 kept = 0;
    u64 vtotal = 0, itotal = 0;
    for (u32 pi = 0; pi < (u32)src->primitives_count; ++pi) {
        bool skip = false;
        DE_TRY(decode_primitive(g, src, pi, &prims[kept], &skip));
        if (skip)
            continue;
        vtotal += prims[kept].vcount;
        itotal += prims[kept].icount;
        m->attr_mask |= prims[kept].mask;
        ++kept;
    }
    if (vtotal > GLTF_MAX_COUNT || itotal > (u64)GLTF_MAX_COUNT * 3u)
        return DE_FAIL(DE_ERR_PARSE, "%s: mesh \"%s\" is too large", g->src->path, m->name);
    m->vertex_count = (u32)vtotal;
    m->index_count = (u32)itotal;
    m->submesh_count = kept;
    if (kept == 0) {
        de_model_ir_warn(ir, "mesh \"%s\" has no triangles", m->name);
        m->attr_mask = 0;
        return DE_OK;
    }
    m->submeshes = (de_model_submesh *)de_model_ir_array(ir, kept, sizeof(de_model_submesh));
    m->indices = (u32 *)de_model_ir_array(ir, itotal + 1u, sizeof(u32));
    if (m->submeshes == NULL || m->indices == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    f32 **dst[DE_MODEL_ATTR_COUNT] = {&m->position, &m->normal, &m->tangent, &m->uv[0],
                                      &m->uv[1],    &m->color0, NULL,        &m->weights0};
    for (u32 a = 0; a < DE_MODEL_ATTR_COUNT; ++a) {
        if (!(m->attr_mask & DE_MODEL_ATTR_BIT(a)))
            continue;
        if (a == DE_MODEL_ATTR_JOINTS0) {
            m->joints0 = (u16 *)de_model_ir_array(ir, vtotal * 4u, sizeof(u16));
            if (m->joints0 == NULL)
                return DE_ERR_OUT_OF_MEMORY;
            continue;
        }
        *dst[a] = (f32 *)de_model_ir_array(ir, vtotal * k_attr_comps[a], sizeof(f32));
        if (*dst[a] == NULL)
            return DE_ERR_OUT_OF_MEMORY;
    }
    u32 vbase = 0, ibase = 0;
    for (u32 k = 0; k < kept; ++k) {
        const prim_data *p = &prims[k];
        for (u32 a = 0; a < DE_MODEL_ATTR_COUNT; ++a) {
            if (!(m->attr_mask & DE_MODEL_ATTR_BIT(a)) || a == DE_MODEL_ATTR_JOINTS0)
                continue;
            u32 c = k_attr_comps[a];
            f32 *d = *dst[a] + (u64)vbase * c;
            if (p->attr[a] != NULL) {
                memcpy(d, p->attr[a], sizeof(f32) * c * p->vcount);
            } else if (a == DE_MODEL_ATTR_COLOR0 || a == DE_MODEL_ATTR_TANGENT) {
                for (u32 i = 0; i < p->vcount; ++i) /* white; tangent (1, 0, 0, +1) */
                    for (u32 q = 0; q < c; ++q)
                        d[i * c + q] = a == DE_MODEL_ATTR_COLOR0 ? 1.0f
                                       : (q == 0 || q == 3)      ? 1.0f
                                                                 : 0.0f;
            }
        }
        if (m->joints0 != NULL && p->joints != NULL)
            for (u64 i = 0; i < (u64)p->vcount * 4u; ++i)
                m->joints0[(u64)vbase * 4u + i] = (u16)p->joints[i];
        for (u32 i = 0; i < p->icount; ++i)
            m->indices[ibase + i] = vbase + p->idx[i];
        de_model_submesh *sm = &m->submeshes[k];
        sm->index_first = ibase;
        sm->index_count = p->icount;
        sm->vertex_first = vbase;
        sm->vertex_count = p->vcount;
        sm->material = p->material;
        sm->attr_mask = p->mask;
        vbase += p->vcount;
        ibase += p->icount;
    }
    return DE_OK;
}

/* ---- Nodes, skins, animations ---------------------------------------------------------------- */

static de_result import_nodes(gltf_ctx *g) {
    const cgltf_data *d = g->data;
    de_model_ir *ir = g->ir;
    for (u32 i = 0; i < ir->node_count; ++i) {
        const cgltf_node *s = &d->nodes[i];
        de_model_node *n = &ir->nodes[i];
        n->name = de_model_ir_strdup(ir, s->name);
        if (n->name == NULL)
            return DE_ERR_OUT_OF_MEMORY;
        n->parent = s->parent != NULL ? (i32)cgltf_node_index(d, s->parent) : -1;
        n->mesh = s->mesh != NULL ? (i32)cgltf_mesh_index(d, s->mesh) : -1;
        n->skin = s->skin != NULL ? (i32)cgltf_skin_index(d, s->skin) : -1;
        if (s->has_matrix) {
            if (!de_model_decompose_matrix(s->matrix, n->translation, n->rotation, n->scale))
                de_model_ir_warn(ir,
                                 "node \"%s\": its matrix has shear or a projection; imported as "
                                 "the nearest translation, rotation and scale",
                                 n->name);
        } else {
            for (u32 k = 0; k < 3; ++k) {
                n->translation[k] = s->has_translation ? s->translation[k] : 0.0f;
                n->scale[k] = s->has_scale ? s->scale[k] : 1.0f;
            }
            for (u32 k = 0; k < 4; ++k)
                n->rotation[k] = s->has_rotation ? s->rotation[k] : (k == 3 ? 1.0f : 0.0f);
            f64 l =
                sqrt((f64)n->rotation[0] * n->rotation[0] + (f64)n->rotation[1] * n->rotation[1] +
                     (f64)n->rotation[2] * n->rotation[2] + (f64)n->rotation[3] * n->rotation[3]);
            if (!(l > 1e-12)) {
                de_model_ir_warn(ir, "node \"%s\": zero rotation quaternion; identity used",
                                 n->name);
                n->rotation[0] = n->rotation[1] = n->rotation[2] = 0.0f;
                n->rotation[3] = 1.0f;
            } else if (fabs(l - 1.0) > 1e-7) {
                for (u32 k = 0; k < 4; ++k)
                    n->rotation[k] = (f32)((f64)n->rotation[k] / l);
            }
        }
    }
    /* The scene: the default one, else the first, else every node without a parent. */
    const cgltf_scene *scene = d->scene != NULL       ? d->scene
                               : d->scenes_count != 0 ? &d->scenes[0]
                                                      : NULL;
    if (d->scenes_count > 1)
        de_model_ir_warn(ir, "the file has %u scenes; only \"%s\" is imported as the prefab",
                         (unsigned)d->scenes_count,
                         scene->name != NULL ? scene->name : "(unnamed)");
    ir->roots = (u32 *)de_model_ir_array(ir, ir->node_count + 1u, sizeof(u32));
    if (ir->roots == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    if (scene != NULL) {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
            u32 n = (u32)cgltf_node_index(d, scene->nodes[i]);
            bool dup = false;
            for (u32 k = 0; k < ir->root_count && !dup; ++k)
                dup = ir->roots[k] == n;
            if (!dup)
                ir->roots[ir->root_count++] = n;
        }
    } else {
        for (u32 i = 0; i < ir->node_count; ++i)
            if (ir->nodes[i].parent < 0)
                ir->roots[ir->root_count++] = i;
    }
    for (u32 i = 0; i < ir->node_count; ++i) {
        u32 top = i, steps = 0;
        while (ir->nodes[top].parent >= 0 && steps++ <= ir->node_count)
            top = (u32)ir->nodes[top].parent;
        for (u32 k = 0; k < ir->root_count; ++k)
            if (ir->roots[k] == top)
                ir->nodes[i].in_scene = true;
    }
    return DE_OK;
}

static de_result import_skins(gltf_ctx *g) {
    const cgltf_data *d = g->data;
    de_model_ir *ir = g->ir;
    for (u32 i = 0; i < ir->skin_count; ++i) {
        const cgltf_skin *s = &d->skins[i];
        de_model_skin *k = &ir->skins[i];
        k->name = de_model_ir_strdup(ir, s->name);
        k->joint_count = (u32)s->joints_count;
        k->joints = (u32 *)de_model_ir_array(ir, s->joints_count + 1u, sizeof(u32));
        k->inverse_bind = (f32 *)de_model_ir_array(ir, (u64)s->joints_count * 16u + 1u, 4);
        if (k->name == NULL || k->joints == NULL || k->inverse_bind == NULL)
            return DE_ERR_OUT_OF_MEMORY;
        for (u32 j = 0; j < k->joint_count; ++j) {
            k->joints[j] = (u32)cgltf_node_index(d, s->joints[j]);
            for (u32 e = 0; e < 4; ++e)
                k->inverse_bind[16u * j + 5u * e] = 1.0f;
        }
        k->skeleton_root = s->skeleton != NULL ? (i32)cgltf_node_index(d, s->skeleton) : -1;
        if (s->inverse_bind_matrices != NULL) {
            const cgltf_accessor *a = s->inverse_bind_matrices;
            if (a->type != cgltf_type_mat4 || a->component_type != cgltf_component_type_r_32f ||
                a->count < s->joints_count)
                return DE_FAIL(DE_ERR_PARSE, "%s: skin \"%s\": bad inverse bind matrices",
                               g->src->path, k->name);
            f32 *m = read_floats(g, a, 16, 0.0f);
            if (m == NULL)
                return de_error_last() != NULL ? de_error_last()->result : DE_ERR_OUT_OF_MEMORY;
            memcpy(k->inverse_bind, m, sizeof(f32) * 16u * k->joint_count);
        }
    }
    return DE_OK;
}

static de_result import_anims(gltf_ctx *g) {
    const cgltf_data *d = g->data;
    de_model_ir *ir = g->ir;
    for (u32 i = 0; i < ir->anim_count; ++i) {
        const cgltf_animation *s = &d->animations[i];
        de_model_anim *an = &ir->anims[i];
        an->name = de_model_ir_strdup(ir, s->name);
        an->channels = (de_model_anim_channel *)de_model_ir_array(ir, s->channels_count + 1u,
                                                                  sizeof(de_model_anim_channel));
        if (an->name == NULL || an->channels == NULL)
            return DE_ERR_OUT_OF_MEMORY;
        for (cgltf_size c = 0; c < s->channels_count; ++c) {
            const cgltf_animation_channel *sc = &s->channels[c];
            if (sc->target_node == NULL || sc->sampler == NULL)
                continue;
            de_model_anim_channel *ch = &an->channels[an->channel_count];
            ch->node = (u32)cgltf_node_index(d, sc->target_node);
            u32 comps = 0;
            switch (sc->target_path) {
            case cgltf_animation_path_type_translation:
                ch->path = DE_MODEL_ANIM_TRANSLATION;
                comps = 3;
                break;
            case cgltf_animation_path_type_rotation:
                ch->path = DE_MODEL_ANIM_ROTATION;
                comps = 4;
                break;
            case cgltf_animation_path_type_scale:
                ch->path = DE_MODEL_ANIM_SCALE;
                comps = 3;
                break;
            case cgltf_animation_path_type_weights:
                ch->path = DE_MODEL_ANIM_WEIGHTS;
                comps = sc->target_node->mesh != NULL && sc->target_node->mesh->primitives_count
                            ? (u32)sc->target_node->mesh->primitives[0].targets_count
                            : 0u;
                break;
            default:
                continue;
            }
            if (comps == 0)
                continue;
            ch->interp = sc->sampler->interpolation == cgltf_interpolation_type_step
                             ? DE_MODEL_INTERP_STEP
                         : sc->sampler->interpolation == cgltf_interpolation_type_cubic_spline
                             ? DE_MODEL_INTERP_CUBIC
                             : DE_MODEL_INTERP_LINEAR;
            const cgltf_accessor *in = sc->sampler->input, *outa = sc->sampler->output;
            u32 per_key = ch->interp == DE_MODEL_INTERP_CUBIC ? 3u : 1u;
            if (in->type != cgltf_type_scalar ||
                (u64)outa->count * cgltf_num_components(outa->type) !=
                    (u64)in->count * per_key * comps)
                return DE_FAIL(DE_ERR_PARSE, "%s: animation \"%s\": sampler counts do not match",
                               g->src->path, an->name);
            ch->key_count = (u32)in->count;
            ch->components = comps;
            ch->times = read_floats(g, in, 1, 0.0f);
            ch->values = read_floats(g, outa, (u32)cgltf_num_components(outa->type), 0.0f);
            if (ch->times == NULL || ch->values == NULL)
                return de_error_last() != NULL ? de_error_last()->result : DE_ERR_OUT_OF_MEMORY;
            for (u32 k = 0; k < ch->key_count; ++k)
                if (ch->times[k] > an->duration)
                    an->duration = ch->times[k];
            ++an->channel_count;
        }
    }
    return DE_OK;
}

/* ---- Entry ----------------------------------------------------------------------------------- */

static de_result load(gltf_ctx *g) {
    de_model_ir *ir = g->ir;
    cgltf_data *d = g->data;
    DE_TRY(check_extensions(g));
    for (cgltf_size i = 0; i < d->buffers_count; ++i)
        if (d->buffers[i].data == NULL && d->buffers[i].size != 0)
            return DE_FAIL(DE_ERR_PARSE, "%s: buffer %u has no data", g->src->path, (unsigned)i);
    for (cgltf_size i = 0; i < d->accessors_count; ++i)
        DE_TRY(check_accessor(g, &d->accessors[i], i));
    cgltf_result cr = cgltf_validate(d);
    if (cr != cgltf_result_success)
        return cg_error(g, cr, "validation");

    ir->material_count = (u32)d->materials_count;
    ir->mesh_count = (u32)d->meshes_count;
    ir->node_count = (u32)d->nodes_count;
    ir->skin_count = (u32)d->skins_count;
    ir->anim_count = (u32)d->animations_count;
    ir->materials = (de_model_material *)de_model_ir_array(ir, d->materials_count + 1u,
                                                           sizeof(de_model_material));
    /* At most one texture per (image, usage). */
    ir->textures = (de_model_texture *)de_model_ir_array(ir, d->images_count * 3u + 1u,
                                                         sizeof(de_model_texture));
    ir->meshes =
        (de_model_mesh *)de_model_ir_array(ir, d->meshes_count + 1u, sizeof(de_model_mesh));
    ir->nodes = (de_model_node *)de_model_ir_array(ir, d->nodes_count + 1u, sizeof(de_model_node));
    ir->skins = (de_model_skin *)de_model_ir_array(ir, d->skins_count + 1u, sizeof(de_model_skin));
    ir->anims =
        (de_model_anim *)de_model_ir_array(ir, d->animations_count + 1u, sizeof(de_model_anim));
    g->bakes = (uv_bake *)de_model_ir_array(ir, d->materials_count + 1u, sizeof(uv_bake));
    if (ir->materials == NULL || ir->textures == NULL || ir->meshes == NULL || ir->nodes == NULL ||
        ir->skins == NULL || ir->anims == NULL || g->bakes == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    for (u32 i = 0; i < ir->material_count; ++i)
        DE_TRY(import_material(g, i));
    for (u32 i = 0; i < ir->mesh_count; ++i)
        DE_TRY(import_mesh(g, i));
    DE_TRY(import_nodes(g));
    DE_TRY(import_skins(g));
    DE_TRY(import_anims(g));
    return DE_OK;
}

de_result de_model_gltf_load(const de_model_source *src, de_model_ir *ir) {
    if (src == NULL || ir == NULL || (src->data == NULL && src->size != 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_model_gltf_load: NULL argument");
    gltf_ctx g;
    memset(&g, 0, sizeof g);
    g.src = src;
    g.ir = ir;
    cgltf_options opt;
    memset(&opt, 0, sizeof opt);
    opt.memory.alloc_func = cg_alloc;
    opt.memory.free_func = cg_free;
    opt.memory.user_data = ir;
    opt.file.read = cg_read;
    opt.file.release = cg_release;
    opt.file.user_data = &g;
    cgltf_result cr = cgltf_parse(&opt, src->data, (cgltf_size)src->size, &g.data);
    if (cr != cgltf_result_success)
        return cg_error(&g, cr, "parse");
    /* "" as the glTF path: buffer URIs reach the reader as they are (relative to the source). */
    cr = cgltf_load_buffers(&opt, g.data, "");
    de_result r = cr == cgltf_result_success ? load(&g) : cg_error(&g, cr, "loading buffers");
    cgltf_free(g.data);
    return r;
}
