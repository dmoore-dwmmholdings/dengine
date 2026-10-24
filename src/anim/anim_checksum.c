#include "anim/graph_internal.h"
#include "asset/slots_internal.h"
#include <stdlib.h>
#include <string.h>

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
/* Single-clip rows retain ANM1 byte identity. Graph rows additionally cover their
 * pending parameters, authored controls and exact clock, without process-local IDs. */
static void graph_word(u64 *hash, u64 word) {
    *hash = de_hash64(&word, sizeof word, *hash);
}
static void graph_float(u64 *hash, f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof bits);
    graph_word(hash, bits);
}
static u64 graph_state(const de_anim_system *s, de_entity entity, const DeAnimator *a,
                       const de__anim_clock *clock, u64 pose) {
    const de__anim_graph *g = de__anim_graph_get(s, a->graph);
    const de__slot *skeleton = s->assets ? de__slot_get(s->assets, a->skeleton.id) : NULL;
    u64 hash = pose;
    graph_word(&hash, UINT64_C(0x3154534850415247)); /* GRAPHST1 */
    graph_word(&hash, g ? g->definition_hash : 0);
    graph_word(&hash, skeleton ? skeleton->id.hash : 0);
    graph_float(&hash, a->speed);
    graph_float(&hash, a->time);
    graph_word(&hash, a->playing);
    graph_word(&hash, a->root_motion);
    if (clock) {
        graph_word(&hash, clock->segment_ticks);
        hash = de_hash64(&clock->phase, sizeof clock->phase, hash);
        hash = de_hash64(&clock->exact_time, sizeof clock->exact_time, hash);
        graph_float(&hash, clock->dt);
        graph_float(&hash, clock->speed);
        graph_float(&hash, clock->output_time);
        graph_word(&hash, clock->playing);
        graph_word(&hash, clock->initialized);
    }
    const DeAnimParams *p = de_entity_get_const(s->world, entity, s->params_component);
    graph_word(&hash, p ? p->count : 0);
    if (p) {
        graph_word(&hash, p->bools);
        graph_word(&hash, p->triggers);
        for (u32 i = 0; i < p->count && i < DE_ANIM_MAX_PARAMS; ++i) {
            graph_word(&hash, p->names[i].hash);
            graph_word(&hash, p->kinds[i]);
            if (p->kinds[i] == DE_ANIM_PARAM_FLOAT)
                graph_float(&hash, p->values[i]);
        }
    }
    return de__anim_graph_sync_hash(s, entity, g, hash);
}
typedef struct checksum_sink {
    de_anim_system *system;
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
        if (animators[i].graph.id)
            row[3] = graph_state(sink->system, chunk->entities[i], &animators[i],
                                 clocks ? &clocks[i] : NULL, row[3]);
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
    checksum_sink sink = {s, s->checksum_rows, 0};
    de_query_each(world, s->checksum_query, collect, &sink);
    if (sink.count > 1)
        qsort(sink.rows, sink.count, 4 * sizeof(u64), compare);
    const u32 header[] = {0x314d4e41u, sink.count}; /* ANM1, count; supported targets are LE. */
    u64 hash = de_hash64(header, sizeof header, 0);
    for (u32 i = 0; i < sink.count; ++i)
        hash = de_hash64(&sink.rows[(size_t)4 * i], 4 * sizeof(u64), hash);
    return hash;
}
