#include "gpu/gpu_accel_internal.h"

de_result de__null_accel_sizes(de_gpu_device *d, const de_gpu_accel_desc *x,
                               de_gpu_accel_sizes *s) {
    (void)d;
    u64 primitives = x->is_tlas ? x->max_instances : 0;
    for (u32 i = 0; i < x->geometry_count; ++i)
        primitives += (x->geometries[i].indices.id ? x->geometries[i].index_count
                                                   : x->geometries[i].vertex_count) /
                      3;
    s->accel_bytes = (256 + primitives * 128 + 255) & ~(u64)255;
    s->build_scratch_bytes = s->accel_bytes;
    s->update_scratch_bytes = x->allow_update ? s->accel_bytes / 2 : 0;
    return DE_OK;
}
de_result de__null_accel_create(de_gpu_device *d, de__gpu_accel *a, const de_gpu_accel_desc *x) {
    (void)d;
    (void)a;
    (void)x;
    return DE_OK;
}
void de__null_accel_build_blas(de_gpu_cmd *c, de__gpu_accel *a, bool update) {
    (void)c;
    (void)update;
    a->built = true;
}
void de__null_accel_build_tlas(de_gpu_cmd *c, de__gpu_accel *a, de__gpu_buffer *b, u32 n, bool u) {
    (void)c;
    (void)b;
    (void)n;
    (void)u;
    a->built = true;
}
void de__null_accel_emit(de_gpu_cmd *c, de__gpu_accel *a) {
    (void)c;
    (void)a;
}
bool de__null_accel_read(de_gpu_device *d, de__gpu_accel *a, u64 *bytes) {
    (void)d;
    *bytes = (a->sizes.accel_bytes * 3 / 4 + 255) & ~(u64)255;
    return true;
}
void de__null_accel_compact(de_gpu_cmd *c, de__gpu_accel *a, de__gpu_accel *b) {
    (void)c;
    (void)a;
    b->built = true;
}
