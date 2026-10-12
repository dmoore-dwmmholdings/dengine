/* heightfield.c: heightfield shape (DENG-132): a copy of the samples and per-block z bounds for
 * the midphase (DENG-134). Layout and triangulation: de_shape_desc and shape_internal.h. */
#include "physics/shapes/shape_internal.h"

de_result de__heightfield_build(de_physics_world *p, const de_shape_desc *d, de__shape *s) {
    if (d->heights == NULL || d->rows < 2 || d->cols < 2)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_shape_create: heightfield needs heights and rows, cols >= 2");
    if ((u64)d->rows * (u64)d->cols > 0x7FFFFFFFull)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape_create: heightfield too large");
    const de_vec3 sc = d->scale;
    if (!de__v3_finite(sc) || !(sc.x > 0.0f) || !(sc.y > 0.0f) || !(sc.z > 0.0f))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape_create: heightfield scale must be > 0");
    const u32 rows = d->rows, cols = d->cols, ns = rows * cols;
    for (u32 i = 0; i < ns; ++i)
        if (d->heights[i] != DE_HEIGHTFIELD_HOLE && !de__f32_finite(d->heights[i]))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape_create: height %u not finite", i);

    const u32 B = DE_HEIGHTFIELD_BLOCK;
    const u32 bx = (cols - 1u + B - 1u) / B, by = (rows - 1u + B - 1u) / B;
    const size_t nb = (size_t)bx * by;
    const size_t off_min = ((sizeof(f32) * (size_t)ns) + 15u) & ~(size_t)15u;
    const size_t off_max = off_min + ((sizeof(f32) * nb + 15u) & ~(size_t)15u);
    const size_t size = off_max + ((sizeof(f32) * nb + 15u) & ~(size_t)15u);
    u8 *block = (u8 *)DE_ALLOC(p->alloc, size, 16, DE_MEM_PHYSICS);
    if (block == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_shape_create: heightfield %ux%u", cols, rows);
    memset(block, 0, size);
    s->data = block;
    s->data_size = size;
    de__heightfield *hf = &s->u.hf;
    hf->rows = rows;
    hf->cols = cols;
    hf->scale = sc;
    hf->heights = (f32 *)block;
    hf->block_min = (f32 *)(block + off_min);
    hf->block_max = (f32 *)(block + off_max);
    hf->blocks_x = bx;
    hf->blocks_y = by;
    memcpy(hf->heights, d->heights, sizeof(f32) * ns);

    f32 zmin = FLT_MAX, zmax = -FLT_MAX;
    for (u32 j = 0; j < by; ++j) {
        for (u32 i = 0; i < bx; ++i) {
            f32 lo = FLT_MAX, hi = -FLT_MAX;
            const u32 c1 = (i + 1u) * B < cols - 1u ? (i + 1u) * B : cols - 1u;
            const u32 r1 = (j + 1u) * B < rows - 1u ? (j + 1u) * B : rows - 1u;
            for (u32 r = j * B; r < r1; ++r) {
                for (u32 c = i * B; c < c1; ++c) {
                    if (de__hf_cell_is_hole(hf, c, r))
                        continue;
                    const f32 z[4] = {de__hf_sample(hf, c, r) * sc.z,
                                      de__hf_sample(hf, c + 1u, r) * sc.z,
                                      de__hf_sample(hf, c, r + 1u) * sc.z,
                                      de__hf_sample(hf, c + 1u, r + 1u) * sc.z};
                    for (u32 k = 0; k < 4; ++k) {
                        lo = de__minf(lo, z[k]);
                        hi = de__maxf(hi, z[k]);
                    }
                }
            }
            hf->block_min[j * bx + i] = lo;
            hf->block_max[j * bx + i] = hi;
            zmin = de__minf(zmin, lo);
            zmax = de__maxf(zmax, hi);
        }
    }
    if (zmin > zmax) /* every cell is a hole */
        zmin = zmax = 0.0f;
    s->local_aabb.min = de__v3(0.0f, 0.0f, zmin);
    s->local_aabb.max = de__v3(de__f32_round_up((f64)(cols - 1u) * sc.x),
                               de__f32_round_up((f64)(rows - 1u) * sc.y), zmax);
    s->has_volume = false;
    f64 r2 = 0.0;
    for (u32 c = 0; c < 8; ++c) {
        const f64 x = (c & 1) ? s->local_aabb.max.x : s->local_aabb.min.x;
        const f64 y = (c & 2) ? s->local_aabb.max.y : s->local_aabb.min.y;
        const f64 z = (c & 4) ? s->local_aabb.max.z : s->local_aabb.min.z;
        r2 = de__maxd(r2, x * x + y * y + z * z);
    }
    s->bounding_radius = de__f32_round_up(de_dsqrt64(r2));
    return DE_OK;
}
