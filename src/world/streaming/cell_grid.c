/* DENG-271: cell grid helpers (see streaming_internal.h). */
#include "streaming_internal.h"

#include <dengine/de_asset.h>

void de_world_cell_of(f64 x, f64 y, i32 *cx, i32 *cy) {
    if (cx != NULL)
        *cx = de__cell_coord(x);
    if (cy != NULL)
        *cy = de__cell_coord(y);
}

void de__cell_keys_sort(de__cell_key *keys, u32 n) {
    for (u32 i = 1; i < n; ++i) {
        de__cell_key k = keys[i];
        u32 j = i;
        while (j > 0 && de__cell_key_cmp(&k, &keys[j - 1]) < 0) {
            keys[j] = keys[j - 1];
            --j;
        }
        keys[j] = k;
    }
}

u32 de__cells_in_radius(f64 x, f64 y, f64 radius, de__cell_key *out, u32 cap) {
    if (!(radius >= 0.0))
        return 0;
    i32 x0 = de__cell_coord(x - radius), x1 = de__cell_coord(x + radius);
    i32 y0 = de__cell_coord(y - radius), y1 = de__cell_coord(y + radius);
    f64 r2 = radius * radius;
    u32 n = 0, total = 0;
    for (i32 cx = x0; cx <= x1; ++cx) {
        for (i32 cy = y0; cy <= y1; ++cy) {
            f64 d2 = de__cell_dist2(cx, cy, x, y);
            if (d2 > r2)
                continue;
            ++total;
            de__cell_key k = {d2, cx, cy};
            if (n < cap) {
                out[n++] = k;
            } else if (cap != 0 && de__cell_key_cmp(&k, &out[cap - 1]) < 0) {
                out[cap - 1] = k; /* keep the nearest `cap` */
            } else {
                continue;
            }
            /* keep `out` sorted: sift the new key down */
            u32 j = n - 1;
            while (j > 0 && de__cell_key_cmp(&out[j], &out[j - 1]) < 0) {
                de__cell_key t = out[j];
                out[j] = out[j - 1];
                out[j - 1] = t;
                --j;
            }
        }
    }
    return total;
}

void de_world_cell_origins_relative(const i32 *cells, u32 count, de_dvec3 camera, de_vec3 *out) {
    if (cells == NULL || out == NULL)
        return;
    for (u32 i = 0; i < count; ++i) {
        de_dvec3 o = de__cell_origin(cells[2 * i], cells[2 * i + 1]);
        out[i].x = (f32)(o.x - camera.x);
        out[i].y = (f32)(o.y - camera.y);
        out[i].z = (f32)(o.z - camera.z);
    }
}
