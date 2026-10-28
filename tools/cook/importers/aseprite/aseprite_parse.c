/* Layout reference: https://github.com/aseprite/aseprite/blob/main/docs/ase-file-specs.md
 * Chunk cursors never extend beyond their enclosing frame. Metadata is decoded before
 * resolving links, so forward links work without recursion or repeated decompression. */
#include "aseprite_parse.h"
#include <limits.h>
#include <stb_image.h>
#include <string.h>

#define ASE_BYTES_MAX (256u * 1024u * 1024u)
#define ASE_DEPTH_MAX 128u
typedef struct block {
    struct block *next;
    size_t bytes;
} block;
typedef struct cursor {
    const u8 *at, *end;
    de_aseprite_doc *doc;
} cursor;
typedef struct parser {
    de_aseprite_doc *doc;
    u32 layer_capacity, cel_capacity, tag_capacity, slice_capacity;
    u32 parents[ASE_DEPTH_MAX];
    de_aseprite_user_data *user;
    u32 user_tag, user_tags_end;
    bool modern_palette;
} parser;

static bool fail(de_aseprite_doc *d, const char *why) {
    if (d->result == DE_OK) {
        d->result = DE_ERR_INVALID_ARGUMENT;
        d->why = why;
    }
    return false;
}
static void *allocate(de_aseprite_doc *d, size_t bytes) {
    const size_t header = (sizeof(block) + 15u) & ~(size_t)15u;
    if (bytes > SIZE_MAX - header || bytes + header > d->max_bytes - d->allocated) {
        fail(d, "Aseprite allocation budget exceeded");
        return NULL;
    }
    size_t total = bytes + header;
    block *b = DE_ALLOC(&d->allocator, total, 16, DE_MEM_ASSET);
    if (!b) {
        d->result = DE_ERR_OUT_OF_MEMORY;
        d->why = "Aseprite allocation failed";
        return NULL;
    }
    b->next = d->allocations;
    b->bytes = total;
    d->allocations = b;
    d->allocated += total;
    void *p = (u8 *)b + header;
    memset(p, 0, bytes);
    return p;
}
static void *grow(de_aseprite_doc *d, void *items, u32 count, u32 *capacity, size_t stride) {
    if (count == *capacity) {
        u32 next = *capacity ? *capacity * 2u : 8u;
        if (next < *capacity || (size_t)next > SIZE_MAX / stride) {
            fail(d, "Aseprite metadata count overflow");
            return NULL;
        }
        void *p = allocate(d, (size_t)next * stride);
        if (!p)
            return NULL;
        if (count)
            memcpy(p, items, (size_t)count * stride);
        items = p;
        *capacity = next;
    }
    return items;
}
static const u8 *take(cursor *c, size_t n) {
    if (n > (size_t)(c->end - c->at)) {
        fail(c->doc, "truncated Aseprite chunk");
        return NULL;
    }
    const u8 *p = c->at;
    c->at += n;
    return p;
}
static u8 byte(cursor *c) {
    const u8 *p = take(c, 1);
    return p ? *p : 0;
}
static u16 word(cursor *c) {
    const u8 *p = take(c, 2);
    return p ? (u16)((u16)p[0] | ((u16)p[1] << 8)) : 0;
}
static u32 dword(cursor *c) {
    const u8 *p = take(c, 4);
    return p ? (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24) : 0;
}
static i16 short_word(cursor *c) {
    u16 n = word(c);
    return (i16)((i32)(n & 32767u) - (i32)(n & 32768u));
}
static i32 signed_dword(cursor *c) {
    u32 n = dword(c);
    return n <= INT32_MAX ? (i32)n : -1 - (i32)(UINT32_MAX - n);
}
static bool utf8(const u8 *p, size_t n) {
    size_t at = 0;
    while (at < n) {
        u32 ch = p[at++], minimum = 0;
        u32 more = 0;
        if (ch >= 0xc2 && ch <= 0xdf) {
            more = 1;
            minimum = 0x80;
            ch &= 31;
        } else if (ch >= 0xe0 && ch <= 0xef) {
            more = 2;
            minimum = 0x800;
            ch &= 15;
        } else if (ch >= 0xf0 && ch <= 0xf4) {
            more = 3;
            minimum = 0x10000;
            ch &= 7;
        } else if (!ch || ch >= 0x80)
            return false;
        if (more > n - at)
            return false;
        for (u32 i = 0; i < more; ++i) {
            u32 part = p[at++];
            if ((part & 0xc0) != 0x80)
                return false;
            ch = (ch << 6) | (part & 63);
        }
        if (ch < minimum || ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff))
            return false;
    }
    return true;
}
static de_aseprite_string string(cursor *c) {
    u16 n = word(c);
    const u8 *p = take(c, n);
    if (!p || !utf8(p, n)) {
        fail(c->doc, "invalid Aseprite UTF-8 string");
        return (de_aseprite_string){0};
    }
    return (de_aseprite_string){(const char *)p, n};
}
static bool preflight(de_aseprite_doc *d, const u8 *source, size_t bytes, u16 speed) {
    cursor file = {source + 128, source + bytes, d};
    for (u32 i = 0; i < d->frame_count; ++i) {
        const u8 *start = file.at;
        u32 size = dword(&file);
        if (size < 16 || size - 4u > (size_t)(file.end - file.at))
            return fail(d, "invalid Aseprite frame size");
        cursor frame = {start + 4, start + size, d};
        if (word(&frame) != 0xf1fa)
            return fail(d, "invalid Aseprite frame magic");
        u16 old_count = word(&frame), ms = word(&frame);
        (void)take(&frame, 2);
        u32 count = dword(&frame);
        if (!count)
            count = old_count;
        if (count > (size_t)(frame.end - frame.at) / 6u)
            return fail(d, "invalid Aseprite chunk count");
        d->frames[i].offset = (u32)(frame.at - source);
        d->frames[i].bytes = size - 16u;
        d->frames[i].chunks = count;
        d->frames[i].ms = ms ? ms : speed;
        if (!d->frames[i].ms)
            return fail(d, "Aseprite frame duration is zero");
        for (u32 j = 0; j < count; ++j) {
            u32 chunk_size = dword(&frame);
            u16 kind = word(&frame);
            if (chunk_size < 6 || chunk_size - 6u > (size_t)(frame.end - frame.at))
                return fail(d, "invalid Aseprite chunk size");
            if (kind == 0x2019) {
                d->has_new_palette = true;
                d->frames[i].has_new_palette = true;
            }
            if (!take(&frame, chunk_size - 6u))
                return false;
        }
        if (frame.at != frame.end)
            return fail(d, "Aseprite frame has unaccounted bytes");
        file.at = start + size;
    }
    return file.at == file.end || fail(d, "Aseprite file has unaccounted frames");
}
static bool layer(parser *p, cursor *c, u32 frame) {
    de_aseprite_doc *d = p->doc;
    if (frame || d->layer_count == 65536u)
        return fail(d, "invalid Aseprite layer position/count");
    d->layers = grow(d, d->layers, d->layer_count, &p->layer_capacity, sizeof *d->layers);
    if (!d->layers)
        return false;
    de_aseprite_layer *l = &d->layers[d->layer_count];
    l->flags = word(c);
    l->type = word(c);
    l->depth = word(c);
    (void)take(c, 4);
    l->blend = word(c);
    l->opacity = byte(c);
    (void)take(c, 3);
    l->name = string(c);
    if (l->type > 2 || l->depth >= ASE_DEPTH_MAX)
        return fail(d, "unsupported Aseprite layer type/depth");
    l->parent = l->depth ? p->parents[l->depth - 1u] : UINT32_MAX;
    if (l->depth && (l->parent == UINT32_MAX || d->layers[l->parent].type != 1))
        return fail(d, "Aseprite layer has no parent group");
    p->parents[l->depth] = d->layer_count;
    for (u32 i = l->depth + 1u; i < ASE_DEPTH_MAX; ++i)
        p->parents[i] = UINT32_MAX;
    if (!(d->flags & 1u))
        l->opacity = 255;
    /* Background layers have neither authored opacity nor a blend mode. */
    if ((l->flags & 8u) || (l->type == 1 && !(d->flags & 2u))) {
        l->opacity = 255;
        l->blend = 0;
    }
    if (l->type == 2) {
        (void)dword(c);
        d->has_tilemaps = true;
    }
    if (d->flags & 4u)
        (void)take(c, 16);
    ++d->layer_count;
    p->user = &l->user;
    return d->result == DE_OK;
}
static u32 adler32(const u8 *data, size_t size) {
    u32 a = 1, b = 0;
    for (size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}
static bool cel(parser *p, cursor *c, u32 frame) {
    de_aseprite_doc *d = p->doc;
    d->cels = grow(d, d->cels, d->cel_count, &p->cel_capacity, sizeof *d->cels);
    if (!d->cels)
        return false;
    de_aseprite_cel *v = &d->cels[d->cel_count];
    v->frame = frame;
    v->layer = word(c);
    v->x = short_word(c);
    v->y = short_word(c);
    v->opacity = byte(c);
    v->type = word(c);
    v->z = short_word(c);
    (void)take(c, 5);
    if (v->layer >= d->layer_count || d->layers[v->layer].type == 1 || v->type > 3)
        return fail(d, "invalid Aseprite cel layer/type");
    if (v->type == 1) {
        v->linked_frame = word(c);
        if (v->linked_frame >= d->frame_count)
            return fail(d, "invalid Aseprite linked frame");
    } else {
        v->width = word(c);
        v->height = word(c);
        if (v->type == 3) {
            if (d->layers[v->layer].type != 2)
                return fail(d, "tilemap cel in an image layer");
            (void)take(c, 28);
            d->has_tilemaps = true;
        } else {
            u64 size = (u64)v->width * v->height * (d->depth / 8u);
            if (!v->width || !v->height || size > INT_MAX || size > d->max_bytes)
                return fail(d, "invalid Aseprite cel extent");
            if (d->layers[v->layer].type != 0)
                return fail(d, "image cel in a tilemap layer");
            if (v->type == 0)
                v->pixels = take(c, (size_t)size);
            else {
                size_t compressed = (size_t)(c->end - c->at);
                if (compressed < 6 || compressed > INT_MAX)
                    return fail(d, "invalid Aseprite zlib size");
                u8 *pixels = allocate(d, (size_t)size);
                if (!pixels)
                    return false;
                int decoded = stbi_zlib_decode_buffer((char *)pixels, (int)size,
                                                      (const char *)c->at, (int)compressed);
                const u8 *sum = c->end - 4;
                u32 checksum =
                    ((u32)sum[0] << 24) | ((u32)sum[1] << 16) | ((u32)sum[2] << 8) | sum[3];
                if (decoded != (int)size || adler32(pixels, (size_t)size) != checksum)
                    return fail(d, "invalid Aseprite compressed pixels/checksum");
                v->pixels = pixels;
                c->at = c->end;
            }
        }
    }
    ++d->cel_count;
    p->user = &v->user;
    return d->result == DE_OK;
}
static bool palette(de_aseprite_doc *d, cursor *c, u16 kind, bool modern, u8 colors[256][4]) {
    if (kind == 0x2019) {
        u32 count = dword(c), first = dword(c), last = dword(c);
        (void)take(c, 8);
        if (!count || count > 65536u || first > last || last >= count)
            return fail(d, "invalid Aseprite palette range");
        for (u32 i = first; i <= last && d->result == DE_OK; ++i) {
            u16 flags = word(c);
            const u8 *rgba = take(c, 4);
            if (rgba && i < 256)
                memcpy(colors[i], rgba, 4);
            if (flags & 1u)
                (void)string(c);
        }
    } else if (!modern) {
        u32 packets = word(c), index = 0;
        for (u32 i = 0; i < packets && d->result == DE_OK; ++i) {
            index += byte(c);
            u32 n = byte(c);
            if (!n)
                n = 256;
            if (index > 256 || n > 256u - index)
                return fail(d, "invalid Aseprite legacy palette range");
            for (u32 j = 0; j < n; ++j) {
                for (u32 k = 0; k < 3; ++k) {
                    u32 value = byte(c);
                    if (kind == 0x000b || kind == 0x0011) {
                        if (value > 63)
                            return fail(d, "invalid six-bit Aseprite palette color");
                        value = (value << 2) | (value >> 4);
                    }
                    colors[index + j][k] = (u8)value;
                }
                colors[index + j][3] = 255;
            }
        }
    }
    return d->result == DE_OK;
}
static bool tags(parser *p, cursor *c) {
    de_aseprite_doc *d = p->doc;
    u32 count = word(c);
    (void)take(c, 8);
    p->user_tag = d->tag_count;
    p->user_tags_end = d->tag_count + count;
    for (u32 i = 0; i < count && d->result == DE_OK; ++i) {
        d->tags = grow(d, d->tags, d->tag_count, &p->tag_capacity, sizeof *d->tags);
        if (!d->tags)
            return false;
        de_aseprite_tag *t = &d->tags[d->tag_count];
        t->from = word(c);
        t->to = word(c);
        t->direction = byte(c);
        t->repeats = word(c);
        (void)take(c, 10);
        t->name = string(c);
        if (t->from > t->to || t->to >= d->frame_count || t->direction > 3)
            return fail(d, "invalid Aseprite tag frame range/direction");
        ++d->tag_count;
    }
    return d->result == DE_OK;
}
static bool slice(parser *p, cursor *c) {
    de_aseprite_doc *d = p->doc;
    d->slices = grow(d, d->slices, d->slice_count, &p->slice_capacity, sizeof *d->slices);
    if (!d->slices)
        return false;
    de_aseprite_slice *s = &d->slices[d->slice_count];
    s->key_count = dword(c);
    s->flags = dword(c);
    (void)dword(c);
    s->name = string(c);
    size_t stride = 20u + ((s->flags & 1u) ? 16u : 0u) + ((s->flags & 2u) ? 8u : 0u);
    if (s->key_count > (size_t)(c->end - c->at) / stride)
        return fail(d, "invalid Aseprite slice key count");
    if (s->key_count) {
        s->keys = allocate(d, (size_t)s->key_count * sizeof *s->keys);
        if (!s->keys)
            return false;
    }
    for (u32 i = 0; i < s->key_count && d->result == DE_OK; ++i) {
        de_aseprite_slice_key *k = &s->keys[i];
        k->frame = dword(c);
        k->x = signed_dword(c);
        k->y = signed_dword(c);
        k->width = dword(c);
        k->height = dword(c);
        if (k->frame >= d->frame_count || (i && k->frame <= s->keys[i - 1u].frame))
            return fail(d, "invalid Aseprite slice frame order");
        if (s->flags & 1u) {
            k->center_x = signed_dword(c);
            k->center_y = signed_dword(c);
            k->center_width = dword(c);
            k->center_height = dword(c);
        }
        if (s->flags & 2u) {
            k->pivot_x = signed_dword(c);
            k->pivot_y = signed_dword(c);
        }
    }
    ++d->slice_count;
    p->user = &s->user;
    return d->result == DE_OK;
}
static bool user_data(parser *p, cursor *c) {
    u32 flags = dword(c);
    de_aseprite_user_data *u = p->user;
    if (p->user_tag < p->user_tags_end)
        u = &p->doc->tags[p->user_tag++].user;
    de_aseprite_string text = {0};
    const u8 *color = NULL;
    if (flags & 1u)
        text = string(c);
    if (flags & 2u)
        color = take(c, 4);
    if (flags & 4u) {
        u32 bytes = dword(c);
        if (bytes < 8 || !take(c, bytes - 4u))
            return fail(p->doc, "invalid Aseprite property-map size");
    }
    if (u) {
        u->text = text;
        if (color) {
            memcpy(u->color, color, 4);
            u->has_color = true;
        }
    }
    return p->doc->result == DE_OK;
}
static bool resolve_links(de_aseprite_doc *d) {
    if (!d->layer_count)
        return fail(d, "Aseprite has no layers");
    size_t count = (size_t)d->frame_count * d->layer_count;
    if (count > SIZE_MAX / sizeof(u32))
        return fail(d, "Aseprite cel index overflow");
    u32 *index = allocate(d, count * sizeof *index);
    u32 *path = allocate(d, (size_t)d->frame_count * sizeof *path);
    u8 *visiting = allocate(d, d->cel_count);
    if (!index || !path || !visiting)
        return false;
    for (u32 i = 0; i < d->cel_count; ++i) {
        de_aseprite_cel *v = &d->cels[i];
        u32 *entry = &index[(size_t)v->frame * d->layer_count + v->layer];
        if (*entry)
            return fail(d, "duplicate Aseprite cel for a frame/layer");
        *entry = i + 1u;
    }
    for (u32 i = 0; i < d->cel_count; ++i) {
        de_aseprite_cel *v = &d->cels[i];
        if (v->type != 1 || v->pixels || d->layers[v->layer].type == 2)
            continue;
        u32 at = i, depth = 0;
        while (d->cels[at].type == 1 && !d->cels[at].pixels) {
            if (visiting[at] || depth == d->frame_count)
                return fail(d, "cyclic Aseprite cel link");
            visiting[at] = 1;
            path[depth++] = at;
            de_aseprite_cel *link = &d->cels[at];
            u32 next = index[(size_t)link->linked_frame * d->layer_count + link->layer];
            if (!next)
                return fail(d, "Aseprite cel links to an absent cel");
            at = next - 1u;
        }
        const de_aseprite_cel *target = &d->cels[at];
        if (!target->pixels)
            return fail(d, "Aseprite cel link has no image");
        while (depth) {
            u32 item = path[--depth];
            d->cels[item].pixels = target->pixels;
            d->cels[item].width = target->width;
            d->cels[item].height = target->height;
            visiting[item] = 0;
        }
    }
    return true;
}
static bool chunks(de_aseprite_doc *d, const u8 *source) {
    parser p = {.doc = d};
    for (u32 i = 0; i < ASE_DEPTH_MAX; ++i)
        p.parents[i] = UINT32_MAX;
    u8 colors[256][4] = {{0}};
    for (u32 i = 0; i < 256; ++i)
        colors[i][3] = 255;
    for (u32 i = 0; i < d->frame_count; ++i) {
        de_aseprite_frame *f = &d->frames[i];
        cursor frame = {source + f->offset, source + f->offset + f->bytes, d};
        f->first_cel = d->cel_count;
        p.user = NULL;
        p.user_tag = p.user_tags_end = 0;
        for (u32 j = 0; j < f->chunks; ++j) {
            u32 bytes = dword(&frame);
            u16 type = word(&frame);
            cursor c = {frame.at, frame.at + bytes - 6u, d};
            frame.at = c.end;
            if (type != 0x2020) {
                p.user = NULL;
                p.user_tag = p.user_tags_end = 0;
            }
            bool ok = true;
            switch (type) {
            case 0x2004:
                ok = layer(&p, &c, i);
                break;
            case 0x2005:
                ok = cel(&p, &c, i);
                break;
            case 0x0004:
            case 0x000b: /* Upstream FLI_COLOR is decimal11; retain documented0x0011 too. */
            case 0x0011:
            case 0x2019:
                ok = palette(d, &c, type, p.modern_palette, colors);
                if (type == 0x2019)
                    p.modern_palette = true;
                p.user = &d->user;
                break;
            case 0x2018:
                ok = tags(&p, &c);
                break;
            case 0x2022:
                ok = slice(&p, &c);
                break;
            case 0x2020:
                ok = user_data(&p, &c);
                break;
            default:
                break; /* Unknown/unsupported chunks stay inside their preflight bounds. */
            }
            if (!ok || d->result != DE_OK)
                return false;
        }
        f->cel_count = d->cel_count - f->first_cel;
        memcpy(f->palette, colors, sizeof colors);
    }
    return resolve_links(d);
}
void de_aseprite_free(de_aseprite_doc *d) {
    if (!d)
        return;
    block *b = d->allocations;
    while (b) {
        block *next = b->next;
        DE_FREE(&d->allocator, b, b->bytes);
        b = next;
    }
    de_allocator a = d->allocator;
    DE_FREE(&a, d, sizeof *d);
}
de_result de_aseprite_parse(const void *source, u64 bytes, const de_allocator *allocator,
                            size_t max_bytes, de_aseprite_doc **out, const char **why) {
    if (out)
        *out = NULL;
    if (why)
        *why = "invalid Aseprite header";
    if (!source || !out || !allocator || bytes < 128 || bytes > ASE_BYTES_MAX)
        return DE_ERR_INVALID_ARGUMENT;
    if (!max_bytes)
        max_bytes = ASE_BYTES_MAX;
    if (max_bytes < sizeof(de_aseprite_doc))
        return DE_ERR_INVALID_ARGUMENT;
    de_aseprite_doc *d = DE_ALLOC(allocator, sizeof *d, 16, DE_MEM_ASSET);
    if (!d) {
        if (why)
            *why = "Aseprite allocation failed";
        return DE_ERR_OUT_OF_MEMORY;
    }
    memset(d, 0, sizeof *d);
    d->allocator = *allocator;
    d->max_bytes = max_bytes;
    d->allocated = sizeof *d;
    cursor h = {source, (const u8 *)source + 128, d};
    u32 declared = dword(&h);
    u16 magic = word(&h);
    d->frame_count = word(&h);
    d->width = word(&h);
    d->height = word(&h);
    d->depth = word(&h);
    d->flags = dword(&h);
    u16 speed = word(&h);
    (void)take(&h, 8);
    d->transparent_index = byte(&h);
    if (declared != bytes || magic != 0xa5e0 || !d->frame_count || !d->width || !d->height ||
        (d->depth != 8 && d->depth != 16 && d->depth != 32) ||
        (u64)d->width * d->height * 4u > max_bytes)
        fail(d, "invalid Aseprite header/extent");
    if (d->result == DE_OK) {
        d->frames = allocate(d, (size_t)d->frame_count * sizeof *d->frames);
        if (d->frames && preflight(d, source, (size_t)bytes, speed))
            (void)chunks(d, source);
    }
    if (d->result != DE_OK) {
        de_result r = d->result;
        if (why)
            *why = d->why;
        de_aseprite_free(d);
        return r;
    }
    *out = d;
    if (why)
        *why = NULL;
    return DE_OK;
}
