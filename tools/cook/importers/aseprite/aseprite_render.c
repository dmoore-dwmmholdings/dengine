#include "aseprite_render.h"
#include <stdlib.h>
#include <string.h>

typedef struct render_item {
    u32 parent, layer;
    i32 order, z;
} render_item;
struct de_aseprite_renderer {
    const de_aseprite_doc *doc;
    de_allocator allocator;
    size_t bytes, row_bytes;
    render_item *items;
    u32 *cels, *first, *count;
    u8 *visible, *selected, *rows;
    u32 item_count;
};

bool de_aseprite_layer_visible(const de_aseprite_renderer *r, u32 layer) {
    return r && layer < r->doc->layer_count && r->visible[layer];
}
de_result de_aseprite_renderer_create(const de_aseprite_doc *doc, const de_allocator *allocator,
                                      size_t max_bytes, de_aseprite_warning_fn warning, void *user,
                                      de_aseprite_renderer **out, const char **why) {
    if (out)
        *out = NULL;
    if (why)
        *why = "invalid Aseprite renderer arguments/budget";
    if (!doc || !allocator || !out || !doc->layer_count || !doc->width || !doc->height)
        return DE_ERR_INVALID_ARGUMENT;
    if (!max_bytes)
        max_bytes = 256u * 1024u * 1024u;
    u32 groups = 0;
    if (doc->flags & 2u)
        for (u32 i = 0; i < doc->layer_count; ++i)
            if (doc->layers[i].type == 1 && doc->layers[i].depth + 1u > groups)
                groups = doc->layers[i].depth + 1u;
    const size_t head = (sizeof(de_aseprite_renderer) + 15u) & ~(size_t)15u;
    u64 required = head + (u64)doc->layer_count * (sizeof(render_item) + sizeof(u32) + 2u) +
                   ((u64)doc->layer_count + 1u) * 2u * sizeof(u32) + (u64)doc->width * 4u * groups;
    if (required > SIZE_MAX || required > max_bytes)
        return DE_ERR_INVALID_ARGUMENT;
    de_aseprite_renderer *r = DE_ALLOC(allocator, (size_t)required, 16, DE_MEM_ASSET);
    if (!r) {
        if (why)
            *why = "Aseprite renderer allocation failed";
        return DE_ERR_OUT_OF_MEMORY;
    }
    memset(r, 0, (size_t)required);
    r->doc = doc;
    r->allocator = *allocator;
    r->bytes = (size_t)required;
    r->row_bytes = (size_t)doc->width * 4u;
    r->items = (render_item *)((u8 *)r + head);
    r->cels = (u32 *)(r->items + doc->layer_count);
    r->first = r->cels + doc->layer_count;
    r->count = r->first + doc->layer_count + 1u;
    r->visible = (u8 *)(r->count + doc->layer_count + 1u);
    r->selected = r->visible + doc->layer_count;
    r->rows = r->selected + doc->layer_count;
    for (u32 i = 0; i < doc->layer_count; ++i) {
        const de_aseprite_layer *l = &doc->layers[i];
        r->visible[i] = (u8)((l->flags & 1u) && (l->parent == UINT32_MAX || r->visible[l->parent]));
        if (warning && r->visible[i]) {
            if (l->type == 2)
                warning(user, l->name, "tilemap layer skipped (unsupported)");
            else if (l->blend)
                warning(user, l->name, "unsupported blend mode; composited as normal");
        }
    }
    *out = r;
    if (why)
        *why = NULL;
    return DE_OK;
}
void de_aseprite_renderer_destroy(de_aseprite_renderer *r) {
    if (!r)
        return;
    de_allocator allocator = r->allocator;
    DE_FREE(&allocator, r, r->bytes);
}

/* Unique final layer keys make sorting deterministic. Hidden layers still occupy
 * their original z-order slots. Group plans use sibling order, not global indexes. */
static int item_compare(const render_item *a, const render_item *b) {
    if (a->parent != b->parent)
        return a->parent < b->parent ? -1 : 1;
    if (a->order != b->order)
        return a->order < b->order ? -1 : 1;
    if (a->z != b->z)
        return a->z < b->z ? -1 : 1;
    return a->layer < b->layer ? -1 : a->layer != b->layer;
}
static void sift(render_item *items, u32 count, u32 root) {
    while (root * 2u + 1u < count) {
        u32 child = root * 2u + 1u;
        if (child + 1u < count && item_compare(&items[child], &items[child + 1u]) < 0)
            ++child;
        if (item_compare(&items[root], &items[child]) >= 0)
            break;
        render_item swap = items[root];
        items[root] = items[child];
        items[child] = swap;
        root = child;
    }
}
static void sort_items(render_item *items, u32 count) {
    for (u32 i = count / 2u; i; --i)
        sift(items, count, i - 1u);
    for (u32 i = count; i > 1u; --i) {
        render_item swap = items[0];
        items[0] = items[i - 1u];
        items[i - 1u] = swap;
        sift(items, i - 1u, 0);
    }
}
static void plan(de_aseprite_renderer *r, u32 frame, u32 selected) {
    const de_aseprite_doc *d = r->doc;
    const de_aseprite_frame *f = &d->frames[frame];
    size_t n = d->layer_count;
    memset(r->cels, 0, n * sizeof *r->cels);
    memset(r->count, 0, (n + 1u) * sizeof *r->count);
    memset(r->selected, 0, n);
    if (selected == UINT32_MAX)
        memcpy(r->selected, r->visible, n);
    else if (r->visible[selected])
        for (u32 i = selected; i != UINT32_MAX; i = d->layers[i].parent)
            r->selected[i] = 1;
    for (u32 i = f->first_cel; i < f->first_cel + f->cel_count; ++i)
        r->cels[d->cels[i].layer] = i + 1u;
    r->item_count = 0;
    for (u32 i = 0; i < d->layer_count; ++i) {
        const de_aseprite_layer *l = &d->layers[i];
        if (l->type == 1 && !(d->flags & 2u))
            continue;
        u32 parent = (d->flags & 2u) && l->parent != UINT32_MAX ? l->parent : d->layer_count;
        u32 order = (d->flags & 2u) ? ++r->count[parent] : i + 1u;
        i32 z = r->cels[i] ? d->cels[r->cels[i] - 1u].z : 0;
        r->items[r->item_count++] = (render_item){parent, i, (i32)order + z, z};
    }
    sort_items(r->items, r->item_count);
    memset(r->count, 0, (n + 1u) * sizeof *r->count);
    for (u32 i = 0; i < r->item_count; ++i) {
        u32 parent = r->items[i].parent;
        if (!r->count[parent])
            r->first[parent] = i;
        ++r->count[parent];
    }
}
static u32 opacity(u32 a, u32 b) {
    return (a * b + 127u) / 255u;
}
/* Straight-alpha source-over: RGB division truncates toward zero, and opacity
 * multiplication rounds to the nearest UNORM8 value. No floating-point host drift. */
static void over(u8 *dst, const u8 *src, u32 alpha_scale) {
    u32 a = opacity(src[3], alpha_scale);
    if (!dst[3]) {
        memcpy(dst, src, 3);
        dst[3] = (u8)a;
    } else if (src[3]) {
        u32 total = a + dst[3] - opacity(a, dst[3]);
        for (u32 k = 0; k < 3; ++k)
            dst[k] = (u8)((i32)dst[k] + ((i32)src[k] - dst[k]) * (i32)a / (i32)total);
        dst[3] = (u8)total;
    }
}
static void image_row(de_aseprite_renderer *r, u32 frame, u32 layer, u32 y, u8 *dst) {
    if (!r->cels[layer])
        return;
    const de_aseprite_doc *d = r->doc;
    const de_aseprite_layer *l = &d->layers[layer];
    const de_aseprite_cel *c = &d->cels[r->cels[layer] - 1u];
    i32 source_y = (i32)y - c->y;
    if (source_y < 0 || (u32)source_y >= c->height)
        return;
    i32 begin = c->x < 0 ? 0 : c->x;
    i32 end = c->x + (i32)c->width;
    if (end > d->width)
        end = d->width;
    u32 scale = opacity(l->opacity, c->opacity);
    if (!scale)
        return;
    for (i32 x = begin; x < end; ++x) {
        size_t at = ((size_t)source_y * c->width + (u32)(x - c->x)) * (d->depth / 8u);
        const u8 *pixel = c->pixels + at;
        u8 rgba[4];
        if (d->depth == 32)
            memcpy(rgba, pixel, 4);
        else if (d->depth == 16) {
            rgba[0] = rgba[1] = rgba[2] = pixel[0];
            rgba[3] = pixel[1];
        } else {
            if (*pixel == d->transparent_index && !(l->flags & 8u))
                continue;
            memcpy(rgba, d->frames[frame].palette[*pixel], 4);
        }
        over(dst + (size_t)x * 4u, rgba, scale);
    }
}
static void group_row(de_aseprite_renderer *r, u32 parent, u32 frame, u32 y, u32 depth, u8 *dst) {
    u32 begin = r->first[parent], end = begin + r->count[parent];
    for (u32 i = begin; i < end; ++i) {
        u32 layer = r->items[i].layer;
        const de_aseprite_layer *l = &r->doc->layers[layer];
        if (!r->selected[layer])
            continue;
        if (l->type == 0)
            image_row(r, frame, layer, y, dst);
        else if (l->type == 1 && l->opacity) {
            u8 *row = r->rows + (size_t)depth * r->row_bytes;
            memset(row, 0, r->row_bytes);
            group_row(r, layer, frame, y, depth + 1u, row);
            for (u32 x = 0; x < r->doc->width; ++x)
                over(dst + (size_t)x * 4u, row + (size_t)x * 4u, l->opacity);
        }
    }
}
de_result de_aseprite_render_frame(de_aseprite_renderer *r, u32 frame, u32 layer, void *rgba,
                                   size_t stride, size_t bytes, const char **why) {
    if (why)
        *why = "invalid Aseprite render target/frame/layer";
    if (!r || !rgba || frame >= r->doc->frame_count ||
        (layer != UINT32_MAX &&
         (layer >= r->doc->layer_count || r->doc->layers[layer].type != 0)) ||
        stride < r->row_bytes || (r->doc->height - 1u) > (SIZE_MAX - r->row_bytes) / stride ||
        bytes < (size_t)(r->doc->height - 1u) * stride + r->row_bytes)
        return DE_ERR_INVALID_ARGUMENT;
    plan(r, frame, layer);
    for (u32 y = 0; y < r->doc->height; ++y) {
        u8 *row = (u8 *)rgba + (size_t)y * stride;
        memset(row, 0, r->row_bytes);
        group_row(r, r->doc->layer_count, frame, y, 0, row);
    }
    if (why)
        *why = NULL;
    return DE_OK;
}
