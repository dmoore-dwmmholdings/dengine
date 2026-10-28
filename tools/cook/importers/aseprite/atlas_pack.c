#include "atlas_pack.h"
#include <string.h>

typedef struct budget {
    de_allocator parent;
    size_t live, limit;
    bool exceeded;
} budget;
#define BLOCK_HEAD 16u
static void release(const de_allocator *a, void *ptr, size_t bytes) {
    if (ptr)
        DE_FREE(a, (u8 *)ptr - BLOCK_HEAD, bytes + BLOCK_HEAD);
}
static void *bounded_alloc(void *user, size_t bytes, size_t align, u32 tag) {
    budget *b = user;
    if (bytes > SIZE_MAX - BLOCK_HEAD || bytes + BLOCK_HEAD > b->limit - b->live) {
        b->exceeded = true;
        return NULL;
    }
    if (align > BLOCK_HEAD)
        return NULL;
    void *ptr = DE_ALLOC(&b->parent, bytes + BLOCK_HEAD, BLOCK_HEAD, tag);
    if (!ptr)
        return NULL;
    b->live += bytes + BLOCK_HEAD;
    return (u8 *)ptr + BLOCK_HEAD;
}
static void bounded_free(void *user, void *ptr, size_t bytes) {
    budget *b = user;
    if (ptr) {
        b->live -= bytes + BLOCK_HEAD;
        release(&b->parent, ptr, bytes);
    }
}
void de_aseprite_atlas_free(de_aseprite_atlas *atlas) {
    if (!atlas)
        return;
    de_allocator a = atlas->allocator;
    release(&a, atlas->rgba, atlas->rgba_bytes);
    release(&a, atlas, atlas->metadata_bytes);
}

typedef struct skyline {
    u32 x, y, width;
} skyline;
/* Sort large rectangles first; unique frame indexes break every tie. */
static bool before(const de_aseprite_atlas *a, u32 x, u32 y) {
    const de_aseprite_atlas_frame *f = &a->frames[x], *g = &a->frames[y];
    u64 fa = (u64)(f->width + 2u) * (f->height + 2u);
    u64 ga = (u64)(g->width + 2u) * (g->height + 2u);
    if (fa != ga)
        return fa > ga;
    if (f->height != g->height)
        return f->height > g->height;
    if (f->width != g->width)
        return f->width > g->width;
    return x < y;
}
static void sift(const de_aseprite_atlas *a, u32 *order, u32 count, u32 root) {
    while (root * 2u + 1u < count) {
        u32 child = root * 2u + 1u;
        if (child + 1u < count && before(a, order[child], order[child + 1u]))
            ++child;
        if (!before(a, order[root], order[child]))
            break;
        u32 swap = order[root];
        order[root] = order[child];
        order[child] = swap;
        root = child;
    }
}
static void sort(const de_aseprite_atlas *a, u32 *order) {
    for (u32 i = 0; i < a->frame_count; ++i)
        order[i] = i;
    for (u32 i = a->frame_count / 2u; i; --i)
        sift(a, order, a->frame_count, i - 1u);
    for (u32 i = a->frame_count; i > 1u; --i) {
        u32 swap = order[0];
        order[0] = order[i - 1u];
        order[i - 1u] = swap;
        sift(a, order, i - 1u, 0);
    }
}
static u32 rounded(u32 extent, u32 limit) {
    u32 result = 1;
    while (result < extent)
        result *= 2u;
    return result > limit ? limit : result;
}
static bool pack(de_aseprite_atlas *a, const u32 *order, skyline *nodes, u32 width, u32 limit,
                 u32 *height) {
    u32 count = 1, used = 0;
    nodes[0] = (skyline){0, 0, width};
    for (u32 entry = 0; entry < a->frame_count; ++entry) {
        de_aseprite_atlas_frame *f = &a->frames[order[entry]];
        u32 w = f->width + 2u, h = f->height + 2u;
        u32 best = UINT32_MAX, best_y = UINT32_MAX;
        for (u32 i = 0; i < count; ++i) {
            if (w > width - nodes[i].x)
                continue;
            u32 left = w, y = nodes[i].y;
            for (u32 j = i; left && j < count; ++j) {
                if (nodes[j].y > y)
                    y = nodes[j].y;
                left = nodes[j].width < left ? left - nodes[j].width : 0;
            }
            if (!left && h <= limit - y && y < best_y) {
                best = i;
                best_y = y;
            }
        }
        if (best == UINT32_MAX)
            return false;
        u32 x = nodes[best].x, right = x + w;
        f->x = x + 1u;
        f->y = best_y + 1u;
        if (best_y + h > used)
            used = best_y + h;
        memmove(nodes + best + 1u, nodes + best, (size_t)(count - best) * sizeof *nodes);
        nodes[best] = (skyline){x, best_y + h, w};
        ++count;
        u32 j = best + 1u;
        while (j < count && nodes[j].x < right) {
            u32 end = nodes[j].x + nodes[j].width;
            if (end > right) {
                nodes[j].x = right;
                nodes[j].width = end - right;
                break;
            }
            memmove(nodes + j, nodes + j + 1u, (size_t)(count - j - 1u) * sizeof *nodes);
            --count;
        }
        for (j = 1; j < count;)
            if (nodes[j - 1u].y == nodes[j].y) {
                nodes[j - 1u].width += nodes[j].width;
                memmove(nodes + j, nodes + j + 1u, (size_t)(count - j - 1u) * sizeof *nodes);
                --count;
            } else
                ++j;
    }
    *height = used;
    return true;
}
static void trim(de_aseprite_atlas_frame *f, const u8 *canvas, u32 width, u32 height) {
    u32 left = width, top = height, right = 0, bottom = 0;
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x) {
            const u8 *pixel = canvas + ((size_t)y * width + x) * 4u;
            /* Aseprite retains RGB beneath alpha zero. Trim canonical zero pixels
             * only, so expanding cooked geometry reproduces every source RGBA byte. */
            if (pixel[0] || pixel[1] || pixel[2] || pixel[3]) {
                if (x < left)
                    left = x;
                if (y < top)
                    top = y;
                if (x + 1u > right)
                    right = x + 1u;
                if (y + 1u > bottom)
                    bottom = y + 1u;
            }
        }
    if (right) {
        f->trim_x = left;
        f->trim_y = top;
        f->trim_width = right - left;
        f->trim_height = bottom - top;
    }
    f->width = f->trim_width ? f->trim_width : 1u;
    f->height = f->trim_height ? f->trim_height : 1u;
}
static void blit(de_aseprite_atlas *a, const de_aseprite_atlas_frame *f, const u8 *canvas) {
    if (!f->trim_width)
        return;
    for (u32 y = 0; y < f->height + 2u; ++y) {
        u32 sy = y ? y - 1u : 0;
        if (sy >= f->height)
            sy = f->height - 1u;
        for (u32 x = 0; x < f->width + 2u; ++x) {
            u32 sx = x ? x - 1u : 0;
            if (sx >= f->width)
                sx = f->width - 1u;
            memcpy(a->rgba + (((size_t)f->y + y - 1u) * a->width + f->x + x - 1u) * 4u,
                   canvas + (((size_t)f->trim_y + sy) * a->canvas_width + f->trim_x + sx) * 4u, 4);
        }
    }
}
de_result de_aseprite_atlas_build(const de_aseprite_doc *doc, const de_allocator *allocator,
                                  const de_aseprite_atlas_desc *desc, de_aseprite_atlas **out,
                                  const char **why) {
    if (out)
        *out = NULL;
    if (why)
        *why = "invalid Aseprite atlas arguments";
    if (!doc || !allocator || !out || !doc->width || !doc->height || !doc->frame_count ||
        !doc->layer_count || !doc->layers || !doc->frames)
        return DE_ERR_INVALID_ARGUMENT;
    de_aseprite_atlas_desc options = desc ? *desc : (de_aseprite_atlas_desc){0};
    u32 limit = options.max_size ? options.max_size : 4096u;
    if (limit < 3u || limit > 16384u)
        return DE_ERR_INVALID_ARGUMENT;
    budget memory = {.parent = *allocator,
                     .limit = options.max_bytes ? options.max_bytes : 256u * 1024u * 1024u};
    de_allocator a = {.alloc = bounded_alloc, .free = bounded_free, .user = &memory};
    de_aseprite_renderer *renderer = NULL;
    de_aseprite_atlas *atlas = NULL;
    u32 *order = NULL;
    skyline *nodes = NULL;
    u8 *canvas = NULL;
    size_t order_bytes = 0, node_bytes = 0, canvas_bytes = 0;
    const char *message = "Aseprite atlas allocation failed";
    de_result r = de_aseprite_renderer_create(doc, &a, memory.limit, options.warning,
                                              options.warning_user, &renderer, &message);
    if (r != DE_OK) {
        if (r == DE_ERR_INVALID_ARGUMENT) {
            r = DE_ERR_UNSUPPORTED;
            message = "Aseprite atlas allocation budget exceeded";
        }
        goto done;
    }
    u32 sets = 1;
    if (options.keep_layers)
        for (u32 i = 0; i < doc->layer_count; ++i)
            if (doc->layers[i].type == 0 && de_aseprite_layer_visible(renderer, i))
                ++sets;
    u64 frames = (u64)sets * doc->frame_count;
    u64 metadata = sizeof *atlas + (u64)sets * sizeof *atlas->sets + frames * sizeof *atlas->frames;
    if (frames > UINT32_MAX || frames * 9u > (u64)limit * limit || metadata > SIZE_MAX) {
        r = DE_ERR_UNSUPPORTED;
        message = "Aseprite frame sets exceed atlas capacity";
        goto done;
    }
    atlas = DE_ALLOC(&a, (size_t)metadata, 16, DE_MEM_ASSET);
    if (!atlas) {
        r = DE_ERR_OUT_OF_MEMORY;
        goto done;
    }
    memset(atlas, 0, (size_t)metadata);
    atlas->allocator = *allocator;
    atlas->metadata_bytes = (size_t)metadata;
    atlas->canvas_width = doc->width;
    atlas->canvas_height = doc->height;
    atlas->set_count = sets;
    atlas->frames_per_set = doc->frame_count;
    atlas->frame_count = (u32)frames;
    atlas->sets = (de_aseprite_atlas_set *)(atlas + 1);
    atlas->frames = (de_aseprite_atlas_frame *)(atlas->sets + sets);
    atlas->sets[0].layer = UINT32_MAX;
    if (options.keep_layers)
        for (u32 i = 0, set = 1; i < doc->layer_count; ++i)
            if (doc->layers[i].type == 0 && de_aseprite_layer_visible(renderer, i)) {
                atlas->sets[set] =
                    (de_aseprite_atlas_set){doc->layers[i].name, i, set * doc->frame_count};
                ++set;
            }
    canvas_bytes = (size_t)doc->width * doc->height * 4u;
    order_bytes = (size_t)frames * sizeof *order;
    node_bytes = ((size_t)frames + 1u) * sizeof *nodes;
    canvas = DE_ALLOC(&a, canvas_bytes, 16, DE_MEM_ASSET);
    order = DE_ALLOC(&a, order_bytes, 16, DE_MEM_ASSET);
    nodes = DE_ALLOC(&a, node_bytes, 16, DE_MEM_ASSET);
    if (!canvas || !order || !nodes) {
        r = DE_ERR_OUT_OF_MEMORY;
        goto done;
    }
    u64 area = 0;
    u32 minimum = 3;
    for (u32 s = 0; s < sets; ++s)
        for (u32 f = 0; f < doc->frame_count; ++f) {
            r = de_aseprite_render_frame(renderer, f, atlas->sets[s].layer, canvas,
                                         (size_t)doc->width * 4u, canvas_bytes, &message);
            if (r != DE_OK)
                goto done;
            de_aseprite_atlas_frame *entry = &atlas->frames[s * doc->frame_count + f];
            entry->ms = doc->frames[f].ms;
            trim(entry, canvas, doc->width, doc->height);
            if (entry->width + 2u > minimum)
                minimum = entry->width + 2u;
            area += (u64)(entry->width + 2u) * (entry->height + 2u);
            if (entry->width + 2u > limit || entry->height + 2u > limit ||
                area > (u64)limit * limit) {
                r = DE_ERR_UNSUPPORTED;
                message = "Aseprite trimmed frames exceed atlas size limit";
                goto done;
            }
        }
    sort(atlas, order);
    u64 best_area = UINT64_MAX;
    for (u32 width = rounded(minimum, limit);; width = width <= limit / 2u ? width * 2u : limit) {
        u32 height;
        if (pack(atlas, order, nodes, width, limit, &height)) {
            height = rounded(height, limit);
            u64 candidate = (u64)width * height;
            u32 longest = width > height ? width : height;
            u32 previous = atlas->width > atlas->height ? atlas->width : atlas->height;
            if (candidate < best_area || (candidate == best_area && longest < previous)) {
                best_area = candidate;
                atlas->width = width;
                atlas->height = height;
            }
        }
        if (width == limit)
            break;
    }
    u32 used;
    if (!atlas->width || !pack(atlas, order, nodes, atlas->width, limit, &used)) {
        r = DE_ERR_UNSUPPORTED;
        message = "Aseprite frames do not fit a single atlas";
        goto done;
    }
    atlas->rgba_bytes = (size_t)atlas->width * atlas->height * 4u;
    atlas->rgba = DE_ALLOC(&a, atlas->rgba_bytes, 16, DE_MEM_ASSET);
    if (!atlas->rgba) {
        r = DE_ERR_OUT_OF_MEMORY;
        goto done;
    }
    memset(atlas->rgba, 0, atlas->rgba_bytes);
    for (u32 s = 0; s < sets; ++s)
        for (u32 f = 0; f < doc->frame_count; ++f) {
            r = de_aseprite_render_frame(renderer, f, atlas->sets[s].layer, canvas,
                                         (size_t)doc->width * 4u, canvas_bytes, &message);
            if (r != DE_OK)
                goto done;
            blit(atlas, &atlas->frames[s * doc->frame_count + f], canvas);
        }
done:
    de_aseprite_renderer_destroy(renderer);
    DE_FREE(&a, canvas, canvas_bytes);
    DE_FREE(&a, order, order_bytes);
    DE_FREE(&a, nodes, node_bytes);
    if (r != DE_OK) {
        de_aseprite_atlas_free(atlas);
        if (!message)
            message = "Aseprite atlas allocation failed";
        if (memory.exceeded) {
            r = DE_ERR_UNSUPPORTED;
            message = "Aseprite atlas allocation budget exceeded";
        }
    } else
        *out = atlas;
    if (why)
        *why = r == DE_OK ? NULL : message;
    return r;
}
