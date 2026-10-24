#ifndef DE_ANIM_BLEND_TREE_H
#define DE_ANIM_BLEND_TREE_H
#include <dengine/de_math.h>
#include <dengine/de_memory.h>

typedef struct de__anim_blend_point {
    de_vec2 position;
    u32 node;
} de__anim_blend_point;
typedef struct de__anim_blend_triangle {
    u32 point[3]; /* indices into the space's spatially sorted point array */
} de__anim_blend_triangle;
typedef struct de__anim_blend_weights {
    u32 count;
    u32 node[3];
    f32 weight[3];
} de__anim_blend_weights;
typedef struct de__anim_blend_space {
    de_allocator allocator;
    size_t bytes;
    u32 dimensions, point_count, triangle_count, hull_count;
    bool collinear;
    de__anim_blend_point *points;
    de__anim_blend_triangle *triangles;
    u32 *hull;
} de__anim_blend_space;
/* Build-time allocation only. Copies input; nodes remain caller-defined graph
 * indices. 1D uses x, 2D uses both axes. Nonfinite/coincident points are invalid.
 * Collinear 2D spaces interpolate adjacent points on their common line. */
de_result de__anim_blend_space_create(const de_allocator *, u32 dimensions,
                                      const de__anim_blend_point *, u32 count,
                                      de__anim_blend_space **out);
void de__anim_blend_space_destroy(de__anim_blend_space *);
/* Allocation-free and read-only; safe for concurrent graph evaluations. Invalid
 * queries leave out untouched. Returns at most 3 nonzero weights. */
bool de__anim_blend_space_weights(const de__anim_blend_space *, de_vec2 query,
                                  de__anim_blend_weights *out);
#endif
