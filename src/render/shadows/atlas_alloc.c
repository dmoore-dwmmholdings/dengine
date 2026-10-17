#include "render/shadows/shadows_internal.h"
#include <string.h>

/* 0 free, 1 split, 2 allocated, 3 full subtree. Children are 4*n+1..4*n+4.
 * The bounded (at most nine levels) traversal always visits NW, NE, SW, SE. */
de_result de__atlas_alloc_init(de__atlas_alloc *a, const de_allocator *allocator, u32 size) {
    if (!a || size < 16 || size > 8192 || (size & (size - 1u)))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "shadow atlas allocator size");
    memset(a, 0, sizeof *a);
    a->allocator = allocator ? allocator : de_heap_allocator();
    a->size = size;
    const u32 leaves = (size / 16u) * (size / 16u);
    a->node_count = (leaves * 4u - 1u) / 3u;
    a->nodes = DE_ALLOC(a->allocator, a->node_count, 1, DE_MEM_RENDER);
    if (!a->nodes) {
        memset(a, 0, sizeof *a);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "shadow atlas nodes");
    }
    memset(a->nodes, 0, a->node_count);
    return DE_OK;
}
void de__atlas_alloc_destroy(de__atlas_alloc *a) {
    if (!a)
        return;
    if (a->nodes)
        DE_FREE(a->allocator, a->nodes, a->node_count);
    memset(a, 0, sizeof *a);
}
static bool take(de__atlas_alloc *a, u32 n, u32 x, u32 y, u32 extent, u32 size,
                 de__atlas_tile *out) {
    if (a->nodes[n] >= 2)
        return false;
    if (extent == size) {
        if (a->nodes[n] != 0)
            return false;
        a->nodes[n] = 2;
        *out = (de__atlas_tile){n, x, y, size};
        return true;
    }
    const u32 half = extent / 2u;
    for (u32 c = 0; c < 4; ++c) {
        if (take(a, n * 4u + 1u + c, x + (c & 1u) * half, y + (c >> 1u) * half, half, size, out)) {
            bool full = true;
            for (u32 i = 0; i < 4; ++i)
                full = full && a->nodes[n * 4u + 1u + i] >= 2;
            a->nodes[n] = full ? 3 : 1;
            return true;
        }
    }
    return false;
}
bool de__atlas_alloc_take(de__atlas_alloc *a, u32 size, de__atlas_tile *out) {
    if (!a || !a->nodes || !out || size < 16 || size > 2048 || size > a->size ||
        (size & (size - 1u)))
        return false;
    if (!take(a, 0, 0, 0, a->size, size, out))
        return false;
    a->occupied += size * size;
    return true;
}
bool de__atlas_alloc_free(de__atlas_alloc *a, de__atlas_tile tile) {
    if (!a || !a->nodes || tile.node >= a->node_count || a->nodes[tile.node] != 2)
        return false;
    /* Check geometry too: malformed handles must not corrupt occupancy or another tile. */
    u32 n = tile.node, x = 0, y = 0, size = 16, path[10], depth = 0;
    while (n) {
        path[depth++] = (n - 1u) % 4u;
        n = (n - 1u) / 4u;
    }
    size = a->size;
    for (u32 i = depth; i > 0; --i) {
        size /= 2u;
        x += (path[i - 1u] & 1u) * size;
        y += (path[i - 1u] >> 1u) * size;
    }
    if (tile.size != size || tile.x != x || tile.y != y)
        return false;
    a->nodes[tile.node] = 0;
    a->occupied -= size * size;
    n = tile.node;
    while (n) {
        n = (n - 1u) / 4u;
        bool empty = true;
        for (u32 c = 0; c < 4; ++c)
            empty = empty && a->nodes[n * 4u + 1u + c] == 0;
        a->nodes[n] = empty ? 0 : 1;
    }
    return true;
}
