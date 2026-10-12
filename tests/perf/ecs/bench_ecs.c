/* bench_ecs.c: the ECS performance baseline (DENG-353; SPEC §19.5, §7).
 *
 * One world of N entities (default 1M) with TPos + TVel:
 *   spawn         de_entity_spawn_with of every entity, then the data filled through a query
 *   each          pos += vel * dt with de_query_each on the calling thread
 *   par_each      the same with de_query_par_each on a task system (grain 1024 and 16384)
 *   add/remove    100k THealth adds and 100k removes per frame (N / 10)
 *   spawn/despawn 100k despawns and 100k spawns per frame (N / 10)
 *   snapshot      take, restore (unchanged layout: the memcpy fast path) and de_world_checksum
 * It prints the best and median time of each step and ns per entity.
 *
 *   bench_ecs                    full run (1M entities; run it in a Release build)
 *   bench_ecs --quick            20k entities, few repetitions: the CTest entry `perf.ecs`
 *   bench_ecs --entities=N --workers=W --reps=R
 *
 * Exit code 1 when results disagree: each and par_each must give the same world checksum, and a
 * restore must give back the checksum at take. Timings never fail. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* sscanf */
#endif
#include <dengine/de_ecs.h>
#include <dengine/de_math.h>
#include <dengine/de_platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TPos {
    de_vec3 v;
} TPos;
typedef struct TVel {
    de_vec3 v;
} TVel;
typedef struct THealth {
    f32 current, max;
} THealth;

typedef struct bench {
    de_task_system *ts;
    de_type_registry *types;
    de_world *w;
    de_component pos, vel, health;
    de_query move;
    de_entity *ents;
    u32 n;
    u32 reps;
    u32 workers;
    int failures;
} bench;

#define MAX_REPS 64u
static const f32 k_dt = 1.0f / 60.0f;

/* ---- timing ----------------------------------------------------------------------------- */

typedef struct samples {
    f64 ms[MAX_REPS];
    u32 count;
} samples;

static void sample_add(samples *s, u64 t0) {
    if (s->count < MAX_REPS)
        s->ms[s->count++] = (f64)(de_time_now_ns() - t0) / 1e6;
}

static int cmp_f64(const void *a, const void *b) {
    const f64 x = *(const f64 *)a, y = *(const f64 *)b;
    return (x > y) - (x < y);
}

/* Prints best and median; `per` is the number of entities or operations one sample covers. */
static void report(const char *what, samples *s, u64 per) {
    if (s->count == 0)
        return;
    qsort(s->ms, s->count, sizeof s->ms[0], cmp_f64);
    const f64 best = s->ms[0], median = s->ms[s->count / 2];
    printf("  %-44s best %9.3f ms  median %9.3f ms  %8.2f ns/op\n", what, best, median,
           per > 0 ? best * 1e6 / (f64)per : 0.0);
}

/* ---- setup ------------------------------------------------------------------------------ */

static bool setup(bench *b) {
    de_fp_env_init_thread();
    de_task_system_desc td;
    memset(&td, 0, sizeof td);
    td.worker_count = b->workers;
    td.background_worker_count = 1;
    if (de_task_system_create(&td, &b->ts) != DE_OK)
        return false;
    if (de_type_registry_create(NULL, &b->types) != DE_OK)
        return false;
    static const de_field_info k_pos[] = {DE_FIELD(TPos, v, DE_FK_VEC3, DE_FIELD_DEFAULT)};
    static const de_field_info k_vel[] = {DE_FIELD(TVel, v, DE_FK_VEC3, DE_FIELD_DEFAULT)};
    static const de_field_info k_health[] = {
        DE_FIELD(THealth, current, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(THealth, max, DE_FK_F32, DE_FIELD_DEFAULT),
    };
    const de_type_id t_pos = de_type_register(b->types, &DE_TYPE_DESC(TPos, 1, k_pos));
    const de_type_id t_vel = de_type_register(b->types, &DE_TYPE_DESC(TVel, 1, k_vel));
    const de_type_id t_health = de_type_register(b->types, &DE_TYPE_DESC(THealth, 1, k_health));

    de_world_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.types = b->types;
    wd.tasks = b->ts;
    wd.initial_entity_capacity = b->n + b->n / 8u;
    if (de_world_create(&wd, &b->w) != DE_OK)
        return false;
    b->pos = de_component_register(b->w, t_pos, DE_COMP_SNAPSHOT);
    b->vel = de_component_register(b->w, t_vel, DE_COMP_SNAPSHOT);
    b->health = de_component_register(b->w, t_health, DE_COMP_SNAPSHOT);
    const de_query_term terms[2] = {{b->pos, DE_WRITE}, {b->vel, DE_READ}};
    const de_query_desc qd = {terms, 2, NULL, 0};
    b->move = de_query_create(b->w, &qd);
    b->ents = (de_entity *)calloc(b->n, sizeof(de_entity));
    return DE_HANDLE_VALID(b->pos) && DE_HANDLE_VALID(b->vel) && DE_HANDLE_VALID(b->health) &&
           DE_HANDLE_VALID(b->move) && b->ents != NULL;
}

static void teardown(bench *b) {
    free(b->ents);
    de_world_destroy(b->w);
    de_type_registry_destroy(b->types);
    de_task_system_destroy(b->ts);
}

/* ---- the move kernel -------------------------------------------------------------------- */

static void move_chunk(const de_query_chunk *ch, void *user) {
    (void)user;
    TPos *p = (TPos *)ch->columns[0];
    const TVel *v = (const TVel *)ch->columns[1];
    const u32 n = ch->count;
    for (u32 i = 0; i < n; ++i) {
        p[i].v.x += v[i].v.x * k_dt;
        p[i].v.y += v[i].v.y * k_dt;
        p[i].v.z += v[i].v.z * k_dt;
    }
}

/* Start values from the row number: the same world for every pass. */
static void fill_chunk(const de_query_chunk *ch, void *user) {
    (void)user;
    TPos *p = (TPos *)ch->columns[0];
    TVel *v = (TVel *)ch->columns[1];
    for (u32 i = 0; i < ch->count; ++i) {
        const u32 r = ch->first_row + i;
        p[i].v.x = (f32)(r % 1000u);
        p[i].v.y = (f32)(r % 777u);
        p[i].v.z = (f32)(r % 13u);
        v[i].v.x = (f32)(r % 7u) - 3.0f;
        v[i].v.y = (f32)(r % 5u) - 2.0f;
        v[i].v.z = 1.0f;
    }
}

static void fill(bench *b) {
    const de_query_term terms[2] = {{b->pos, DE_WRITE}, {b->vel, DE_WRITE}};
    const de_query_desc qd = {terms, 2, NULL, 0};
    const de_query q = de_query_create(b->w, &qd);
    de_query_each(b->w, q, fill_chunk, NULL);
    de_query_destroy(b->w, q);
}

/* ---- steps ------------------------------------------------------------------------------ */

static void bench_spawn(bench *b) {
    samples s = {{0}, 0};
    const de_component comps[2] = {b->pos, b->vel};
    const u64 t0 = de_time_now_ns();
    for (u32 i = 0; i < b->n; ++i) {
        if (de_entity_spawn_with(b->w, comps, 2, &b->ents[i]) != DE_OK) {
            printf("spawn failed at %u\n", i);
            ++b->failures;
            return;
        }
    }
    sample_add(&s, t0);
    report("spawn_with (TPos+TVel)", &s, b->n);
    samples f = {{0}, 0};
    const u64 t1 = de_time_now_ns();
    fill(b);
    sample_add(&f, t1);
    report("fill through de_query_each", &f, b->n);
}

static u64 bench_move(bench *b, const char *what, bool parallel, u32 grain) {
    fill(b);
    samples s = {{0}, 0};
    for (u32 r = 0; r < b->reps; ++r) {
        const u64 t0 = de_time_now_ns();
        if (parallel)
            de_query_par_each(b->w, b->move, grain, move_chunk, NULL);
        else
            de_query_each(b->w, b->move, move_chunk, NULL);
        sample_add(&s, t0);
    }
    report(what, &s, b->n);
    return de_world_checksum(b->w, DE_COMP_SNAPSHOT);
}

static void bench_add_remove(bench *b) {
    const u32 batch = b->n / 10u;
    const u32 frames = b->reps < 10u ? b->reps : 10u;
    samples s = {{0}, 0};
    u32 prev = 0, cur = 0;
    for (u32 f = 0; f <= frames; ++f) {
        const u64 t0 = de_time_now_ns();
        if (f > 0) {
            for (u32 i = 0; i < batch; ++i)
                de_entity_remove(b->w, b->ents[(prev + i) % b->n], b->health);
        }
        if (f < frames) {
            for (u32 i = 0; i < batch; ++i) {
                THealth *h = (THealth *)de_entity_add(b->w, b->ents[(cur + i) % b->n], b->health);
                if (h != NULL)
                    h->current = h->max = 100.0f;
            }
        }
        if (f > 0 && f < frames)
            sample_add(&s, t0);
        prev = cur;
        cur = (cur + batch * 3u + 1u) % b->n;
    }
    char what[96];
    snprintf(what, sizeof what, "add+remove THealth, %u each per frame", batch);
    report(what, &s, 2u * (u64)batch);
}

static void bench_spawn_despawn(bench *b) {
    const u32 batch = b->n / 10u;
    const u32 frames = b->reps < 10u ? b->reps : 10u;
    const de_component comps[2] = {b->pos, b->vel};
    samples s = {{0}, 0};
    u32 cur = 0;
    for (u32 f = 0; f < frames; ++f) {
        const u64 t0 = de_time_now_ns();
        for (u32 i = 0; i < batch; ++i)
            de_entity_despawn(b->w, b->ents[(cur + i) % b->n]);
        for (u32 i = 0; i < batch; ++i) {
            if (de_entity_spawn_with(b->w, comps, 2, &b->ents[(cur + i) % b->n]) != DE_OK) {
                ++b->failures;
                return;
            }
        }
        sample_add(&s, t0);
        cur = (cur + batch) % b->n;
    }
    char what[96];
    snprintf(what, sizeof what, "despawn+spawn, %u each per frame", batch);
    report(what, &s, 2u * (u64)batch);
}

static void bench_snapshot(bench *b) {
    fill(b);
    const u64 before = de_world_checksum(b->w, DE_COMP_SNAPSHOT);
    samples take = {{0}, 0}, restore = {{0}, 0}, sum = {{0}, 0};
    de_world_snapshot snap;
    memset(&snap, 0, sizeof snap);
    const u32 reps = b->reps < 10u ? b->reps : 10u;
    for (u32 r = 0; r < reps; ++r) {
        de_world_snapshot_free(NULL, &snap);
        u64 t0 = de_time_now_ns();
        if (de_world_snapshot_take(b->w, NULL, &snap) != DE_OK) {
            printf("snapshot take failed\n");
            ++b->failures;
            return;
        }
        sample_add(&take, t0);
        de_query_each(b->w, b->move, move_chunk, NULL); /* change data, not layout */
        t0 = de_time_now_ns();
        if (de_world_snapshot_restore(b->w, &snap) != DE_OK) {
            printf("snapshot restore failed\n");
            ++b->failures;
            break;
        }
        sample_add(&restore, t0);
        t0 = de_time_now_ns();
        const u64 after = de_world_checksum(b->w, DE_COMP_SNAPSHOT);
        sample_add(&sum, t0);
        if (after != before) {
            printf("restore checksum %016llx != take checksum %016llx\n", (unsigned long long)after,
                   (unsigned long long)before);
            ++b->failures;
        }
    }
    printf("  snapshot size: %.1f MiB\n", (f64)snap.size / (1024.0 * 1024.0));
    de_world_snapshot_free(NULL, &snap);
    report("snapshot take", &take, b->n);
    report("snapshot restore (memcpy fast path)", &restore, b->n);
    report("de_world_checksum(DE_COMP_SNAPSHOT)", &sum, b->n);
}

static u32 arg_u32(const char *arg, const char *name, u32 fallback) {
    const size_t len = strlen(name);
    unsigned v;
    if (strncmp(arg, name, len) == 0 && sscanf(arg + len, "%u", &v) == 1)
        return (u32)v;
    return fallback;
}

int main(int argc, char **argv) {
    bench b;
    memset(&b, 0, sizeof b);
    b.n = 1000000u;
    b.reps = 15u;
    const u32 cores = de_cpu_logical_cores();
    b.workers = cores > 1u ? cores - 1u : 1u;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--quick") == 0) {
            b.n = 20000u;
            b.reps = 2u;
        }
        b.n = arg_u32(argv[i], "--entities=", b.n);
        b.workers = arg_u32(argv[i], "--workers=", b.workers);
        b.reps = arg_u32(argv[i], "--reps=", b.reps);
    }
    if (b.n < 10u)
        b.n = 10u;
    if (b.reps < 1u)
        b.reps = 1u;
    if (b.reps > MAX_REPS)
        b.reps = MAX_REPS;

    printf("bench_ecs: %u entities, %u reps, %u workers + main thread\n", b.n, b.reps, b.workers);
    if (!setup(&b)) {
        printf("setup failed\n");
        teardown(&b);
        return 1;
    }
    bench_spawn(&b);
    const u64 c_each = bench_move(&b, "pos += vel*dt, de_query_each (1 thread)", false, 0);
    const u64 c_par = bench_move(&b, "pos += vel*dt, de_query_par_each grain 1024", true, 1024u);
    const u64 c_par16 =
        bench_move(&b, "pos += vel*dt, de_query_par_each grain 16384", true, 16384u);
    if (c_each != c_par || c_each != c_par16) {
        printf("each and par_each disagree: %016llx %016llx %016llx\n", (unsigned long long)c_each,
               (unsigned long long)c_par, (unsigned long long)c_par16);
        ++b.failures;
    }
    bench_add_remove(&b);
    bench_spawn_despawn(&b);
    bench_snapshot(&b);
    teardown(&b);
    if (b.failures != 0) {
        printf("bench_ecs: %d failure(s)\n", b.failures);
        return 1;
    }
    return 0;
}
