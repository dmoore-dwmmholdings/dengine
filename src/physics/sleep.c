/*
 * sleep.c: sleep timers, putting islands to sleep, the body callbacks and the wake API of the 3D
 * physics world (DENG-218, SPEC §13.1; see islands.h).
 *
 * After each step (de__islands_finalize -> de__sleep_finalize):
 *   1. Timers, per awake dynamic body, in fixed ranges of solver slots on p->tasks: + dt while the
 *      body allows sleep and |v| < linear threshold and |w| < angular threshold, else 0.
 *   2. Island evaluation, in fixed ranges of islands on p->tasks: smallest timer, whether every
 *      body allows sleep. Each range writes only its own islands.
 *   3. Serially, in ascending island id: an island whose bodies all allow sleep, whose smallest
 *      timer reached physics.sleep.time and that no moving kinematic body touches goes to sleep:
 *      its bodies get zero velocities, keep their pose (remembered, so a user change wakes them)
 *      and leave the solver from the next step on.
 *
 * Determinism (SPEC §5.7): comparisons and + only; fixed ranges, no shared writes, serial
 * decisions in island order.
 */
#include "physics/islands.h"

#include "physics/solver/graph_color.h"
#include "physics/solver/solver_internal.h"

#include <dengine/de_debug.h>
#include <dengine/de_task.h>

/* ================================ Settings ============================================= */

static de_cvar sleep_cvar(const char *name, const char *help, f64 def) {
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.help = help;
    d.type = DE_CVAR_FLOAT;
    d.default_number = def;
    d.min = 0.0;
    d.max = 1.0e6;
    return de_cvar_register(&d);
}

static f32 sleep_value(de_cvar cv, f32 def) {
    if (!DE_HANDLE_VALID(cv))
        return def;
    const f32 v = (f32)de_cvar_float(cv);
    return de__f32_finite(v) && v >= 0.0f ? v : def;
}

/* Registration takes the cvar registry lock: only when the solver is created. */
void de__sleep_read_settings(de__islands *is) {
    const de_cvar lin = sleep_cvar(DE__SLEEP_LINEAR_CVAR,
                                   "Physics: a body slower than this (m/s) may fall asleep (read "
                                   "when the solver is created)",
                                   (f64)DE__SLEEP_DEFAULT_LINEAR);
    const de_cvar ang = sleep_cvar(DE__SLEEP_ANGULAR_CVAR,
                                   "Physics: a body turning slower than this (rad/s) may fall "
                                   "asleep (read when the solver is created)",
                                   (f64)DE__SLEEP_DEFAULT_ANGULAR);
    const de_cvar time = sleep_cvar(DE__SLEEP_TIME_CVAR,
                                    "Physics: seconds an island must rest before it sleeps (read "
                                    "when the solver is created)",
                                    (f64)DE__SLEEP_DEFAULT_TIME);
    is->sleep_linear = sleep_value(lin, DE__SLEEP_DEFAULT_LINEAR);
    is->sleep_angular = sleep_value(ang, DE__SLEEP_DEFAULT_ANGULAR);
    is->sleep_time = sleep_value(time, DE__SLEEP_DEFAULT_TIME);
}

/* ================================ After the step ======================================= */

static void timers_range(de__solver *s, u32 begin, u32 end) {
    const de_physics_world *p = s->p;
    const de__body_pool *b = &p->bodies;
    de__islands *is = s->islands;
    const f32 lin2 = is->sleep_linear * is->sleep_linear;
    const f32 ang2 = is->sleep_angular * is->sleep_angular;
    const f32 dt = s->ctx.dt;
    for (u32 k = begin; k < end; ++k) {
        const de__solver_body *sb = &s->bodies[k];
        if ((sb->flags & DE__SOLVER_BODY_DYNAMIC) == 0u)
            continue;
        const u32 i = sb->body;
        const de_vec3 v = b->linear_velocity[i], w = b->angular_velocity[i];
        const bool slow = de__v3_dot(v, v) < lin2 && de__v3_dot(w, w) < ang2;
        if ((b->flags[i] & DE_BODY_FLAG_ALLOW_SLEEP) != 0u && slow)
            is->timer[i] += dt;
        else
            is->timer[i] = 0.0f;
    }
}

typedef struct sl_eval {
    const de_physics_world *p;
    de__islands *is;
} sl_eval;

static void eval_range(u32 begin, u32 end, void *user) {
    const sl_eval *e = (const sl_eval *)user;
    de__islands *is = e->is;
    const de__body_pool *b = &e->p->bodies;
    for (u32 n = begin; n < end; ++n) {
        de__island *I = &is->islands[n];
        f32 lo = 3.0e38f;
        for (u32 j = 0; j < I->body_count; ++j) {
            const u32 i = is->island_bodies[I->first + j];
            lo = de__minf(lo, is->timer[i]);
            if ((b->flags[i] & DE_BODY_FLAG_ALLOW_SLEEP) == 0u)
                I->flags |= DE__ISLAND_NO_SLEEP;
        }
        I->min_sleep_time = lo;
    }
}

static void island_sleep(de_physics_world *p, de__islands *is, de__island *I) {
    de__body_pool *b = &p->bodies;
    const de_vec3 zero = {0.0f, 0.0f, 0.0f};
    for (u32 j = 0; j < I->body_count; ++j) {
        const u32 i = is->island_bodies[I->first + j];
        is->sleeping[i] = 1u;
        is->sleep_island[i] = I->id;
        b->linear_velocity[i] = zero;
        b->angular_velocity[i] = zero;
        is->rest_position[i] = b->position[i];
        is->rest_rotation[i] = b->rotation[i];
    }
    I->flags |= DE__ISLAND_SLEPT;
    is->stats.sleeping_bodies += I->body_count;
    ++is->stats.slept_islands;
}

void de__sleep_finalize(de_physics_world *p, de__solver *s) {
    de__islands *is = s->islands;
    de__solver_for_bodies(s, timers_range);
    sl_eval e = {p, is};
    const u32 n = is->island_count;
    const u32 grain = s->coloring->body_grain > 0u ? s->coloring->body_grain : 1u;
    if (p->tasks != NULL && n > grain)
        de_task_parallel_for(p->tasks, n, grain, eval_range, &e);
    else
        eval_range(0, n, &e);
    const u32 blocked = DE__ISLAND_NO_SLEEP | DE__ISLAND_KEEP_AWAKE;
    for (u32 k = 0; k < n; ++k) {
        de__island *I = &is->islands[k];
        if ((I->flags & blocked) == 0u && I->min_sleep_time >= is->sleep_time)
            island_sleep(p, is, I);
    }
}

/* ================================ Body callbacks ======================================= */

void de__islands_body_added(de__islands *is, u32 index) {
    if (index >= is->capacity)
        return;
    DE_ASSERT(!is->sleeping[index]);
    is->sleeping[index] = 0u;
    is->sleep_island[index] = DE__ISLAND_NONE;
    is->timer[index] = 0.0f;
}

void de__islands_body_removed(de__islands *is, u32 index) {
    if (index >= is->capacity)
        return;
    if (is->sleeping[index]) {
        /* The rest of its island lost a support (or a passenger): wake it next step. */
        de__islands_request_wake(is, index);
        is->sleeping[index] = 0u;
        DE_ASSERT(is->stats.sleeping_bodies > 0u);
        --is->stats.sleeping_bodies;
    }
    is->sleep_island[index] = DE__ISLAND_NONE;
    is->timer[index] = 0.0f;
}

/* ================================ Wake API ============================================= */

de__islands *de__islands_get(const de_physics_world *p) {
    const de__solver *s = p != NULL ? de__solver_get(p) : NULL;
    return s != NULL ? s->islands : NULL;
}

void de__islands_wake_body(de_physics_world *p, u32 index) {
    de__islands *is = de__islands_get(p);
    if (is == NULL || !de__body_alive(p, index))
        return;
    de__islands_request_wake(is, index);
}

bool de__islands_body_sleeping(const de_physics_world *p, u32 index) {
    const de__islands *is = de__islands_get(p);
    if (is == NULL || !de__body_alive(p, index) || index >= is->capacity || !is->sleeping[index])
        return false;
    return is->wake_mark[is->sleep_island[index]] == 0u;
}

void de__islands_stats_get(const de_physics_world *p, de__islands_stats *out) {
    const de__islands *is = de__islands_get(p);
    if (is == NULL)
        memset(out, 0, sizeof *out);
    else
        *out = is->stats;
}

void de_body_wake(de_physics_world *p, de_body b) {
    u32 i;
    if (de__body_resolve(p, b, &i))
        de__islands_wake_body(p, i);
}

bool de_body_is_sleeping(const de_physics_world *p, de_body b) {
    u32 i;
    return de__body_resolve(p, b, &i) && de__islands_body_sleeping(p, i);
}
