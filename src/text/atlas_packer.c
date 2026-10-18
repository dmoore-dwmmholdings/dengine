#include "atlas_packer.h"
#include <string.h>
void de_atlas_packer_init(de_atlas_packer *p, u32 w, u32 h, u32 padding) {
    memset(p, 0, sizeof *p);
    p->width = w;
    p->height = h;
    p->padding = padding;
    p->count = 1;
    p->nodes[0] = (de_atlas_node){0, 0, w};
}
static bool fit(de_atlas_packer *p, u32 at, u32 w, u32 h, u32 *y) {
    u32 x = p->nodes[at].x;
    if (x > p->width || w > p->width - x)
        return false;
    u32 top = p->nodes[at].y, left = w;
    for (u32 i = at; left; ++i) {
        if (i >= p->count)
            return false;
        if (p->nodes[i].y > top)
            top = p->nodes[i].y;
        if (top > p->height || h > p->height - top)
            return false;
        if (p->nodes[i].width >= left)
            left = 0;
        else
            left -= p->nodes[i].width;
    }
    *y = top;
    return true;
}
bool de_atlas_packer_insert(de_atlas_packer *p, u32 width, u32 height, u32 *x, u32 *y) {
    if (!p || !x || !y || !width || !height || p->padding > UINT32_MAX / 2)
        return false;
    u32 pad = p->padding * 2;
    if (width > UINT32_MAX - pad || height > UINT32_MAX - pad)
        return false;
    u32 w = width + pad, h = height + pad, best = UINT32_MAX, by = UINT32_MAX, bw = UINT32_MAX;
    for (u32 i = 0; i < p->count; ++i) {
        u32 top;
        if (fit(p, i, w, h, &top) && (top < by || (top == by && p->nodes[i].width < bw))) {
            best = i;
            by = top;
            bw = p->nodes[i].width;
        }
    }
    if (best == UINT32_MAX || p->count >= DE_ATLAS_SKYLINE_MAX)
        return false;
    u32 bx = p->nodes[best].x;
    memmove(p->nodes + best + 1, p->nodes + best, (p->count - best) * sizeof *p->nodes);
    p->nodes[best] = (de_atlas_node){bx, by + h, w};
    ++p->count;
    for (u32 i = best + 1; i < p->count;) {
        de_atlas_node *prev = &p->nodes[i - 1], *n = &p->nodes[i];
        u32 edge = prev->x + prev->width;
        if (n->x >= edge)
            break;
        u32 cut = edge - n->x;
        if (cut >= n->width) {
            memmove(n, n + 1, (p->count - i - 1) * sizeof *n);
            --p->count;
        } else {
            n->x += cut;
            n->width -= cut;
            break;
        }
    }
    for (u32 i = 1; i < p->count;) {
        if (p->nodes[i - 1].y == p->nodes[i].y) {
            p->nodes[i - 1].width += p->nodes[i].width;
            memmove(p->nodes + i, p->nodes + i + 1, (p->count - i - 1) * sizeof *p->nodes);
            --p->count;
        } else
            ++i;
    }
    p->used += (u64)w * h;
    *x = bx + p->padding;
    *y = by + p->padding;
    return true;
}
