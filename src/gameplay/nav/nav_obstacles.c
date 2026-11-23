/*
 * nav_obstacles.c: dynamic obstacles (DENG-77, SPEC §21.2). Each de_nav_update checks a few
 * obstacles, from a rotating cursor, and queues a rebuild of the tiles of any obstacle that was
 * added, resized, removed or despawned, or that moved or turned since its tiles were last built.
 *
 * Choice (DENG-77): obstacles carve by rebuilding their tiles through the normal Recast pipeline
 * (background task, applied in order by de_nav_update), not with DetourTileCache. TileCache would
 * need a second, layered build pipeline and compressed layer storage beside the cooked DNAV
 * tiles; a tile rebuild costs more per change, so the update step limits rebuilds per frame and
 * ignores moves under two cells.
 */
#include "gameplay/nav/nav_internal.h"

#include <dengine/de_base.h>

#include <string.h>

/** Obstacles whose tiles are queued for rebuild per de_nav_update. */
#define DE__NAV_OBSTACLE_BUDGET 4u

static bool pose_moved(const de__nav_settings *st, const de__nav_pose *a, const de__nav_pose *b) {
    const f64 lim = 2.0 * (f64)st->desc.cell_size;
    const f64 dx = a->pos.x - b->pos.x, dy = a->pos.y - b->pos.y, dz = a->pos.z - b->pos.z;
    if (dx > lim || dx < -lim || dy > lim || dy < -lim || dz > lim || dz < -lim)
        return true;
    f64 dot = a->q[0] * b->q[0] + a->q[1] * b->q[1] + a->q[2] * b->q[2] + a->q[3] * b->q[3];
    if (dot < 0.0)
        dot = -dot;
    return dot < 0.9999; /* about 1.6 degrees */
}

static void union_range(i32 *r, i32 tx0, i32 tz0, i32 tx1, i32 tz1, bool *have) {
    if (!*have) {
        r[0] = tx0;
        r[1] = tz0;
        r[2] = tx1;
        r[3] = tz1;
        *have = true;
        return;
    }
    r[0] = tx0 < r[0] ? tx0 : r[0];
    r[1] = tz0 < r[1] ? tz0 : r[1];
    r[2] = tx1 > r[2] ? tx1 : r[2];
    r[3] = tz1 > r[3] ? tz1 : r[3];
}

void de__nav_update_obstacles(de_nav *n) {
    if (n->obstacle_count == 0 || n->mesh == NULL || !n->has_settings || !n->has_origin)
        return;
    const u32 count = n->obstacle_count;
    u32 queued = 0, visited = 0;
    bool drop_any = false;
    for (; visited < count && queued < DE__NAV_OBSTACLE_BUDGET; ++visited) {
        de__nav_obstacle *o = &n->obstacles[(n->obstacle_cursor + visited) % count];
        de__nav_pose p;
        const bool has = !o->removed && n->world != NULL &&
                         de__nav_entity_pose(n->world, o->entity, &p);
        bool need;
        if (!has)
            need = o->carved; /* removed or despawned: uncarve */
        else
            need = o->dirty || !o->carved || pose_moved(&n->settings, &o->carved_pose, &p) ||
                   o->carved_half.x != o->half_extents.x ||
                   o->carved_half.y != o->half_extents.y || o->carved_half.z != o->half_extents.z;
        if (!need) {
            if (o->removed)
                drop_any = true;
            continue;
        }
        i32 r[4] = {0, 0, 0, 0};
        bool have = false;
        i32 a0, b0, a1, b1;
        if (o->carved) {
            de__nav_obstacle_tiles(n, &o->carved_pose, o->carved_half, &a0, &b0, &a1, &b1);
            union_range(r, a0, b0, a1, b1, &have);
        }
        if (has) {
            de__nav_obstacle_tiles(n, &p, o->half_extents, &a0, &b0, &a1, &b1);
            union_range(r, a0, b0, a1, b1, &have);
        }
        /* The rebuild's input gathering marks a present obstacle carved at its pose. */
        if (de__nav_rebuild_range(n, &n->settings, r[0], r[1], r[2], r[3]) != DE_OK) {
            DE_WARN(n->log, "nav: obstacle rebuild not queued: %s",
                    de_error_last() != NULL ? de_error_last()->message : "error");
            o->dirty = false; /* do not retry every frame; a later change retries */
            if (has) {
                o->carved = true;
                o->carved_pose = p;
                o->carved_half = o->half_extents;
            }
        }
        if (!has) {
            o->carved = false;
            if (o->removed)
                drop_any = true;
        }
        ++queued;
    }
    n->obstacle_cursor = (n->obstacle_cursor + visited) % count;
    if (!drop_any)
        return;
    /* Drop removed obstacles whose tiles are rebuilt (or were never carved), keeping order. */
    u32 w = 0;
    for (u32 i = 0; i < count; ++i) {
        const de__nav_obstacle *o = &n->obstacles[i];
        if (o->removed && !o->carved)
            continue;
        if (w != i)
            n->obstacles[w] = *o;
        ++w;
    }
    n->obstacle_count = w;
    n->obstacle_cursor = w != 0 ? n->obstacle_cursor % w : 0;
}
