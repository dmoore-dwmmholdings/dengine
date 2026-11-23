/* editor_terrain.c: terrain.info, terrain.sample, terrain.exportHeightmap and the terrain.* edit
 * ops (DENG-580; DIDE protocol 1.11, capability world.terrain).
 *
 * A terrain is an entity with DeTerrain (a render component). The IDE reads it with the three
 * methods and changes it with four edit ops inside an edit transaction, so undo and redo work as
 * for every other edit:
 *
 *   - terrain.stroke: `value` a TerrainStroke (a sculpt, paint or hole brush stroke);
 *   - terrain.import: `path` a heightmap file, `value` {format, tile} (both optional);
 *   - terrain.bake: writes the enabled auto-material rules into the splat map;
 *   - terrain.restore: `value` the tiles another op changed, as they were.
 *
 * The inverse of each is a terrain.restore that holds only the tiles the op changed (and the
 * rules, when they changed). The math is the deterministic CPU model in src/world/terrain; this
 * file reads the protocol, the heightmap files and the undo data.
 *
 * Heightmap formats: png16 (16-bit greyscale PNG), raw16 (square 16-bit little-endian samples) and
 * exr (32-bit float metres, first channel). png16 and raw16 hold 0..65535 for 0..height. exr is in
 * Debug and Dev builds only (tinyexr); terrain.info lists the formats the build has. Paths are
 * file system paths, as the IDE sends them.
 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* fopen, snprintf */
#endif
#include "host/editor/editor_internal.h"

#include "world/terrain/terrain.h"
#include <dengine/de_render.h>
#include <stb_image.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
#define DE_ED_TERRAIN_EXR 1
#include <tinyexr.h>
#else
#define DE_ED_TERRAIN_EXR 0
#endif

#define MAX_FILE  (64u * 1024u * 1024u) /* bytes of a heightmap file */
#define MAX_SIDE  4097u                  /* samples on a heightmap side */

static de_world *world_of(de__editor *ed) {
    return ed->e != NULL ? ed->e->world : NULL;
}

/* The DeTerrain component of the world, registered on first use (the world may be replaced). */
static de_component terrain_id(de_world *w) {
    if (w == NULL)
        return (de_component){0};
    de_component c = de_component_find(w, DE_TERRAIN_COMPONENT);
    if (!DE_HANDLE_VALID(c) && de_render_register_components(w) == DE_OK)
        c = de_component_find(w, DE_TERRAIN_COMPONENT);
    return c;
}

static const DeTerrain *terrain_get(de__editor *ed, u64 bits) {
    de_world *w = world_of(ed);
    const de_component c = terrain_id(w);
    const de_entity e = {bits};
    if (!DE_HANDLE_VALID(c) || !de_entity_alive(w, e))
        return NULL;
    return (const DeTerrain *)de_entity_get_const(w, e, c);
}

/* The terrain of `bits`, or answers the request with an error and returns NULL. */
static const DeTerrain *terrain_of(de__editor *ed, mpack_writer_t *w, u64 id, u64 bits,
                                   const char *method) {
    const DeTerrain *t = terrain_get(ed, bits);
    if (t == NULL) {
        de__ed_error(w, id, DE_EP_ERR_NOT_FOUND, "%s: entity %llu has no " DE_TERRAIN_COMPONENT,
                     method, (unsigned long long)bits);
        return NULL;
    }
    if (!de__terrain_valid(t)) {
        de__ed_error(w, id, DE_EP_ERR_INVALID_PARAMS, "%s: the terrain grid is not %ux%u", method,
                     DE_TERRAIN_RES, DE_TERRAIN_RES);
        return NULL;
    }
    return t;
}

static void origin_of(de__editor *ed, u64 bits, f64 out[3]) {
    const de_transform x = de__ed_world_xf(world_of(ed), (de_entity){bits});
    out[0] = (f64)x.position.x;
    out[1] = (f64)x.position.y;
    out[2] = (f64)x.position.z;
}

/* ---------- Formats ---------- */

static const char *const k_formats[] = {"png16", "raw16", "exr"};
#define FORMAT_COUNT (DE_ED_TERRAIN_EXR ? 3u : 2u)

static bool ends_with(de_ep_str s, const char *suffix) {
    const size_t n = strlen(suffix);
    if (s.len < n)
        return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s.ptr[s.len - n + i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != suffix[i])
            return false;
    }
    return true;
}

/* The format a path's extension names: .exr, .r16 / .raw, else png16. */
static const char *format_of(de_ep_str path) {
    if (ends_with(path, ".exr"))
        return "exr";
    if (ends_with(path, ".r16") || ends_with(path, ".raw"))
        return "raw16";
    return "png16";
}

/* The format named `s` (NULL when the build has no such format). */
static const char *format_named(de_ep_str s) {
    for (u32 i = 0; i < FORMAT_COUNT; ++i)
        if (de_ep_str_eq(s, k_formats[i]))
            return k_formats[i];
    return NULL;
}

static bool copy_path(de_ep_str s, char *out, size_t cap) {
    if (s.len == 0 || s.len >= cap || memchr(s.ptr, '\0', s.len) != NULL)
        return false;
    memcpy(out, s.ptr, s.len);
    out[s.len] = '\0';
    return true;
}

static u8 *read_file(const char *path, size_t *size, char *err, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        snprintf(err, cap, "%s: could not open the file", path);
        return NULL;
    }
    u8 *bytes = NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        n = ftell(f);
    if (n < 0 || (unsigned long)n > MAX_FILE || fseek(f, 0, SEEK_SET) != 0) {
        snprintf(err, cap, "%s: the file is unreadable or larger than %u MB", path,
                 MAX_FILE / (1024u * 1024u));
    } else {
        bytes = (u8 *)malloc((size_t)n + 1u);
        if (bytes == NULL || fread(bytes, 1, (size_t)n, f) != (size_t)n) {
            snprintf(err, cap, "%s: could not read the file", path);
            free(bytes);
            bytes = NULL;
        } else {
            *size = (size_t)n;
        }
    }
    fclose(f);
    return bytes;
}

static bool write_file(const char *path, const void *bytes, size_t size, char *err, size_t cap) {
    FILE *f = fopen(path, "wb");
    bool ok = f != NULL && fwrite(bytes, 1, size, f) == size;
    if (f != NULL && fclose(f) != 0)
        ok = false;
    if (!ok)
        snprintf(err, cap, "%s: could not write the file", path);
    return ok;
}

/* -- png16: a 16-bit greyscale PNG with stored DEFLATE blocks; read with stb_image -- */

static void be32(u8 *out, u32 v) {
    out[0] = (u8)(v >> 24);
    out[1] = (u8)(v >> 16);
    out[2] = (u8)(v >> 8);
    out[3] = (u8)v;
}

static u32 crc32_of(const u8 *p, size_t n) {
    u32 crc = ~0u;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (u32 b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* Writes a chunk at `out` (length, type, data, crc); returns the end. */
static u8 *png_chunk(u8 *out, const char type[4], const u8 *data, u32 n) {
    be32(out, n);
    memcpy(out + 4, type, 4);
    if (n != 0)
        memcpy(out + 8, data, n);
    be32(out + 8 + n, crc32_of(out + 4, (size_t)n + 4u));
    return out + 12 + n;
}

static bool png16_write(const char *path, u32 w, u32 h, const u16 *v, char *err, size_t cap) {
    const size_t row = (size_t)w * 2u + 1u, raw = row * h;
    const size_t blocks = (raw + 65534u) / 65535u;
    const size_t zlen = 2u + raw + blocks * 5u + 4u;
    u8 *z = (u8 *)malloc(zlen);
    u8 *png = (u8 *)malloc(8u + 25u + 12u + zlen + 12u);
    bool ok = false;
    if (z != NULL && png != NULL) {
        u8 *o = z;
        *o++ = 0x78;
        *o++ = 0x01;
        u32 a = 1, b = 0;
        size_t at = 0;
        while (at < raw) {
            const u32 n = (u32)(raw - at < 65535u ? raw - at : 65535u);
            *o++ = at + n == raw ? 1 : 0;
            *o++ = (u8)n;
            *o++ = (u8)(n >> 8);
            *o++ = (u8)~n;
            *o++ = (u8)(~n >> 8);
            for (u32 i = 0; i < n; ++i, ++at) {
                const size_t y = at / row, c = at % row;
                u8 byte = 0; /* filter: none */
                if (c != 0) {
                    const u16 s = v[y * w + (c - 1u) / 2u];
                    byte = (c - 1u) % 2u == 0 ? (u8)(s >> 8) : (u8)s;
                }
                *o++ = byte;
                a = (a + byte) % 65521u;
                b = (b + a) % 65521u;
            }
        }
        be32(o, (b << 16) | a);
        static const u8 sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
        u8 ihdr[13];
        be32(ihdr, w);
        be32(ihdr + 4, h);
        ihdr[8] = 16; /* bit depth */
        ihdr[9] = 0;  /* greyscale */
        ihdr[10] = ihdr[11] = ihdr[12] = 0;
        memcpy(png, sig, 8);
        u8 *p = png_chunk(png + 8, "IHDR", ihdr, 13);
        p = png_chunk(p, "IDAT", z, (u32)zlen);
        p = png_chunk(p, "IEND", NULL, 0);
        ok = write_file(path, png, (size_t)(p - png), err, cap);
    } else {
        snprintf(err, cap, "out of memory");
    }
    free(z);
    free(png);
    return ok;
}

typedef struct heightmap {
    de_terrain_grid g;
    f32 *v; /* owned */
} heightmap;

static bool grid_alloc(heightmap *hm, u32 w, u32 h, char *err, size_t cap) {
    if (w < 2 || h < 2) {
        snprintf(err, cap, "the heightmap is too small");
        return false;
    }
    if (w > MAX_SIDE || h > MAX_SIDE) {
        snprintf(err, cap, "the heightmap is too large (at most %u x %u)", MAX_SIDE, MAX_SIDE);
        return false;
    }
    hm->v = (f32 *)malloc(sizeof(f32) * (size_t)w * h);
    if (hm->v == NULL) {
        snprintf(err, cap, "out of memory");
        return false;
    }
    hm->g = (de_terrain_grid){w, h, hm->v};
    return true;
}

static bool png16_read(const u8 *bytes, size_t n, heightmap *hm, char *err, size_t cap) {
    static const u8 sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    int w = 0, h = 0, comp = 0;
    if (n < 8 || memcmp(bytes, sig, 8) != 0 || n > (size_t)0x7fffffff ||
        !stbi_info_from_memory(bytes, (int)n, &w, &h, &comp) || comp != 1 ||
        !stbi_is_16_bit_from_memory(bytes, (int)n)) {
        snprintf(err, cap, "a PNG heightmap must be 16-bit greyscale");
        return false;
    }
    if (!grid_alloc(hm, (u32)w, (u32)h, err, cap))
        return false;
    stbi_us *px = stbi_load_16_from_memory(bytes, (int)n, &w, &h, &comp, 1);
    if (px == NULL) {
        snprintf(err, cap, "the PNG heightmap could not be decoded");
        return false;
    }
    for (size_t i = 0; i < (size_t)w * (size_t)h; ++i)
        hm->v[i] = (f32)px[i] / 65535.0f;
    stbi_image_free(px);
    return true;
}

static bool raw16_read(const u8 *bytes, size_t n, heightmap *hm, char *err, size_t cap) {
    const size_t count = n / 2u;
    size_t side = 0;
    while ((side + 1u) * (side + 1u) <= count)
        ++side;
    if (n % 2u != 0 || side * side != count || side < 2) {
        snprintf(err, cap, "a RAW heightmap must be square 16-bit little-endian samples");
        return false;
    }
    if (!grid_alloc(hm, (u32)side, (u32)side, err, cap))
        return false;
    for (size_t i = 0; i < count; ++i)
        hm->v[i] = (f32)(u16)(bytes[2u * i] | (bytes[2u * i + 1u] << 8)) / 65535.0f;
    return true;
}

#if DE_ED_TERRAIN_EXR
static bool exr_fail(const char *what, const char *e, char *err, size_t cap) {
    snprintf(err, cap, "EXR: %s%s%s", what, e != NULL ? ": " : "", e != NULL ? e : "");
    if (e != NULL)
        FreeEXRErrorMessage(e);
    return false;
}

static bool exr_read(const u8 *bytes, size_t n, heightmap *hm, char *err, size_t cap) {
    EXRVersion ver;
    memset(&ver, 0, sizeof ver);
    if (ParseEXRVersionFromMemory(&ver, bytes, n) != TINYEXR_SUCCESS)
        return exr_fail("not an OpenEXR file", NULL, err, cap);
    if (ver.multipart || ver.non_image || ver.tiled)
        return exr_fail("multi-part, deep and tiled files are not supported", NULL, err, cap);
    EXRHeader hdr;
    InitEXRHeader(&hdr);
    const char *e = NULL;
    if (ParseEXRHeaderFromMemory(&hdr, &ver, bytes, n, &e) != TINYEXR_SUCCESS) {
        FreeEXRHeader(&hdr);
        return exr_fail("bad header", e, err, cap);
    }
    const i64 w = (i64)hdr.data_window.max_x - hdr.data_window.min_x + 1;
    const i64 h = (i64)hdr.data_window.max_y - hdr.data_window.min_y + 1;
    bool ok = hdr.num_channels >= 1;
    if (!ok)
        exr_fail("the EXR has no channels", NULL, err, cap);
    else if (w < 2 || h < 2)
        ok = (snprintf(err, cap, "the heightmap is too small"), false);
    else
        ok = grid_alloc(hm, (u32)(w > MAX_SIDE ? MAX_SIDE + 1 : w),
                        (u32)(h > MAX_SIDE ? MAX_SIDE + 1 : h), err, cap);
    if (!ok) {
        FreeEXRHeader(&hdr);
        return false;
    }
    for (int c = 0; c < hdr.num_channels; ++c)
        if (hdr.pixel_types[c] == TINYEXR_PIXELTYPE_HALF)
            hdr.requested_pixel_types[c] = TINYEXR_PIXELTYPE_FLOAT;
    EXRImage img;
    InitEXRImage(&img);
    e = NULL;
    if (LoadEXRImageFromMemory(&img, &hdr, bytes, n, &e) != TINYEXR_SUCCESS || img.images == NULL ||
        img.width != (int)w || img.height != (int)h) {
        FreeEXRImage(&img);
        FreeEXRHeader(&hdr);
        return exr_fail("decoding failed", e, err, cap);
    }
    const size_t count = (size_t)w * (size_t)h;
    if (hdr.requested_pixel_types[0] == TINYEXR_PIXELTYPE_UINT) {
        const u32 *p = (const u32 *)img.images[0];
        for (size_t i = 0; i < count; ++i)
            hm->v[i] = (f32)p[i];
    } else {
        memcpy(hm->v, img.images[0], sizeof(f32) * count);
    }
    FreeEXRImage(&img);
    FreeEXRHeader(&hdr);
    return true;
}

static bool exr_write(const char *path, u32 w, u32 h, const f32 *v, char *err, size_t cap) {
    EXRChannelInfo ch;
    memset(&ch, 0, sizeof ch);
    memcpy(ch.name, "Y", 2);
    int type = TINYEXR_PIXELTYPE_FLOAT;
    EXRHeader hdr;
    InitEXRHeader(&hdr);
    hdr.num_channels = 1;
    hdr.channels = &ch;
    hdr.pixel_types = &type;
    hdr.requested_pixel_types = &type;
    hdr.compression_type = TINYEXR_COMPRESSIONTYPE_NONE;
    unsigned char *planes[1] = {(unsigned char *)(uintptr_t)v};
    EXRImage img;
    InitEXRImage(&img);
    img.num_channels = 1;
    img.width = (int)w;
    img.height = (int)h;
    img.images = planes;
    unsigned char *out = NULL;
    const char *e = NULL;
    const size_t size = SaveEXRImageToMemory(&img, &hdr, &out, &e);
    if (size == 0)
        return exr_fail("encoding failed", e, err, cap);
    const bool ok = write_file(path, out, size, err, cap);
    free(out);
    return ok;
}
#endif

/* Reads a heightmap file. False with a message for a bad file. */
static bool heightmap_read(de_ep_str path, const char *format, heightmap *hm, char *err,
                           size_t cap) {
    memset(hm, 0, sizeof *hm);
    char p[1024];
    if (!copy_path(path, p, sizeof p)) {
        snprintf(err, cap, "the heightmap path is empty or too long");
        return false;
    }
    size_t n = 0;
    u8 *bytes = read_file(p, &n, err, cap);
    if (bytes == NULL)
        return false;
    bool ok = false;
    if (strcmp(format, "raw16") == 0)
        ok = raw16_read(bytes, n, hm, err, cap);
    else if (strcmp(format, "png16") == 0)
        ok = png16_read(bytes, n, hm, err, cap);
#if DE_ED_TERRAIN_EXR
    else if (strcmp(format, "exr") == 0)
        ok = exr_read(bytes, n, hm, err, cap);
#endif
    else
        snprintf(err, cap, "unknown heightmap format `%s`", format);
    free(bytes);
    if (!ok) {
        free(hm->v);
        hm->v = NULL;
    }
    return ok;
}

/* Writes a region of the terrain to a heightmap file. */
static bool heightmap_write(const DeTerrain *t, de_ep_str path, const char *format, i32 tx, i32 ty,
                            char *err, size_t cap) {
    char p[1024];
    if (!copy_path(path, p, sizeof p)) {
        snprintf(err, cap, "the heightmap path is empty or too long");
        return false;
    }
    u32 x0, y0, w, h;
    de__terrain_region(tx, ty, &x0, &y0, &w, &h);
    if (strcmp(format, "raw16") == 0 || strcmp(format, "png16") == 0) {
        static u16 v[DE_TERRAIN_SAMPLES];
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x)
                v[y * w + x] = t->heights[(y0 + y) * DE_TERRAIN_RES + x0 + x];
        if (strcmp(format, "png16") == 0)
            return png16_write(p, w, h, v, err, cap);
        static u8 le[DE_TERRAIN_SAMPLES * 2];
        for (u32 i = 0; i < w * h; ++i) {
            le[2 * i] = (u8)v[i];
            le[2 * i + 1] = (u8)(v[i] >> 8);
        }
        return write_file(p, le, (size_t)w * h * 2u, err, cap);
    }
#if DE_ED_TERRAIN_EXR
    if (strcmp(format, "exr") == 0) {
        static f32 m[DE_TERRAIN_SAMPLES];
        de__terrain_export_metres(t, tx, ty, m);
        return exr_write(p, w, h, m, err, cap);
    }
#endif
    snprintf(err, cap, "unknown heightmap format `%s`", format);
    return false;
}

/* A tile [x, y] (count 2, each below DE_TERRAIN_TILES). */
static bool tile_ok(const u32 *v, u32 count, char *err, size_t cap) {
    if (count != 2) {
        snprintf(err, cap, "tile is [x, y]");
        return false;
    }
    if (v[0] >= DE_TERRAIN_TILES || v[1] >= DE_TERRAIN_TILES) {
        snprintf(err, cap, "tile is out of range (0..%u)", (unsigned)DE_TERRAIN_TILES);
        return false;
    }
    return true;
}

/* ---------- Methods ---------- */

static void terrain_info(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w) {
    DE__ED_PARAMS(terrain_info, de_ep_terrain_params, q);
    const DeTerrain *t = terrain_of(ed, w, req->id, q.entity, "terrain.info");
    if (t == NULL)
        return;
    static de_ep_str tools[DE_TERRAIN_TOOL_COUNT], formats[3];
    for (u32 i = 0; i < DE_TERRAIN_TOOL_COUNT; ++i)
        tools[i] = de_ep_cstr(de__terrain_tools[i]);
    for (u32 i = 0; i < FORMAT_COUNT; ++i)
        formats[i] = de_ep_cstr(k_formats[i]);
    static f64 origin[3];
    static u32 tiles[2];
    static de_ep_terrain_layer layers[DE_TERRAIN_LAYERS];
    origin_of(ed, q.entity, origin);
    tiles[0] = tiles[1] = DE_TERRAIN_TILES;
    u32 n = 0;
    for (u32 i = 0; i < DE_TERRAIN_LAYERS; ++i) {
        const u32 slot = t->order[i];
        if (slot >= DE_TERRAIN_LAYERS)
            continue;
        const de_terrain_layer *l = &t->layers[slot];
        const char *mat =
            DE_HANDLE_VALID(l->material) ? de__ed_asset_path(ed, l->material.id) : NULL;
        layers[n].slot = slot;
        layers[n].name = (de_ep_str){l->name, strnlen(l->name, sizeof l->name)};
        layers[n].material = de_ep_cstr(mat != NULL ? mat : "");
        layers[n].tiling = l->tiling;
        layers[n].sharpness = l->sharpness;
        ++n;
    }
    de_ep_terrain_info r;
    memset(&r, 0, sizeof r);
    r.entity = q.entity;
    r.component = de_ep_cstr(DE_TERRAIN_COMPONENT);
    r.resolution = t->resolution;
    r.size = t->size;
    r.height = t->height;
    r.origin = (de_ep_list_f64){origin, 3};
    r.tile_size = DE_TERRAIN_TILE;
    r.tiles = (de_ep_list_u32){tiles, 2};
    r.layers = (de_ep_list_terrain_layer){layers, n};
    r.max_layers = DE_TERRAIN_LAYERS;
    r.max_rules = DE_TERRAIN_RULES;
    r.tools = (de_ep_list_str){tools, DE_TERRAIN_TOOL_COUNT};
    r.formats = (de_ep_list_str){formats, FORMAT_COUNT};
    de_ep_write_response_terrain_info(w, req->id, &r);
}

static void terrain_sample(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                           mpack_writer_t *w) {
    DE__ED_PARAMS(terrain_sample, de_ep_terrain_sample_params, q);
    const DeTerrain *t = terrain_of(ed, w, req->id, q.entity, "terrain.sample");
    if (t == NULL)
        return;
    f64 o[3];
    origin_of(ed, q.entity, o);
    static de_terrain_sample s;
    if (!de__terrain_sample_at(t, o[0], o[2], q.x, q.z, &s)) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "the point is off the terrain");
        return;
    }
    de_ep_terrain_sample r;
    memset(&r, 0, sizeof r);
    r.height = s.height;
    r.slope = s.slope;
    r.weights = (de_ep_list_f32){s.weights, DE_TERRAIN_LAYERS};
    r.baked = (de_ep_list_f32){s.baked, DE_TERRAIN_LAYERS};
    r.hole = s.hole;
    de_ep_write_response_terrain_sample(w, req->id, &r);
}

static void terrain_export(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                           mpack_writer_t *w) {
    DE__ED_PARAMS(terrain_export_heightmap, de_ep_heightmap_export_params, q);
    const DeTerrain *t = terrain_of(ed, w, req->id, q.entity, "terrain.exportHeightmap");
    if (t == NULL)
        return;
    char err[256];
    const de_ep_str asked = q.format.len != 0 ? q.format : de_ep_cstr(format_of(q.path));
    const char *format = format_named(asked);
    if (format == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "terrain.exportHeightmap: unknown heightmap format `%.*s`", (int)asked.len,
                     asked.ptr);
        return;
    }
    i32 tx = -1, ty = -1;
    if (q.has_tile) {
        if (!tile_ok(q.tile.items, q.tile.count, err, sizeof err)) {
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "terrain.exportHeightmap: %s", err);
            return;
        }
        tx = (i32)q.tile.items[0];
        ty = (i32)q.tile.items[1];
    }
    if (!heightmap_write(t, q.path, format, tx, ty, err, sizeof err)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "terrain.exportHeightmap: %s", err);
        return;
    }
    const de_ep_path_result r = {q.path};
    de_ep_write_response_terrain_export_heightmap(w, req->id, &r);
}

/* ---------- Edit ops ---------- */

typedef struct op_fail {
    de_ep_error *code;
    char *msg;
    size_t cap;
} op_fail;

static bool op_error(op_fail *f, de_ep_error code, const char *fmt, ...) {
    *f->code = code;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->msg, f->cap, fmt, ap);
    va_end(ap);
    return false;
}

static bool restore_u(mpack_node_t list, u32 i, u32 max, u32 *out) {
    if (i >= mpack_node_array_length(list))
        return false;
    const mpack_node_t n = mpack_node_array_at(list, i);
    const mpack_type_t ty = mpack_node_type(n);
    if (ty != mpack_type_uint && ty != mpack_type_int)
        return false;
    const u64 v = mpack_node_u64(n);
    if (mpack_node_error(n) != mpack_ok || v > max)
        return false;
    *out = (u32)v;
    return true;
}

static f32 rule_f(mpack_node_t m, const char *key) {
    const mpack_node_t n = mpack_node_map_cstr_optional(m, key);
    if (mpack_node_is_missing(n) || mpack_node_is_nil(n))
        return 0.0f;
    return mpack_node_float(n);
}

static bool restore_rules(mpack_node_t list, DeTerrain *t) {
    if (mpack_node_type(list) != mpack_type_array)
        return false;
    const u32 n = (u32)mpack_node_array_length(list);
    if (n > DE_TERRAIN_RULES)
        return false;
    de_terrain_rule rules[DE_TERRAIN_RULES];
    memset(rules, 0, sizeof rules);
    for (u32 i = 0; i < n; ++i) {
        const mpack_node_t m = mpack_node_array_at(list, i);
        if (mpack_node_type(m) != mpack_type_map)
            return false;
        const mpack_node_t en = mpack_node_map_cstr_optional(m, "enabled");
        rules[i].enabled = !mpack_node_is_missing(en) && mpack_node_bool(en);
        const mpack_node_t ly = mpack_node_map_cstr_optional(m, "layer");
        const u64 layer = mpack_node_is_missing(ly) ? 0 : mpack_node_u64(ly);
        if (layer > 255)
            return false;
        rules[i].layer = (u8)layer;
        rules[i].min_slope = rule_f(m, "min_slope");
        rules[i].max_slope = rule_f(m, "max_slope");
        rules[i].min_height = rule_f(m, "min_height");
        rules[i].max_height = rule_f(m, "max_height");
        rules[i].min_curvature = rule_f(m, "min_curvature");
        rules[i].max_curvature = rule_f(m, "max_curvature");
        if (mpack_node_error(m) != mpack_ok)
            return false;
    }
    memcpy(t->rules, rules, sizeof rules);
    return true;
}

/* Applies a terrain.restore value to `t` (a copy: it may be partly changed on failure). */
static bool restore(DeTerrain *t, de_ep_raw value) {
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, value.ptr, value.len);
    mpack_tree_parse(&tree);
    bool ok = mpack_tree_error(&tree) == mpack_ok;
    const mpack_node_t root = ok ? mpack_tree_root(&tree) : (mpack_node_t){0};
    ok = ok && mpack_node_type(root) == mpack_type_map;
    mpack_node_t tiles = {0}, hs = {0}, ss = {0}, ho = {0};
    if (ok) {
        tiles = mpack_node_map_cstr_optional(root, "tiles");
        hs = mpack_node_map_cstr_optional(root, "heights");
        ss = mpack_node_map_cstr_optional(root, "splat");
        ho = mpack_node_map_cstr_optional(root, "holes");
        ok = mpack_node_type(tiles) == mpack_type_array && mpack_node_type(hs) == mpack_type_array &&
             mpack_node_type(ss) == mpack_type_array && mpack_node_type(ho) == mpack_type_array;
    }
    u32 h = 0, s = 0;
    const size_t tile_count = ok ? mpack_node_array_length(tiles) : 0;
    for (size_t k = 0; ok && k < tile_count; ++k) {
        const mpack_node_t xy = mpack_node_array_at(tiles, k);
        u32 tx = 0, ty = 0;
        ok = mpack_node_type(xy) == mpack_type_array && restore_u(xy, 0, DE_TERRAIN_TILES - 1, &tx) &&
             restore_u(xy, 1, DE_TERRAIN_TILES - 1, &ty);
        u16 idx[DE_TERRAIN_TILE * DE_TERRAIN_TILE];
        const u32 n = ok ? de__terrain_tile_indices(tx, ty, idx) : 0;
        for (u32 j = 0; ok && j < n; ++j, ++h) {
            u32 v = 0;
            ok = restore_u(hs, h, 0xffffu, &v);
            t->heights[idx[j]] = (u16)v;
            ok = ok && restore_u(ho, h, 0xffu, &v);
            t->holes[idx[j]] = (u8)v;
            for (u32 c = 0; ok && c < DE_TERRAIN_LAYERS; ++c, ++s) {
                ok = restore_u(ss, s, 0xffu, &v);
                t->splat[idx[j] * DE_TERRAIN_LAYERS + c] = (u8)v;
            }
        }
    }
    if (ok) {
        const mpack_node_t rules = mpack_node_map_cstr_optional(root, "rules");
        if (!mpack_node_is_missing(rules))
            ok = restore_rules(rules, t);
    }
    if (mpack_tree_destroy(&tree) != mpack_ok)
        ok = false;
    return ok;
}

/* The terrain.restore value that turns `after` back into `before`: the changed tiles as `before`
 * has them, and the rules when they changed. Empty when it does not fit the arena. */
static de_ep_raw restore_from(de_ep_arena *a, const DeTerrain *before, const DeTerrain *after) {
    u32 changed[DE_TERRAIN_TILES * DE_TERRAIN_TILES][2];
    u32 nt = 0, ns = 0;
    u16 idx[DE_TERRAIN_TILE * DE_TERRAIN_TILE];
    for (u32 ty = 0; ty < DE_TERRAIN_TILES; ++ty)
        for (u32 tx = 0; tx < DE_TERRAIN_TILES; ++tx)
            if (de__terrain_tile_changed(before, after, tx, ty)) {
                changed[nt][0] = tx;
                changed[nt][1] = ty;
                ++nt;
                ns += de__terrain_tile_indices(tx, ty, idx);
            }
    const bool rules = !de__terrain_rules_equal(before, after);
    mpack_writer_t mw;
    de__ed_raw_begin(a, &mw);
    mpack_start_map(&mw, rules ? 5u : 4u);
    mpack_write_cstr(&mw, "tiles");
    mpack_start_array(&mw, nt);
    for (u32 k = 0; k < nt; ++k) {
        mpack_start_array(&mw, 2);
        mpack_write_u32(&mw, changed[k][0]);
        mpack_write_u32(&mw, changed[k][1]);
        mpack_finish_array(&mw);
    }
    mpack_finish_array(&mw);
    for (u32 field = 0; field < 3; ++field) {
        static const char *const names[3] = {"heights", "splat", "holes"};
        mpack_write_cstr(&mw, names[field]);
        mpack_start_array(&mw, field == 1 ? ns * DE_TERRAIN_LAYERS : ns);
        for (u32 k = 0; k < nt; ++k) {
            const u32 n = de__terrain_tile_indices(changed[k][0], changed[k][1], idx);
            for (u32 j = 0; j < n; ++j) {
                const u32 i = idx[j];
                if (field == 0)
                    mpack_write_u32(&mw, before->heights[i]);
                else if (field == 2)
                    mpack_write_u32(&mw, before->holes[i]);
                else
                    for (u32 c = 0; c < DE_TERRAIN_LAYERS; ++c)
                        mpack_write_u32(&mw, before->splat[i * DE_TERRAIN_LAYERS + c]);
            }
        }
        mpack_finish_array(&mw);
    }
    if (rules) {
        mpack_write_cstr(&mw, "rules");
        mpack_start_array(&mw, DE_TERRAIN_RULES);
        for (u32 k = 0; k < DE_TERRAIN_RULES; ++k) {
            const de_terrain_rule *r = &before->rules[k];
            mpack_start_map(&mw, 8);
            mpack_write_cstr(&mw, "enabled");
            mpack_write_bool(&mw, r->enabled);
            mpack_write_cstr(&mw, "layer");
            mpack_write_u32(&mw, r->layer);
            mpack_write_cstr(&mw, "min_slope");
            mpack_write_float(&mw, r->min_slope);
            mpack_write_cstr(&mw, "max_slope");
            mpack_write_float(&mw, r->max_slope);
            mpack_write_cstr(&mw, "min_height");
            mpack_write_float(&mw, r->min_height);
            mpack_write_cstr(&mw, "max_height");
            mpack_write_float(&mw, r->max_height);
            mpack_write_cstr(&mw, "min_curvature");
            mpack_write_float(&mw, r->min_curvature);
            mpack_write_cstr(&mw, "max_curvature");
            mpack_write_float(&mw, r->max_curvature);
            mpack_finish_map(&mw);
        }
        mpack_finish_array(&mw);
    }
    mpack_finish_map(&mw);
    return de__ed_raw_end(a, &mw);
}

static bool op_stroke(de_ep_arena *a, const de_ep_edit_op *o, DeTerrain *t, const f64 org[3],
                      op_fail *f) {
    if (!o->has_value)
        return op_error(f, DE_EP_ERR_INVALID_PARAMS, "terrain.stroke needs value");
    de_ep_terrain_stroke p;
    memset(&p, 0, sizeof p);
    mpack_reader_t r;
    mpack_reader_init_data(&r, o->value.ptr, o->value.len);
    de_ep_read_terrain_stroke(&r, a, &p);
    if (mpack_reader_destroy(&r) != mpack_ok)
        return op_error(f, DE_EP_ERR_INVALID_PARAMS, "terrain.stroke: bad value");
    de_terrain_stroke s;
    memset(&s, 0, sizeof s);
    s.tool = p.tool.ptr;
    s.tool_len = p.tool.len;
    s.points = p.points.items;
    s.point_count = p.points.count;
    s.radius = p.radius;
    s.strength = p.strength;
    s.falloff = p.falloff.items;
    s.falloff_count = p.falloff.count;
    s.has_target = p.has_target;
    s.target = p.target;
    s.has_step = p.has_step;
    s.step = p.step;
    s.has_seed = p.has_seed;
    s.seed = p.seed;
    s.has_scale = p.has_scale;
    s.scale = p.scale;
    s.has_rotation = p.has_rotation;
    s.rotation = p.rotation;
    s.has_iterations = p.has_iterations;
    s.iterations = p.iterations;
    s.has_layer = p.has_layer;
    s.layer = p.layer;
    s.area_tile = p.has_area && de_ep_str_eq(p.area, "tile");
    s.erase = p.has_erase && p.erase;
    heightmap stamp;
    memset(&stamp, 0, sizeof stamp);
    if (de_ep_str_eq(p.tool, "stamp") && p.has_stamp &&
        !heightmap_read(p.stamp, format_of(p.stamp), &stamp, f->msg, f->cap)) {
        *f->code = DE_EP_ERR_INVALID_PARAMS;
        return false;
    }
    const bool ok = de__terrain_stroke(t, org[0], org[2], &s, stamp.v != NULL ? &stamp.g : NULL,
                                       f->msg, f->cap);
    free(stamp.v);
    if (!ok)
        *f->code = DE_EP_ERR_INVALID_PARAMS;
    return ok;
}

static bool op_import(const de_ep_edit_op *o, DeTerrain *t, op_fail *f) {
    if (!o->has_path)
        return op_error(f, DE_EP_ERR_INVALID_PARAMS, "terrain.import needs path");
    const char *format = format_of(o->path);
    char named[16] = "";
    i32 tx = -1, ty = -1;
    if (o->has_value && o->value.len != 0) {
        mpack_tree_t tree;
        mpack_tree_init_data(&tree, o->value.ptr, o->value.len);
        mpack_tree_parse(&tree);
        bool ok = mpack_tree_error(&tree) == mpack_ok;
        const mpack_node_t root = ok ? mpack_tree_root(&tree) : (mpack_node_t){0};
        if (ok && mpack_node_type(root) == mpack_type_map) {
            const mpack_node_t fm = mpack_node_map_cstr_optional(root, "format");
            if (!mpack_node_is_missing(fm) && mpack_node_type(fm) == mpack_type_str) {
                const size_t n = mpack_node_strlen(fm);
                const de_ep_str s = {mpack_node_str(fm), n};
                format = format_named(s);
                if (format == NULL)
                    snprintf(named, sizeof named, "%.*s", (int)(n < 15 ? n : 15), s.ptr);
            }
            const mpack_node_t tl = mpack_node_map_cstr_optional(root, "tile");
            if (!mpack_node_is_missing(tl) && !mpack_node_is_nil(tl)) {
                u32 v[2] = {0, 0};
                const u32 n = mpack_node_type(tl) == mpack_type_array
                                  ? (u32)mpack_node_array_length(tl)
                                  : 0;
                const bool ints = n == 2 && restore_u(tl, 0, 0xffffffffu, &v[0]) &&
                                  restore_u(tl, 1, 0xffffffffu, &v[1]);
                ok = tile_ok(v, ints ? 2 : 0, f->msg, f->cap);
                if (ok) {
                    tx = (i32)v[0];
                    ty = (i32)v[1];
                }
            }
        } else if (ok && !mpack_node_is_nil(root)) {
            ok = (snprintf(f->msg, f->cap, "terrain.import: value is {format, tile}"), false);
        } else if (!ok) {
            snprintf(f->msg, f->cap, "terrain.import: bad value");
        }
        mpack_tree_destroy(&tree);
        if (!ok) {
            *f->code = DE_EP_ERR_INVALID_PARAMS;
            return false;
        }
    }
    if (format == NULL)
        return op_error(f, DE_EP_ERR_INVALID_PARAMS, "unknown heightmap format `%s`", named);
    if (!de__terrain_valid(t))
        return op_error(f, DE_EP_ERR_INVALID_PARAMS, "the terrain grid is not %ux%u",
                        DE_TERRAIN_RES, DE_TERRAIN_RES);
    heightmap hm;
    if (!heightmap_read(o->path, format, &hm, f->msg, f->cap)) {
        *f->code = DE_EP_ERR_INVALID_PARAMS;
        return false;
    }
    de__terrain_import(t, &hm.g, tx, ty, strcmp(format, "exr") == 0);
    free(hm.v);
    return true;
}

/* The terrain.* edit ops (editor_edit.c calls it for an op that starts with "terrain."). */
static bool terrain_op(de__editor *ed, de_ep_arena *a, const de_ep_edit_op *o, de_ep_edit_op *inv,
                       de_ep_error *code, char *msg, size_t cap) {
    op_fail f = {code, msg, cap};
    if (!o->has_entity)
        return op_error(&f, DE_EP_ERR_INVALID_PARAMS, "%.*s needs entity", (int)o->op.len,
                        o->op.ptr);
    de_world *w = world_of(ed);
    const de_component c = terrain_id(w);
    const de_entity e = {o->entity};
    DeTerrain *live = DE_HANDLE_VALID(c) && de_entity_alive(w, e)
                          ? (DeTerrain *)de_entity_get(w, e, c)
                          : NULL;
    if (live == NULL)
        return op_error(&f, DE_EP_ERR_NOT_FOUND, "%llu has no " DE_TERRAIN_COMPONENT,
                        (unsigned long long)o->entity);
    static DeTerrain after; /* main thread only; 8 KB */
    after = *live;
    f64 org[3];
    origin_of(ed, o->entity, org);
    bool ok;
    if (de_ep_str_eq(o->op, "terrain.stroke"))
        ok = op_stroke(a, o, &after, org, &f);
    else if (de_ep_str_eq(o->op, "terrain.import"))
        ok = op_import(o, &after, &f);
    else if (de_ep_str_eq(o->op, "terrain.bake")) {
        ok = de__terrain_valid(&after) ||
             op_error(&f, DE_EP_ERR_INVALID_PARAMS, "the terrain grid is not %ux%u",
                      DE_TERRAIN_RES, DE_TERRAIN_RES);
        if (ok)
            de__terrain_bake(&after);
    } else if (de_ep_str_eq(o->op, "terrain.restore")) {
        if (!o->has_value)
            return op_error(&f, DE_EP_ERR_INVALID_PARAMS, "terrain.restore needs value");
        ok = (de__terrain_valid(&after) && restore(&after, o->value)) ||
             op_error(&f, DE_EP_ERR_INVALID_PARAMS, "terrain.restore: bad tile data");
    } else {
        return op_error(&f, DE_EP_ERR_INVALID_PARAMS, "unknown op `%.*s`", (int)o->op.len,
                        o->op.ptr);
    }
    if (!ok)
        return false;
    const de_ep_raw undo = restore_from(a, live, &after);
    if (undo.len == 0)
        return op_error(&f, DE_EP_ERR_BUSY, "%.*s: out of scratch memory", (int)o->op.len,
                        o->op.ptr);
    *live = after;
    de_entity_mark_changed(w, e, c);
    memset(inv, 0, sizeof *inv);
    inv->op = de_ep_cstr("terrain.restore");
    inv->has_entity = true;
    inv->entity = o->entity;
    inv->has_value = true;
    inv->value = undo;
    return true;
}

void de__ed_terrain_register(de__editor *ed) {
    (void)terrain_id(world_of(ed)); /* the IDE's reflection lists the component from the start */
    ed->terrain_op = terrain_op;
    de__ed_on(ed, DE_EP_METHOD_TERRAIN_INFO, terrain_info);
    de__ed_on(ed, DE_EP_METHOD_TERRAIN_SAMPLE, terrain_sample);
    de__ed_on(ed, DE_EP_METHOD_TERRAIN_EXPORT_HEIGHTMAP, terrain_export);
    de__ed_add_capability(ed, "world.terrain");
}
