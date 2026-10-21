#include "anim_internal.h"
#include <stdlib.h>

u64 de__anim_pose_checksum(const de__anim_local_pose *pose) {
    u64 hash = de_hash64(&pose->count, sizeof pose->count, 0);
    for (u32 j = 0; j < pose->count; ++j) {
        de_quat q = pose->rotations[j];
        de_vec3 t = pose->translations[j], s = pose->scales[j];
        const f32 values[] = {q.x, q.y, q.z, q.w, t.x, t.y, t.z, s.x, s.y, s.z};
        hash = de_hash64(values, sizeof values, hash);
    }
    const f32 root[] = {pose->root_translation.x, pose->root_translation.y,
                        pose->root_translation.z, pose->root_rotation.x,
                        pose->root_rotation.y,    pose->root_rotation.z,
                        pose->root_rotation.w};
    return de_hash64(root, sizeof root, hash);
}
typedef struct checksum_sink {
    u64 *rows;
    u32 count;
} checksum_sink;
static void collect(const de_query_chunk *chunk, void *user) {
    checksum_sink *sink = user;
    const DeAnimator *animators = chunk->columns[0];
    const de__anim_clock *clocks = chunk->columns[1];
    for (u32 i = 0; i < chunk->count; ++i) {
        if (!animators[i].deterministic)
            continue;
        u64 *row = &sink->rows[(size_t)4 * sink->count++];
        row[0] = chunk->entities[i].bits;
        row[1] = clocks ? clocks[i].ticks : 0;
        row[2] = clocks ? clocks[i].state_id : 0;
        row[3] = clocks ? clocks[i].pose_hash : 0;
    }
}
static int compare(const void *a, const void *b) {
    u32 left = (u32) * (const u64 *)a, right = (u32) * (const u64 *)b;
    return (left > right) - (left < right);
}
u64 de_anim_checksum(de_world *world) {
    de_anim_system *s = de__anim_for_world(world);
    if (!s)
        return 0;
    u32 count = de_query_count(world, s->checksum_query);
    if (count > s->checksum_capacity) {
        u64 *rows = DE_ALLOC(s->alloc, (size_t)count * 4 * sizeof(u64), 8, DE_MEM_GENERAL);
        if (!rows) {
            (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "animation checksum scratch");
            return 0;
        }
        if (s->checksum_rows)
            DE_FREE(s->alloc, s->checksum_rows, (size_t)s->checksum_capacity * 4 * sizeof(u64));
        s->checksum_rows = rows;
        s->checksum_capacity = count;
    }
    checksum_sink sink = {s->checksum_rows, 0};
    de_query_each(world, s->checksum_query, collect, &sink);
    if (sink.count > 1)
        qsort(sink.rows, sink.count, 4 * sizeof(u64), compare);
    const u32 header[] = {0x314d4e41u, sink.count}; /* ANM1, count; supported targets are LE. */
    u64 hash = de_hash64(header, sizeof header, 0);
    for (u32 i = 0; i < sink.count; ++i)
        hash = de_hash64(&sink.rows[(size_t)4 * i], 4 * sizeof(u64), hash);
    return hash;
}
