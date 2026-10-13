/*
 * checksum2d.c: de_physics2d_checksum (DENG-252). DENG-253 adds the contact state.
 *
 * XXH3-64 (de_hash64) over an explicit little-endian byte encoding of the state, never raw struct
 * bytes (no padding leaks in): a header (tick, gravity, substeps, plane, live body count), then
 * one record per live body in ascending index order. Each record is hashed with the running value
 * as the seed. Floats are hashed by their bit patterns.
 */
#include "physics/2d/p2d_internal.h"

typedef struct ck_buf {
    u8 b[256];
    u32 n;
} ck_buf;

static void ck_u32(ck_buf *c, u32 v) {
    DE_ASSERT(c->n + 4 <= sizeof c->b);
    for (u32 i = 0; i < 4; ++i)
        c->b[c->n++] = (u8)(v >> (8u * i));
}
static void ck_u64(ck_buf *c, u64 v) {
    DE_ASSERT(c->n + 8 <= sizeof c->b);
    for (u32 i = 0; i < 8; ++i)
        c->b[c->n++] = (u8)(v >> (8u * i));
}
static void ck_f32(ck_buf *c, f32 v) {
    u32 u;
    memcpy(&u, &v, sizeof u);
    ck_u32(c, u);
}
static void ck_f64(ck_buf *c, f64 v) {
    u64 u;
    memcpy(&u, &v, sizeof u);
    ck_u64(c, u);
}
static void ck_v2(ck_buf *c, de_vec2 v) {
    ck_f32(c, v.x);
    ck_f32(c, v.y);
}

u64 de_physics2d_checksum(const de_physics2d_world *p) {
    if (p == NULL)
        return 0;
    const de__body2d_pool *b = &p->bodies;
    ck_buf c;
    c.n = 0;
    ck_u64(&c, p->tick);
    ck_v2(&c, p->gravity);
    ck_u32(&c, p->substeps);
    ck_u32(&c, (u32)p->plane);
    ck_u32(&c, b->live_count);
    u64 h = de_hash64(c.b, c.n, 0);

    for (u32 i = 0; i < b->high_water; ++i) {
        if (!b->alive[i])
            continue;
        c.n = 0;
        ck_u32(&c, i);
        ck_u32(&c, b->generation[i]);
        ck_u32(&c, b->type[i]);
        ck_u32(&c, b->flags[i]);
        ck_f64(&c, b->position[i].x);
        ck_f64(&c, b->position[i].y);
        ck_f32(&c, b->rotation[i].c);
        ck_f32(&c, b->rotation[i].s);
        ck_v2(&c, b->linear_velocity[i]);
        ck_f32(&c, b->angular_velocity[i]);
        ck_f32(&c, b->mass[i]);
        ck_f32(&c, b->inv_mass[i]);
        ck_f32(&c, b->inertia[i]);
        ck_f32(&c, b->inv_inertia[i]);
        ck_v2(&c, b->local_com[i]);
        ck_f32(&c, b->linear_damping[i]);
        ck_f32(&c, b->angular_damping[i]);
        ck_f32(&c, b->gravity_scale[i]);
        ck_u32(&c, b->shape[i].id);
        ck_u32(&c, b->filter[i].layer);
        ck_u32(&c, b->filter[i].mask);
        ck_u32(&c, b->filter[i].group);
        ck_f32(&c, b->material[i].friction);
        ck_f32(&c, b->material[i].restitution);
        ck_f32(&c, b->material[i].density);
        ck_u64(&c, b->owner[i].bits);
        ck_v2(&c, b->world_aabb[i].min);
        ck_v2(&c, b->world_aabb[i].max);
        h = de_hash64(c.b, c.n, h);
    }
    return h;
}
