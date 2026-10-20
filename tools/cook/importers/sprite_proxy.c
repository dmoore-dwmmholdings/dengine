#include "sprite_proxy.h"
#include <string.h>
#include <math.h>
typedef struct point {
    i32 x, y;
} point;
static i64 cross(point a, point b, point c) {
    return (i64)(b.x - a.x) * (c.y - a.y) - (i64)(b.y - a.y) * (c.x - a.x);
}
static bool opaque(const f32 *rgba, u32 w, u32 h, i32 x, i32 y, f32 cutoff) {
    return x >= 0 && y >= 0 && (u32)x < w && (u32)y < h &&
           rgba[((size_t)(h - 1 - (u32)y) * w + (u32)x) * 4 + 3] >= cutoff;
}
/* Exact union of vertically merged opaque runs. Multiple closed boxes preserve holes
 * and components without bridging contours. Internal faces stay inside the solid union.
 * If this representation exceeds either cooked limit, use alpha-tested geometry. */
static de_result topology_proxy(const f32 *rgba, u32 w, u32 h, f32 cutoff,
                                de_cooked_sprite_proxy *out) {
    typedef struct rectangle {
        u32 x0, x1, y0, y1;
    } rectangle;
    rectangle rectangles[DE_SPRITE_PROXY_MAX_INDICES / 36];
    u32 count = 0;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w;) {
            if (!opaque(rgba, w, h, (i32)x, (i32)y, cutoff)) {
                ++x;
                continue;
            }
            u32 x0 = x++;
            while (x < w && opaque(rgba, w, h, (i32)x, (i32)y, cutoff))
                ++x;
            u32 i = 0;
            while (i < count &&
                   !(rectangles[i].x0 == x0 && rectangles[i].x1 == x && rectangles[i].y1 == y))
                ++i;
            if (i == count) {
                if (count == sizeof rectangles / sizeof *rectangles)
                    goto unsupported;
                rectangles[count++] = (rectangle){x0, x, y, y};
            }
            rectangles[i].y1 = y + 1;
        }
    out->vertex_count = out->index_count = 0;
    for (u32 i = 0; i < count; ++i) {
        const rectangle *r = &rectangles[i];
        const u32 xs[] = {r->x0, r->x1, r->x1, r->x0}, ys[] = {r->y0, r->y0, r->y1, r->y1};
        u32 vertices[8];
        for (u32 side = 0; side < 2; ++side)
            for (u32 corner = 0; corner < 4; ++corner) {
                f32 position[] = {(f32)xs[corner] / w, (f32)ys[corner] / h, side ? .5f : -.5f};
                u32 v = 0;
                while (v < out->vertex_count &&
                       memcmp(out->positions[v], position, sizeof position))
                    ++v;
                if (v == out->vertex_count) {
                    if (v == DE_SPRITE_PROXY_MAX_VERTICES)
                        goto unsupported;
                    memcpy(out->positions[out->vertex_count++], position, sizeof position);
                }
                vertices[side * 4 + corner] = v;
            }
        static const u32 triangles[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                          1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
        for (u32 j = 0; j < 36; ++j)
            out->indices[out->index_count++] = vertices[triangles[j]];
    }
    return DE_OK;
unsupported:
    memset(out, 0, sizeof *out);
    out->region[2] = out->region[3] = 1;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "sprite proxy topology exceeds the cooked geometry budget");
}
de_result de_sprite_proxy_build(const de_allocator *a, const f32 *rgba, u32 w, u32 h, f32 cutoff,
                                de_cooked_sprite_proxy *out) {
    if (!a || !rgba || !out || !w || !h || w > 16384 || h > 16384)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "sprite proxy mask");
    memset(out, 0, sizeof *out);
    out->region[2] = out->region[3] = 1;
    size_t grid = (size_t)(w + 1) * (h + 1), max_points = (size_t)w * h * 4;
    u8 *edges = DE_ALLOC(a, grid, 16, DE_MEM_ASSET);
    point *points = DE_ALLOC(a, max_points * sizeof *points, 16, DE_MEM_ASSET);
    point *best = DE_ALLOC(a, max_points * sizeof *best, 16, DE_MEM_ASSET);
    de_result result = DE_OK;
    if (!edges || !points || !best) {
        result = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "sprite proxy contour");
        goto done;
    }
    memset(edges, 0, grid);
    /* March the binary mask's exact grid boundary, keeping solid cells on the left. */
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x)
            if (opaque(rgba, w, h, (i32)x, (i32)y, cutoff)) {
                if (!opaque(rgba, w, h, (i32)x, (i32)y - 1, cutoff))
                    edges[(size_t)y * (w + 1) + x] |= 1;
                if (!opaque(rgba, w, h, (i32)x + 1, (i32)y, cutoff))
                    edges[(size_t)y * (w + 1) + x + 1] |= 2;
                if (!opaque(rgba, w, h, (i32)x, (i32)y + 1, cutoff))
                    edges[(size_t)(y + 1) * (w + 1) + x + 1] |= 4;
                if (!opaque(rgba, w, h, (i32)x - 1, (i32)y, cutoff))
                    edges[(size_t)(y + 1) * (w + 1) + x] |= 8;
            }
    const i32 dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
    u32 count = 0, contours = 0;
    bool complex = false;
    i64 best_area = 0;
    for (size_t start = 0; start < grid; ++start)
        while (edges[start]) {
            point current = {(i32)(start % (w + 1)), (i32)(start / (w + 1))};
            size_t at = start;
            u32 n = 0;
            do {
                if (n >= max_points) {
                    result = DE_FAIL(DE_ERR_INTERNAL, "sprite proxy contour overflow");
                    goto done;
                }
                points[n++] = current;
                complex |= (edges[at] & (edges[at] - 1)) != 0;
                u32 direction = 0;
                while (direction < 4 && !(edges[at] & (1u << direction)))
                    ++direction;
                if (direction == 4)
                    break;
                edges[at] &= (u8) ~(1u << direction);
                current.x += dx[direction];
                current.y += dy[direction];
                at = (size_t)current.y * (w + 1) + (u32)current.x;
            } while (at != start);
            i64 area = 0;
            for (u32 i = 0; i < n; ++i)
                area += (i64)points[i].x * points[(i + 1) % n].y -
                        (i64)points[(i + 1) % n].x * points[i].y;
            if (n >= 3)
                ++contours;
            if (area > best_area) {
                best_area = area;
                count = n;
                memcpy(best, points, (size_t)n * sizeof *best);
            }
        }
    if (complex || contours > 1) {
        result = topology_proxy(rgba, w, h, cutoff, out);
        goto done;
    }
    if (count < 3) {
        result = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "sprite proxy has no opaque silhouette");
        goto done;
    }
    /* Deterministic polygon simplification: remove straight runs, then smallest-area corners. */
    for (u32 i = 0; count > 3 && i < count;) {
        if (cross(best[(i + count - 1) % count], best[i], best[(i + 1) % count]) == 0) {
            memmove(best + i, best + i + 1, (size_t)(count - i - 1) * sizeof *best);
            --count;
            if (i)
                i--;
        } else
            ++i;
    }
    while (count > 16) {
        u32 remove = 0;
        i64 area = INT64_MAX;
        for (u32 i = 0; i < count; ++i) {
            i64 c = cross(best[(i + count - 1) % count], best[i], best[(i + 1) % count]);
            if (c < 0)
                c = -c;
            if (c < area) {
                area = c;
                remove = i;
            }
        }
        memmove(best + remove, best + remove + 1, (size_t)(count - remove - 1) * sizeof *best);
        --count;
    }
    out->vertex_count = count * 2;
    for (u32 i = 0; i < count; ++i)
        for (u32 side = 0; side < 2; ++side) {
            out->positions[i + side * count][0] = (f32)best[i].x / (f32)w;
            out->positions[i + side * count][1] = (f32)best[i].y / (f32)h;
            out->positions[i + side * count][2] = side ? .5f : -.5f;
        }
    u32 remaining[16];
    for (u32 i = 0; i < count; ++i)
        remaining[i] = i;
    u32 n = count;
    while (n > 2) {
        bool found = false;
        for (u32 i = 0; i < n; ++i) {
            u32 a0 = remaining[(i + n - 1) % n], b = remaining[i], c = remaining[(i + 1) % n];
            if (cross(best[a0], best[b], best[c]) <= 0)
                continue;
            bool inside = false;
            for (u32 j = 0; j < n; ++j) {
                u32 q = remaining[j];
                if (q == a0 || q == b || q == c)
                    continue;
                if (cross(best[a0], best[b], best[q]) >= 0 &&
                    cross(best[b], best[c], best[q]) >= 0 && cross(best[c], best[a0], best[q]) >= 0)
                    inside = true;
            }
            if (inside)
                continue;
            u32 tri[6] = {c, b, a0, a0 + count, b + count, c + count};
            memcpy(out->indices + out->index_count, tri, sizeof tri);
            out->index_count += 6;
            memmove(remaining + i, remaining + i + 1, (size_t)(n - i - 1) * sizeof *remaining);
            --n;
            found = true;
            break;
        }
        if (!found) {
            result = DE_FAIL(DE_ERR_UNSUPPORTED, "sprite proxy silhouette cannot be triangulated");
            goto done;
        }
    }
    for (u32 i = 0; i < count; ++i) {
        u32 j = (i + 1) % count;
        u32 side[6] = {i, j, j + count, i, j + count, i + count};
        memcpy(out->indices + out->index_count, side, sizeof side);
        out->index_count += 6;
    }
done:
    if (edges)
        DE_FREE(a, edges, grid);
    if (points)
        DE_FREE(a, points, max_points * sizeof *points);
    if (best)
        DE_FREE(a, best, max_points * sizeof *best);
    return result;
}
