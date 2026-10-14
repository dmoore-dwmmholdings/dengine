/* test_rg_execute.c: render graph record + submit (DENG-173): the fixed partition and parallel
 * recording on the task system, per-pass markers, timestamps and breadcrumbs (null backend
 * command log), culled passes, the published GPU timings and their latency, no heap allocation
 * after warm-up, and a three-pass graph on Metal read back byte for byte. */
#include "de_test.h"

#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>
#include <dengine/de_task.h>

#include "de_alloc_guard.h"
#include "gpu/gpu_backend.h"
#include "render/graph/rg_internal.h"

#if DE_PIPE_TEST_COOK
#include "pipeline_shaders.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct xfx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_arena *arena;
    de_rg *g;
    de_gpu_device *dev;
    de_gpu_texture out; /* an imported output (history / swapchain) */
    de_task_system *tasks;
} xfx;

static void open_null(xfx *f, bool breadcrumbs) {
    ASSERT_EQ(de_gpu_device_create(
                  &(de_gpu_device_desc){.backend = DE_GPU_NULL, .crash_breadcrumbs = breadcrumbs},
                  &f->dev),
              DE_OK);
    ASSERT_EQ(
        de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "t"}, &f->g),
        DE_OK);
    f->out = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                       .width = 64,
                                       .height = 64,
                                       .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                                DE_USAGE_STORAGE | DE_USAGE_COPY_DST,
                                       .name = "out"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->out));
}

static void xfx_setup(xfx *f) {
    de_fp_env_init_thread(); /* the task system's creating thread must have it (SPEC §5.7) */
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "rg_execute_test"}, &f->arena), DE_OK);
    de_error_clear();
}

static void xfx_teardown(xfx *f) {
    if (f->g) {
        de__rg_reset(f->g);
        de__rg_destroy(f->g);
    }
    if (f->tasks)
        de_task_system_destroy(f->tasks);
    if (f->dev) {
        de_gpu_wait_idle(f->dev);
        if (DE_HANDLE_VALID(f->out))
            de_gpu_texture_destroy(f->dev, f->out);
        de_gpu_device_destroy(f->dev);
    }
    if (f->arena)
        de_arena_destroy(f->arena);
    if (f->alloc.alloc)
        EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
}

static void frame_begin(xfx *f) {
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
}

static void frame_run(xfx *f, de_task_system *tasks, u64 frame) {
    de_result r = de__rg_finalize(f->g);
    if (r == DE_OK)
        r = de__rg_compile(f->g);
    if (r == DE_OK)
        r = de__rg_execute(f->g, &(de__rg_execute_desc){.tasks = tasks, .frame_index = frame});
    if (r != DE_OK)
        fprintf(stderr, "frame %llu: %s\n", (unsigned long long)frame,
                de_error_last() ? de_error_last()->message : "?");
    ASSERT_EQ(r, DE_OK);
}

/* ---------- Pass callbacks ---------- */

#define MAX_PASSES 80u

/* What each pass's callback saw. Each pass writes only its own entry, so the callbacks may run
 * on different workers at once. */
typedef struct pass_rec {
    u32 calls;
    u32 worker;
} pass_rec;

static pass_rec g_rec[MAX_PASSES];

static void exec_mark(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    (void)pass;
    const u32 i = (u32)(uintptr_t)user;
    g_rec[i].calls++;
    g_rec[i].worker = de_task_worker_index();
    char name[32];
    snprintf(name, sizeof name, "in%u", i);
    de_gpu_cmd_marker_push(cmd, name, 0); /* nested inside the graph's marker */
    de_gpu_cmd_marker_pop(cmd);
}

/* 64 passes, mostly graphics with some async compute and copy work, chained through eight
 * transient textures; the last one writes the imported output. Returns the pass count. */
static u32 build_64(xfx *f) {
    de_rg *g = f->g;
    de_rg_resource out = de_rg_import_texture(g, f->out, DE_ACCESS_NONE);
    de_rg_set_name(g, out, "out");
    de_rg_resource t[8];
    for (u32 i = 0; i < 8; ++i) {
        char name[16];
        snprintf(name, sizeof name, "t%u", i);
        t[i] = de_rg_create_texture(
            g, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                      .width = 32u << (i % 3),
                                      .height = 32,
                                      .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE |
                                               DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                                      .name = name});
    }
    const u32 n = 64;
    for (u32 p = 0; p + 1u < n; ++p) {
        char name[16];
        snprintf(name, sizeof name, "p%u", p);
        const de_gpu_queue q = p % 21u == 7u   ? DE_QUEUE_COPY
                               : p % 16u == 5u ? DE_QUEUE_COMPUTE
                                               : DE_QUEUE_GRAPHICS;
        de_rg_pass *ps = de_rg_add_pass(g, name, q, exec_mark, (void *)(uintptr_t)p);
        const u32 w = p < 8u ? p : (p * 5u + 1u) % 8u, r = (p * 3u) % 8u;
        if (q == DE_QUEUE_COPY) {
            if (p >= 8u)
                de_rg_read(ps, t[r], DE_ACCESS_COPY_SRC);
            if (r != w || p < 8u)
                de_rg_write(ps, t[w], DE_ACCESS_COPY_DST);
        } else {
            if (p >= 8u)
                de_rg_read(ps, t[r], DE_ACCESS_SHADER_READ);
            de_rg_write(ps, t[w], DE_ACCESS_SHADER_WRITE);
        }
        de_rg_pass_side_effect(ps); /* keep all 64 alive: the test counts them */
    }
    de_rg_pass *last =
        de_rg_add_pass(g, "p63", DE_QUEUE_GRAPHICS, exec_mark, (void *)(uintptr_t)63);
    de_rg_read(last, t[2], DE_ACCESS_SHADER_READ);
    de_rg_write(last, out, DE_ACCESS_RENDER_TARGET);
    return n;
}

/* ---------- The null command log as text ---------- */

typedef struct log_line {
    char list[DE__GPU_NAME_MAX];
    u32 seq;
    char text[160];
} log_line;

static int cmp_line(const void *a, const void *b) {
    const log_line *x = (const log_line *)a, *y = (const log_line *)b;
    const int c = strcmp(x->list, y->list);
    if (c)
        return c;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static const char *queue_of_fence(xfx *f, de_gpu_fence fence) {
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
        if (f->g->exec && f->g->exec->timeline[q].id == fence.id)
            return de__rg_queue_name((de_gpu_queue)q);
    return "?";
}

/* One entry as text. Breadcrumb values and fence values change every frame: values are written
 * relative to `base` (the timelines before the frame), breadcrumbs without their value. */
static void entry_text(xfx *f, const de__gpu_null_entry *e, const u64 *base, char *out,
                       size_t cap) {
    switch (e->op) {
    case DE__GPU_NULL_OP_BARRIERS:
        snprintf(out, cap, "barriers %u", e->data_count);
        break;
    case DE__GPU_NULL_OP_MARKER_PUSH:
        snprintf(out, cap, "push %s #%06llx", e->text, (unsigned long long)e->a[0]);
        break;
    case DE__GPU_NULL_OP_MARKER_POP:
        snprintf(out, cap, "pop");
        break;
    case DE__GPU_NULL_OP_TIMESTAMP:
        snprintf(out, cap, "timestamp %s %llu", e->text, (unsigned long long)e->a[0]);
        break;
    case DE__GPU_NULL_OP_BREADCRUMB:
        snprintf(out, cap, "breadcrumb");
        break;
    case DE__GPU_NULL_OP_RESOLVE:
        snprintf(out, cap, "resolve %s %llu+%llu", e->text, (unsigned long long)e->a[0],
                 (unsigned long long)e->a[1]);
        break;
    case DE__GPU_NULL_OP_SUBMIT: {
        size_t n = (size_t)snprintf(out, cap, "submit %s lists %llu", de__rg_queue_name(e->queue),
                                    (unsigned long long)e->a[0]);
        const de_gpu_wait *w = de__gpu_null_log_waits(f->dev, e);
        for (u32 k = 0; k < e->data_count && n < cap; ++k) {
            const char *qn = queue_of_fence(f, w[k].fence);
            u64 b = 0;
            for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
                if (f->g->exec->timeline[q].id == w[k].fence.id)
                    b = base[q];
            n += (size_t)snprintf(out + n, cap - n, " wait %s+%lld", qn,
                                  (long long)(w[k].value - b));
        }
        if (e->a[2] && n < cap) {
            u64 b = 0;
            for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
                if (f->g->exec->timeline[q].id == (u32)e->a[2])
                    b = base[q];
            snprintf(out + n, cap - n, " signal %s+%lld",
                     queue_of_fence(f, (de_gpu_fence){(u32)e->a[2]}), (long long)(e->a[3] - b));
        }
        break;
    }
    default:
        snprintf(out, cap, "op %u", e->op);
    }
}

/* The frame's log in a form that does not depend on how lists were interleaved by the workers:
 * each list's commands in recording order, the lists sorted by name, then the submits in order. */
static size_t canonical_log(xfx *f, const u64 *base, char *out, size_t cap) {
    const u32 count = de__gpu_null_log_count(f->dev);
    log_line *lines = (log_line *)calloc(count ? count : 1u, sizeof *lines);
    if (!lines)
        return 0;
    u32 nl = 0;
    for (u32 i = 0; i < count; ++i) {
        const de__gpu_null_entry *e = de__gpu_null_log_entry(f->dev, i);
        if (e->op == DE__GPU_NULL_OP_SUBMIT)
            continue;
        log_line *l = &lines[nl];
        memcpy(l->list, e->list, sizeof l->list);
        l->seq = nl++;
        entry_text(f, e, base, l->text, sizeof l->text);
    }
    qsort(lines, nl, sizeof *lines, cmp_line);
    size_t n = 0;
    out[0] = 0;
    for (u32 i = 0; i < nl && n < cap; ++i)
        n += (size_t)snprintf(out + n, cap - n, "%s: %s\n", lines[i].list, lines[i].text);
    for (u32 i = 0; i < count && n < cap; ++i) {
        const de__gpu_null_entry *e = de__gpu_null_log_entry(f->dev, i);
        if (e->op != DE__GPU_NULL_OP_SUBMIT)
            continue;
        char t[256];
        entry_text(f, e, base, t, sizeof t);
        n += (size_t)snprintf(out + n, cap - n, "%s (%s)\n", t, e->list);
    }
    free(lines);
    return n;
}

/* Runs one frame of build_64 with the log on and returns its canonical text (malloc'd). */
static char *logged_frame(xfx *f, de_task_system *tasks, u64 frame) {
    u64 base[DE_QUEUE_COUNT_] = {0};
    if (f->g->exec)
        memcpy(base, f->g->exec->value, sizeof base);
    memset(g_rec, 0, sizeof g_rec);
    frame_begin(f);
    build_64(f);
    de__gpu_null_log_enable(f->dev, true);
    frame_run(f, tasks, frame);
    de__gpu_null_log_enable(f->dev, false);
    const size_t cap = 1u << 20;
    char *text = (char *)malloc(cap);
    if (text)
        canonical_log(f, base, text, cap);
    return text;
}

static bool same_partition(const de__rg_chunk *a, u32 na, const de__rg_chunk *b, u32 nb) {
    if (na != nb)
        return false;
    for (u32 i = 0; i < na; ++i)
        if (a[i].batch != b[i].batch || a[i].first != b[i].first || a[i].count != b[i].count ||
            a[i].queue != b[i].queue)
            return false;
    return true;
}

/* ---------- Tests ---------- */

/* A 64-pass graph recorded on the calling thread and on 4 workers: the same command lists, the
 * same commands in each, the same submits. */
TEST_F(rg_execute, parallel_deterministic_partition, xfx) {
    open_null(f, true);
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 4,
                                                           .background_worker_count = 1,
                                                           .allocator = &f->alloc},
                                    &f->tasks),
              DE_OK);
    /* Warm up every timing slot (heaps, names). */
    for (u32 i = 0; i < DE__RG_TIMING_SLOTS; ++i) {
        frame_begin(f);
        build_64(f);
        frame_run(f, f->tasks, i);
    }
    char *serial = logged_frame(f, NULL, 10);
    u32 n1 = 0;
    const de__rg_chunk *c1 = de__rg_exec_chunks(f->g, &n1);
    de__rg_chunk part1[MAX_PASSES];
    ASSERT_LE(n1, MAX_PASSES);
    memcpy(part1, c1, sizeof *c1 * n1);
    for (u32 p = 0; p < 64; ++p)
        EXPECT_EQ(g_rec[p].calls, 1u);

    u32 workers_seen = 0;
    char *parallel = NULL;
    for (u32 round = 0; round < 4; ++round) { /* until two workers recorded (not guaranteed) */
        free(parallel);
        parallel = logged_frame(f, f->tasks, 11 + round);
        u32 seen = 0;
        for (u32 p = 0; p < 64; ++p)
            seen |= 1u << (g_rec[p].worker & 31u);
        workers_seen = 0;
        for (u32 b = 0; b < 32; ++b)
            workers_seen += (seen >> b) & 1u;
        if (workers_seen > 1)
            break;
    }
    u32 n2 = 0;
    const de__rg_chunk *c2 = de__rg_exec_chunks(f->g, &n2);
    for (u32 p = 0; p < 64; ++p)
        EXPECT_EQ(g_rec[p].calls, 1u);
    printf("%u command lists over %u batches; %u worker(s) recorded the parallel frame\n", n1,
           f->g->plan->batch_count, workers_seen);
    EXPECT_TRUE(same_partition(part1, n1, c2, n2));
    EXPECT_GT(n1, f->g->plan->batch_count); /* some batch was split into several lists */
    ASSERT_TRUE(serial != NULL && parallel != NULL);
    if (strcmp(serial, parallel) != 0)
        printf("--- serial ---\n%s\n--- parallel ---\n%s\n", serial, parallel);
    EXPECT_STREQ(serial, parallel);
    /* Chunks follow the plan: consecutive steps of one batch, every step exactly once. */
    u32 covered = 0;
    for (u32 i = 0; i < n1; ++i) {
        const de__rg_batch *b = &f->g->plan->batches[part1[i].batch];
        EXPECT_GE(part1[i].first, b->step_first);
        EXPECT_LE(part1[i].first + part1[i].count, b->step_first + b->step_count);
        EXPECT_EQ(part1[i].queue, b->queue);
        if (i > 0)
            EXPECT_EQ(part1[i].first, part1[i - 1].first + part1[i - 1].count);
        covered += part1[i].count;
    }
    EXPECT_EQ(covered, f->g->plan->step_count);
    free(serial);
    free(parallel);
}

/* Every executed pass: one breadcrumb, then its marker, a begin timestamp, ..., an end timestamp
 * and the pop, in its list; the frame span and the resolves. */
TEST_F(rg_execute, markers_timers_breadcrumbs, xfx) {
    open_null(f, true);
    frame_begin(f);
    build_64(f);
    frame_run(f, NULL, 0); /* warm-up: heaps */
    memset(g_rec, 0, sizeof g_rec);
    frame_begin(f);
    build_64(f);
    de__gpu_null_log_enable(f->dev, true);
    frame_run(f, NULL, 1);
    de__gpu_null_log_enable(f->dev, false);

    const u32 count = de__gpu_null_log_count(f->dev);
    u32 pushes = 0, pops = 0, stamps = 0, crumbs = 0, resolves = 0, passes_ok = 0;
    bool seen[64] = {false};
    for (u32 i = 0; i < count; ++i) {
        const de__gpu_null_entry *e = de__gpu_null_log_entry(f->dev, i);
        pushes += e->op == DE__GPU_NULL_OP_MARKER_PUSH;
        pops += e->op == DE__GPU_NULL_OP_MARKER_POP;
        stamps += e->op == DE__GPU_NULL_OP_TIMESTAMP;
        crumbs += e->op == DE__GPU_NULL_OP_BREADCRUMB;
        resolves += e->op == DE__GPU_NULL_OP_RESOLVE;
        if (e->op != DE__GPU_NULL_OP_MARKER_PUSH || e->text[0] != 'p')
            continue;
        /* A graph marker "p<n>": the list's entries around it (one thread, one list). */
        const u32 p = (u32)atoi(e->text + 1);
        ASSERT_LT(p, 64u);
        EXPECT_FALSE(seen[p]);
        seen[p] = true;
        u32 at[16], na = 0; /* indices of this list's entries from the breadcrumb on */
        for (u32 j = i; j-- > 0 && na < 1;)
            if (strcmp(de__gpu_null_log_entry(f->dev, j)->list, e->list) == 0)
                at[na++] = j;
        for (u32 j = i; j < count && na < 16; ++j)
            if (strcmp(de__gpu_null_log_entry(f->dev, j)->list, e->list) == 0)
                at[na++] = j;
        ASSERT_GE(na, 6u);
        const de__gpu_null_entry *bc = de__gpu_null_log_entry(f->dev, at[0]);
        const de__gpu_null_entry *t0 = de__gpu_null_log_entry(f->dev, at[2]);
        EXPECT_EQ(bc->op, (u32)DE__GPU_NULL_OP_BREADCRUMB);
        EXPECT_EQ(t0->op, (u32)DE__GPU_NULL_OP_TIMESTAMP);
        /* Then optional PRE barriers, the callback's own marker pair, optional POST barriers,
         * the end timestamp (query + 1) and the pop. */
        u32 k = 3;
        while (k < na && de__gpu_null_log_entry(f->dev, at[k])->op == DE__GPU_NULL_OP_BARRIERS)
            ++k;
        ASSERT_LT(k + 1u, na);
        EXPECT_EQ(de__gpu_null_log_entry(f->dev, at[k])->op, (u32)DE__GPU_NULL_OP_MARKER_PUSH);
        EXPECT_EQ(de__gpu_null_log_entry(f->dev, at[k + 1u])->op, (u32)DE__GPU_NULL_OP_MARKER_POP);
        k += 2;
        while (k < na && de__gpu_null_log_entry(f->dev, at[k])->op == DE__GPU_NULL_OP_BARRIERS)
            ++k;
        ASSERT_LT(k + 1u, na);
        const de__gpu_null_entry *t1 = de__gpu_null_log_entry(f->dev, at[k]);
        EXPECT_EQ(t1->op, (u32)DE__GPU_NULL_OP_TIMESTAMP);
        EXPECT_EQ(t1->a[0], t0->a[0] + 1u);
        EXPECT_STREQ(t1->text, t0->text);
        EXPECT_EQ(de__gpu_null_log_entry(f->dev, at[k + 1u])->op, (u32)DE__GPU_NULL_OP_MARKER_POP);
        /* Colour by queue; copy passes time on the copy heap. */
        const bool copy = e->queue == DE_QUEUE_COPY;
        EXPECT_TRUE(strstr(t0->text, copy ? "copy" : "graphics") != NULL);
        EXPECT_NE(e->a[0], 0u);
        ++passes_ok;
    }
    EXPECT_EQ(passes_ok, 64u);
    EXPECT_EQ(pushes, 128u); /* the graph's + the callbacks' */
    EXPECT_EQ(pops, 128u);
    EXPECT_EQ(crumbs, 64u);
    EXPECT_EQ(stamps, 64u * 2u + 2u); /* + the frame span */
    EXPECT_EQ(resolves, 2u);          /* main heap + copy heap */
    for (u32 p = 0; p < 64; ++p)
        EXPECT_TRUE(seen[p]);
    /* Breadcrumb names are registered for the crash report. */
    EXPECT_NE(de__rg_breadcrumb_id(f->g, 1, 3), de__rg_breadcrumb_id(f->g, 2, 3));
}

/* Culled passes record nothing: no marker, no timestamp, no breadcrumb, no callback. */
TEST_F(rg_execute, culled_no_commands, xfx) {
    open_null(f, true);
    memset(g_rec, 0, sizeof g_rec);
    frame_begin(f);
    de_rg *g = f->g;
    de_rg_resource out = de_rg_import_texture(g, f->out, DE_ACCESS_NONE);
    const de_gpu_texture_desc td = {.format = DE_FMT_RGBA16_FLOAT,
                                    .width = 16,
                                    .height = 16,
                                    .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE,
                                    .name = "unused"};
    de_rg_resource unused = de_rg_create_texture(g, &td);
    de_rg_pass *a = de_rg_add_pass(g, "p0", DE_QUEUE_GRAPHICS, exec_mark, (void *)(uintptr_t)0);
    de_rg_write(a, out, DE_ACCESS_SHADER_WRITE);
    de_rg_pass *c1 = de_rg_add_pass(g, "p1", DE_QUEUE_COMPUTE, exec_mark, (void *)(uintptr_t)1);
    de_rg_write(c1, unused, DE_ACCESS_SHADER_WRITE); /* nobody reads it: culled */
    de_rg_pass *c2 = de_rg_add_pass(g, "p2", DE_QUEUE_GRAPHICS, exec_mark, (void *)(uintptr_t)2);
    de_rg_read(c2, unused, DE_ACCESS_SHADER_READ);
    de_rg_write(c2, unused, DE_ACCESS_SHADER_WRITE); /* culled with it */
    de_rg_pass *b = de_rg_add_pass(g, "p3", DE_QUEUE_GRAPHICS, exec_mark, (void *)(uintptr_t)3);
    de_rg_read(b, out, DE_ACCESS_SHADER_READ);
    de_rg_write(b, out, DE_ACCESS_SHADER_WRITE);
    de__gpu_null_log_enable(f->dev, true);
    frame_run(f, NULL, 0);
    de__gpu_null_log_enable(f->dev, false);
    EXPECT_TRUE(g->passes[1]->culled);
    EXPECT_TRUE(g->passes[2]->culled);
    EXPECT_EQ(g_rec[0].calls, 1u);
    EXPECT_EQ(g_rec[1].calls, 0u);
    EXPECT_EQ(g_rec[2].calls, 0u);
    EXPECT_EQ(g_rec[3].calls, 1u);
    u32 stamps = 0, crumbs = 0, compute_entries = 0;
    for (u32 i = 0; i < de__gpu_null_log_count(f->dev); ++i) {
        const de__gpu_null_entry *e = de__gpu_null_log_entry(f->dev, i);
        stamps += e->op == DE__GPU_NULL_OP_TIMESTAMP;
        crumbs += e->op == DE__GPU_NULL_OP_BREADCRUMB;
        compute_entries += e->queue == DE_QUEUE_COMPUTE;
        if (e->op == DE__GPU_NULL_OP_MARKER_PUSH) {
            EXPECT_TRUE(strcmp(e->text, "p1") != 0 && strcmp(e->text, "in1") != 0);
            EXPECT_TRUE(strcmp(e->text, "p2") != 0 && strcmp(e->text, "in2") != 0);
        }
    }
    EXPECT_EQ(stamps, 2u * 2u + 2u);
    EXPECT_EQ(crumbs, 2u);
    EXPECT_EQ(compute_entries, 0u); /* the culled compute pass made no compute list */
    /* The published names hold only the executed passes (next frame publishes this one). */
    frame_begin(f);
    de_rg_pass *only = de_rg_add_pass(f->g, "solo", DE_QUEUE_GRAPHICS, NULL, NULL);
    de_rg_pass_side_effect(only);
    frame_run(f, NULL, 1);
    const de__rg_timings *t = de__rg_timings_get(f->g);
    ASSERT_TRUE(t->valid);
    ASSERT_EQ(t->pass_count, 2u);
    EXPECT_STREQ(t->pass_names[0], "p0");
    EXPECT_STREQ(t->pass_names[1], "p3");
}

/* Timings arrive one frame late on the null device (its fences complete at submit), labelled with
 * the frame that produced them even when the next frame has another pass set. */
TEST_F(rg_execute, stats_latency, xfx) {
    open_null(f, false);
    static const char *const names_a[3] = {"gbuffer", "lighting", "copyout"};
    static const char *const names_b[2] = {"lighting", "copyout"};
    de_gpu_buffer rb =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = 64u * 64u * 8u,
                                                           .usage = DE_USAGE_COPY_DST,
                                                           .memory = DE_MEM_READBACK,
                                                           .name = "rb"});
    ASSERT_TRUE(DE_HANDLE_VALID(rb));
    for (u32 frame = 0; frame < 4; ++frame) {
        frame_begin(f);
        de_rg *g = f->g;
        de_rg_resource out = de_rg_import_texture(g, f->out, DE_ACCESS_NONE);
        const bool a = frame % 2u == 0u; /* frames 0 and 2: three passes; 1 and 3: two */
        if (a) {
            de_rg_pass *p = de_rg_add_pass(g, "gbuffer", DE_QUEUE_GRAPHICS, NULL, NULL);
            de_rg_write(p, out, DE_ACCESS_RENDER_TARGET);
        }
        de_rg_pass *l = de_rg_add_pass(g, "lighting", DE_QUEUE_GRAPHICS, NULL, NULL);
        if (a)
            de_rg_read(l, out, DE_ACCESS_RENDER_TARGET);
        de_rg_write(l, out, DE_ACCESS_RENDER_TARGET);
        de_rg_pass *c = de_rg_add_pass(g, "copyout", DE_QUEUE_COPY, NULL, NULL);
        de_rg_read(c, out, DE_ACCESS_COPY_SRC);
        de_rg_write(c, de_rg_import_buffer(g, rb, DE_ACCESS_NONE), DE_ACCESS_COPY_DST);
        frame_run(f, NULL, 100 + frame);
        const de__rg_timings *t = de__rg_timings_get(f->g);
        EXPECT_TRUE(t->cpu_record_ms >= 0.0f);
        if (frame == 0) {
            EXPECT_FALSE(t->valid); /* nothing read back yet */
            EXPECT_EQ(t->pass_count, 0u);
            continue;
        }
        /* The previous frame's results. */
        ASSERT_TRUE(t->valid);
        EXPECT_EQ(t->frame_index, 100u + frame - 1u);
        const bool prev_a = !a;
        const char *const *want = prev_a ? names_a : names_b;
        ASSERT_EQ(t->pass_count, prev_a ? 3u : 2u);
        /* Null ticks: a region [q, q + 1] lasts 1000 * (q + 2) ns. Graphics passes take main-heap
         * queries 2, 4, ...; the copy pass takes copy-heap query 0. */
        u32 next_main = 2;
        for (u32 i = 0; i < t->pass_count; ++i) {
            EXPECT_STREQ(t->pass_names[i], want[i]);
            const bool is_copy = strcmp(want[i], "copyout") == 0;
            const u32 q = is_copy ? 0u : next_main;
            if (!is_copy)
                next_main += 2u;
            EXPECT_EQ(t->pass_queue[i], is_copy ? DE_QUEUE_COPY : DE_QUEUE_GRAPHICS);
            EXPECT_FLOAT_EQ(t->pass_gpu_ms[i], (f32)(q + 2u) * 1e-3f, 1e-6f);
        }
        EXPECT_FLOAT_EQ(t->gpu_ms, 2e-3f, 1e-6f); /* queries 0 and 1 */
        de_render_stats s;
        memset(&s, 0, sizeof s);
        de__rg_fill_stats(f->g, &s);
        EXPECT_EQ(s.pass_count, t->pass_count);
        EXPECT_TRUE(s.pass_names == t->pass_names && s.pass_gpu_ms == t->pass_gpu_ms);
        EXPECT_FLOAT_EQ(s.gpu_ms, t->gpu_ms, 0.0f);
    }
    /* Timers off: the names still come, the times read 0. */
    de_cvar cv = de_cvar_find("r.rg_gpu_timers");
    ASSERT_TRUE(DE_HANDLE_VALID(cv));
    ASSERT_EQ(de_cvar_set(cv, "false"), DE_OK);
    for (u32 frame = 0; frame < 2; ++frame) {
        frame_begin(f);
        de_rg_pass *p = de_rg_add_pass(f->g, "solo", DE_QUEUE_GRAPHICS, NULL, NULL);
        de_rg_pass_side_effect(p);
        de__gpu_null_log_enable(f->dev, true);
        frame_run(f, NULL, 200 + frame);
        u32 stamps = 0;
        for (u32 i = 0; i < de__gpu_null_log_count(f->dev); ++i)
            stamps += de__gpu_null_log_entry(f->dev, i)->op == DE__GPU_NULL_OP_TIMESTAMP;
        de__gpu_null_log_enable(f->dev, false);
        EXPECT_EQ(stamps, 0u);
    }
    const de__rg_timings *t = de__rg_timings_get(f->g);
    ASSERT_EQ(t->pass_count, 1u);
    EXPECT_STREQ(t->pass_names[0], "solo");
    EXPECT_FLOAT_EQ(t->pass_gpu_ms[0], 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(t->gpu_ms, 0.0f, 0.0f);
    EXPECT_EQ(de_cvar_set(cv, "true"), DE_OK);
    de_gpu_buffer_destroy(f->dev, rb);
}

/* After warm-up, frames of build + compile + parallel record + submit allocate nothing. */
TEST_F(rg_execute, no_heap_after_warmup, xfx) {
    open_null(f, true);
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 4,
                                                           .background_worker_count = 1,
                                                           .allocator = &f->alloc},
                                    &f->tasks),
              DE_OK);
    for (u32 i = 0; i < 2u * DE__RG_TIMING_SLOTS; ++i) {
        frame_begin(f);
        build_64(f);
        frame_run(f, f->tasks, i);
    }
    const de_tracking_stats before = de_tracking_allocator_stats(&f->track);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    de_result r = DE_OK;
    for (u32 i = 0; i < 20 && r == DE_OK; ++i) {
        de__rg_reset(f->g);
        de_arena_reset(f->arena);
        r = de__rg_begin(f->g, f->arena);
        if (r == DE_OK) {
            build_64(f);
            r = de__rg_finalize(f->g);
        }
        if (r == DE_OK)
            r = de__rg_compile(f->g);
        if (r == DE_OK)
            r = de__rg_execute(f->g,
                               &(de__rg_execute_desc){.tasks = f->tasks, .frame_index = 100 + i});
    }
    de_alloc_guard_end(&guard);
    ASSERT_EQ(r, DE_OK);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    const de_tracking_stats after = de_tracking_allocator_stats(&f->track);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    const de__rg_timings *t = de__rg_timings_get(f->g);
    EXPECT_TRUE(t->valid);
    EXPECT_EQ(t->pass_count, 64u);
}

/* ---------- Metal: clear -> compute write -> copy to readback ---------- */

#if defined(DE_HAS_GPU_METAL) && DE_HAS_GPU_METAL && defined(DE_PIPE_TEST_COOK) && DE_PIPE_TEST_COOK
#define M_ENABLED 1
#else
#define M_ENABLED 0
#endif

#if M_ENABLED
#define M_SIZE  64u
#define M_PITCH (M_SIZE * 4u)
#define M_ELEMS 1000u

typedef struct metal_graph {
    de_rg_resource rt, src, dst, rb_tex, rb_buf;
    de_gpu_pipeline pipe;
    de_gpu_device *dev;
} metal_graph;

static void m_clear(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const metal_graph *m = (const metal_graph *)user;
    de_gpu_attachment a = {.texture = de_rg_texture(pass, m->rt),
                           .clear = true,
                           .store = true,
                           .clear_color = {0.2f, 0.4f, 0.6f, 1.0f}};
    de_gpu_cmd_begin_render(cmd, &a, 1, NULL);
    de_gpu_cmd_end_render(cmd);
}

static void m_compute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const metal_graph *m = (const metal_graph *)user;
    de_gpu_cmd_set_pipeline(cmd, m->pipe);
    const u32 pc[4] = {de_gpu_buffer_bindless(m->dev, de_rg_buffer(pass, m->src)),
                       de_gpu_buffer_bindless(m->dev, de_rg_buffer(pass, m->dst)), M_ELEMS,
                       0x1000u};
    de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (M_ELEMS + 63u) / 64u, 1, 1);
}

static void m_copy(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const metal_graph *m = (const metal_graph *)user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, de_rg_buffer(pass, m->rb_tex), 0, M_PITCH,
                                      de_rg_texture(pass, m->rt), 0, 0);
    de_gpu_cmd_copy_buffer(cmd, de_rg_buffer(pass, m->rb_buf), 0, de_rg_buffer(pass, m->dst), 0,
                           M_ELEMS * 4u);
}

#endif /* M_ENABLED */

TEST_F(rg_execute, metal_three_pass_readback, xfx) {
#if !M_ENABLED
    (void)f;
    SKIP("needs the Metal backend and the shader importer (the compute pass runs compute.slang)");
#else
    static char why[600];
    setenv("MTL_DEBUG_LAYER", "1", 0); /* Metal API validation, before the first device */
    de_result r = de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_METAL,
                                                             .debug_layer = true,
                                                             .pipeline_cache_dir = ""},
                                       &f->dev);
    if (r != DE_OK) {
        f->dev = NULL;
        snprintf(why, sizeof why, "no Metal device: %.400s",
                 de_error_last() ? de_error_last()->message : "?");
        SKIP(why);
    }
    const de_gpu_caps *caps = de_gpu_get_caps(f->dev);
    printf("device: \"%s\", async compute %d\n", caps->device_name, caps->async_compute);
    if (!caps->base_tier_ok) {
        /* CI's paravirtual GPU hangs on the Metal 3 compute kernel (DENG-328). */
        snprintf(why, sizeof why, "below the Base tier: %.400s", caps->base_tier_reason);
        SKIP(why);
    }
    if (!de_pipe_test_cook(DE__SHADER_TARGET_METALLIB, why, sizeof why))
        SKIP(why);
    de_pipe_test_shader cs;
    ASSERT_TRUE(de_pipe_test_get(DE__SHADER_TARGET_METALLIB, "compute.slang", "cs_main", &cs));
    metal_graph m = {.dev = f->dev};
    m.pipe = de_gpu_pipeline_create(
        f->dev, &(de_gpu_pipeline_desc){
                    .kind = DE_PIPE_COMPUTE,
                    .cs = cs.blob,
                    .push_constant_bytes = cs.push_constant_bytes,
                    .threadgroup_size = {cs.threadgroup[0], cs.threadgroup[1], cs.threadgroup[2]},
                    .name = "rg compute"});
    ASSERT_TRUE(DE_HANDLE_VALID(m.pipe));
    /* src: an UPLOAD buffer the kernel reads (shared memory on Apple GPUs). */
    de_gpu_buffer src =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = M_ELEMS * 4u,
                                                           .usage = DE_USAGE_STORAGE,
                                                           .stride = 4,
                                                           .memory = DE_MEM_UPLOAD,
                                                           .name = "src"});
    de_gpu_buffer dst = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = M_ELEMS * 4u,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                      .stride = 4,
                                      .name = "dst"});
    de_gpu_buffer rb_tex =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)M_PITCH * M_SIZE,
                                                           .usage = DE_USAGE_COPY_DST,
                                                           .memory = DE_MEM_READBACK,
                                                           .name = "rb tex"});
    de_gpu_buffer rb_buf =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = M_ELEMS * 4u,
                                                           .usage = DE_USAGE_COPY_DST,
                                                           .memory = DE_MEM_READBACK,
                                                           .name = "rb buf"});
    ASSERT_TRUE(DE_HANDLE_VALID(src) && DE_HANDLE_VALID(dst) && DE_HANDLE_VALID(rb_tex) &&
                DE_HANDLE_VALID(rb_buf));
    u32 *s = (u32 *)de_gpu_buffer_map(f->dev, src);
    ASSERT_TRUE(s != NULL);
    for (u32 i = 0; i < M_ELEMS; ++i)
        s[i] = i * 7u + 1u;
    ASSERT_EQ(
        de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "metal3"},
                      &f->g),
        DE_OK);
    de_gpu_fence frame_fence = de_gpu_fence_create(f->dev, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(frame_fence));

    u32 state[4] = {DE_ACCESS_NONE, DE_ACCESS_NONE, DE_ACCESS_NONE, DE_ACCESS_NONE};
    bool timed = false;
    for (u32 frame = 0; frame < 3; ++frame) {
        u8 *px = (u8 *)de_gpu_buffer_map(f->dev, rb_tex);
        u32 *out = (u32 *)de_gpu_buffer_map(f->dev, rb_buf);
        memset(px, 0, (size_t)M_PITCH * M_SIZE);
        memset(out, 0, M_ELEMS * 4u);
        frame_begin(f);
        de_rg *g = f->g;
        m.rt = de_rg_create_texture(
            g, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                      .width = M_SIZE,
                                      .height = M_SIZE,
                                      .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_SRC,
                                      .name = "rt"});
        m.src = de_rg_import_buffer(g, src, state[0]);
        m.dst = de_rg_import_buffer(g, dst, state[1]);
        m.rb_tex = de_rg_import_buffer(g, rb_tex, state[2]);
        m.rb_buf = de_rg_import_buffer(g, rb_buf, state[3]);
        de_rg_pass *clear = de_rg_add_pass(g, "clear", DE_QUEUE_GRAPHICS, m_clear, &m);
        de_rg_write(clear, m.rt, DE_ACCESS_RENDER_TARGET);
        de_rg_pass *comp = de_rg_add_pass(g, "compute", DE_QUEUE_COMPUTE, m_compute, &m);
        de_rg_read(comp, m.src, DE_ACCESS_SHADER_READ);
        de_rg_write(comp, m.dst, DE_ACCESS_SHADER_WRITE);
        de_rg_pass *copy = de_rg_add_pass(g, "readback", DE_QUEUE_COPY, m_copy, &m);
        de_rg_read(copy, m.rt, DE_ACCESS_COPY_SRC);
        de_rg_read(copy, m.dst, DE_ACCESS_COPY_SRC);
        de_rg_write(copy, m.rb_tex, DE_ACCESS_COPY_DST);
        de_rg_write(copy, m.rb_buf, DE_ACCESS_COPY_DST);
        r = de__rg_finalize(g);
        if (r == DE_OK)
            r = de__rg_compile(g);
        if (r == DE_OK)
            r = de__rg_execute(g, &(de__rg_execute_desc){.frame_index = frame,
                                                         .frame_fence = frame_fence,
                                                         .frame_value = frame + 1u});
        if (r != DE_OK)
            fprintf(stderr, "frame %u: %s\n", frame,
                    de_error_last() ? de_error_last()->message : "?");
        ASSERT_EQ(r, DE_OK);
        const de_rg_resource imports[4] = {m.src, m.dst, m.rb_tex, m.rb_buf};
        for (u32 i = 0; i < 4; ++i)
            state[i] = de__rg_end_access(g, imports[i]);
        ASSERT_EQ(de_gpu_fence_wait(f->dev, frame_fence, frame + 1u, 10ull * 1000000000ull), DE_OK);
        de_gpu_fence tl;
        u64 end = 0;
        ASSERT_TRUE(de__rg_frame_end(g, &tl, &end));
        EXPECT_GE(de_gpu_fence_value(f->dev, tl), end);
        const u8 want_px[4] = {0x33, 0x66, 0x99, 0xFF};
        EXPECT_MEMEQ(px, want_px, 4);
        EXPECT_MEMEQ(px + (size_t)M_PITCH * M_SIZE - 4u, want_px, 4);
        u32 bad = 0;
        for (u32 i = 0; i < M_ELEMS; ++i)
            bad += out[i] != (i * 7u + 1u) * 3u + 0x1000u;
        if (bad)
            fprintf(stderr, "frame %u: %u wrong elements, out[0] = %u\n", frame, bad, out[0]);
        EXPECT_EQ(bad, 0u);
        const de__rg_timings *t = de__rg_timings_get(g);
        if (frame >= 1) {
            ASSERT_TRUE(t->valid);
            ASSERT_EQ(t->pass_count, 3u);
            EXPECT_STREQ(t->pass_names[0], "clear");
            EXPECT_STREQ(t->pass_names[1], "compute");
            EXPECT_STREQ(t->pass_names[2], "readback");
            for (u32 i = 0; i < 3; ++i) {
                EXPECT_TRUE(t->pass_gpu_ms[i] >= 0.0f);
                timed |= t->pass_gpu_ms[i] > 0.0f;
            }
            EXPECT_TRUE(t->gpu_ms >= 0.0f);
            printf("frame %u (timings of frame %llu): clear %.4f ms, compute %.4f ms, readback "
                   "%.4f ms, frame %.4f ms, record %.3f ms\n",
                   frame, (unsigned long long)t->frame_index, t->pass_gpu_ms[0], t->pass_gpu_ms[1],
                   t->pass_gpu_ms[2], t->gpu_ms, t->cpu_record_ms);
        }
    }
    printf("timestamps %s\n", timed ? "measured" : "unavailable on this device (times read 0)");
    de__rg_reset(f->g);
    de__rg_destroy(f->g);
    f->g = NULL;
    de_gpu_fence_destroy(f->dev, frame_fence);
    de_gpu_buffer_destroy(f->dev, src);
    de_gpu_buffer_destroy(f->dev, dst);
    de_gpu_buffer_destroy(f->dev, rb_tex);
    de_gpu_buffer_destroy(f->dev, rb_buf);
    de_gpu_pipeline_destroy(f->dev, m.pipe);
#endif
}
