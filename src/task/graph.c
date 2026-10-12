/*
 * graph.c: reusable task graphs (SPEC §8). A graph is built once (add, depend), compiled once,
 * and run any number of times; the S05 ECS compiles each schedule into one and runs it every
 * frame.
 *
 * Build. Node descriptions and edges are appended to two arrays on the task system's allocator.
 * A node handle is (salt << 24) | (index + 1): the per-graph salt catches a handle used on the
 * wrong graph (in most cases). Add and depend cannot return an error, so a failure (out of
 * memory, a bad handle) is latched and reported by the next compile. Changing a compiled graph
 * marks it dirty; running a dirty graph asserts in Debug and Dev builds.
 *
 * Compile. Successor lists are built as CSR (counting sort by predecessor, duplicate edges
 * dropped), then Kahn's algorithm checks that every node is reachable from the roots. When it is
 * not, the graph has a cycle: every node Kahn could not take has a predecessor it could not take
 * either, so walking predecessors from such a node must repeat a node, and the repeat closes a
 * cycle. Its node names go into the DE_FAIL message. The result is one contiguous block: the
 * compiled nodes (template dependency count, live pending counter, successor range), then the
 * successor indices, then the root indices.
 *
 * Run. de_task_graph_run charges the done counter with the node count in one atomic add, resets
 * every pending counter from its template and submits the roots. graph_run_node runs the user
 * function, decrements each successor's pending counter once (acq_rel, so the predecessors'
 * writes are visible to the successor) and submits the successors it took to zero (frame nodes
 * onto the current worker's deque, main-thread nodes onto the main-thread queue). Every node task
 * carries `done` as its task counter, charged in advance by the run (de__task_submit_charged), so
 * the scheduler takes a node off `done` only after the node's function has returned and its
 * completion is counted: a waiter on `done` sees every node task finished, in the task
 * statistics too (DENG-388; before, the node decremented `done` inside its function).
 * Successors are always submitted before their predecessor's done decrement, so the done counter
 * cannot reach zero early. A run allocates nothing once the scheduler's record pools have grown
 * to the graph's width.
 *
 * Wait-and-help. A thread waiting on a counter (a graph run from inside a task, a nested
 * de_task_parallel_for) keeps running other tasks, so nested waits never idle a worker and never
 * deadlock fork-join work: every queued task sits in a deque, the injection queue or the
 * main-thread queue, and each waiting thread keeps draining its own deque, the injection queue
 * and (the main thread) the main-thread queue. Helping with any task can nest waits on one
 * thread without bound (a helped task waits and helps in turn), so the first
 * DE__TASK_MAX_WAIT_DEPTH (32) nested waits on a thread help with any task, and deeper waits stop
 * stealing from other workers: they run only their own deque, the injection queue and main-thread
 * tasks (scheduler.c, de_task_wait). Owners still drain their own deques, so this cannot strand a
 * task; it only bounds how much unrelated work piles onto one stack. A "help only with this
 * counter's tasks" mode was not needed: a Chase-Lev deque cannot skip foreign tasks without
 * popping them.
 */
#include "task/task_internal.h"
#include "task/profile.h"

#include <stdio.h>
#include <string.h>

#include "core/de_array.h"

/** Node index bits of a handle; the high 8 bits are the graph's salt. */
#define DE__GRAPH_INDEX_BITS 24u
#define DE__GRAPH_MAX_NODES  ((1u << DE__GRAPH_INDEX_BITS) - 1u)

#if defined(DE_DEBUG) || defined(DE_DEV)
#define DE__GRAPH_CHECKS 1
#else
#define DE__GRAPH_CHECKS 0
#endif

typedef struct de__graph_desc {
    const char *name;
    de_task_fn fn;
    void *user;
    u32 pool;
} de__graph_desc;

typedef struct de__graph_edge {
    u32 before;
    u32 after;
} de__graph_edge;

/** A compiled node. The task system passes a pointer to it to graph_run_node. */
typedef struct de__graph_node {
    de_task_fn fn;
    void *user;
    struct de_task_graph *graph;
    const char *name;
    u32 pool;
    u32 initial;    /* predecessors (template of `pending`) */
    u32 succ_begin; /* range in the successor array */
    u32 succ_count;
    de_atomic_u32 pending; /* predecessors not finished in the current run */
    /* Profile srcloc and statistics (DENG-293, profile.h). */
    de__task_node_prof prof;
} de__graph_node;

struct de_task_graph {
    de_task_system *ts;
    const de_allocator *a;
    DE_ARRAY(de__graph_desc) descs;
    DE_ARRAY(de__graph_edge) edges;

    /* The compiled block (one allocation) and the views into it. */
    void *block;
    size_t block_size;
    de__graph_node *nodes;
    u32 *succ;
    u32 *roots;
    u32 node_count;
    u32 root_count;

    /* The current run's done counter; written by run before the roots are submitted. */
    de__task_counter *done;

    de_result error;        /* latched by add/depend; DE_OK when none */
    const char *error_what; /* what failed (string literal) */
    u32 salt;
    bool compiled;
    bool dirty;
#if DE__GRAPH_CHECKS
    de_atomic_u32 nodes_left; /* nodes of the current run that have not finished */
#endif
};

static de_atomic_u32 de__graph_salt_seq;

/* ---------- Build ---------- */

static void de__graph_latch(de_task_graph *g, de_result r, const char *what) {
    if (g->error == DE_OK) {
        g->error = r;
        g->error_what = what;
    }
}

static void de__graph_modified(de_task_graph *g) {
    if (g->compiled)
        g->dirty = true;
}

de_result de_task_graph_create(de_task_system *ts, de_task_graph **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_task_graph_create: out is NULL");
    *out = NULL;
    if (ts == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_task_graph_create: ts is NULL");
    de_task_graph *g = DE_NEW(ts->allocator, de_task_graph, ts->tag);
    if (g == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_task_graph_create: graph");
    memset(g, 0, sizeof *g);
    g->ts = ts;
    g->a = ts->allocator;
    DE_ARRAY_INIT_TAG(&g->descs, de__graph_desc, g->a, ts->tag);
    DE_ARRAY_INIT_TAG(&g->edges, de__graph_edge, g->a, ts->tag);
    g->salt = de_atomic_fetch_add_u32(&de__graph_salt_seq, 1, DE_ATOMIC_RELAXED) % 255u + 1u;
    *out = g;
    return DE_OK;
}

static void de__graph_free_block(de_task_graph *g) {
    if (g->block != NULL)
        DE_FREE(g->a, g->block, g->block_size);
    g->block = NULL;
    g->block_size = 0;
    g->nodes = NULL;
    g->succ = NULL;
    g->roots = NULL;
    g->node_count = 0;
    g->root_count = 0;
    g->compiled = false;
}

static void de__graph_assert_idle(de_task_graph *g) {
#if DE__GRAPH_CHECKS
    DE_ASSERT(de_atomic_load_u32(&g->nodes_left, DE_ATOMIC_ACQUIRE) == 0 && "the graph is running");
#else
    (void)g;
#endif
}

void de_task_graph_destroy(de_task_graph *g) {
    if (g == NULL)
        return;
    de__graph_assert_idle(g);
    de__graph_free_block(g);
    de_array_free(&g->descs);
    de_array_free(&g->edges);
    DE_FREE(g->a, g, sizeof *g);
}

de_task_node de_task_graph_add(de_task_graph *g, const de_task_node_desc *desc) {
    de_task_node h = {0};
    DE_ASSERT(g != NULL);
    if (g == NULL)
        return h;
    de__graph_assert_idle(g);
    de__graph_modified(g);
    if (desc == NULL || desc->fn == NULL ||
        !(desc->pool == DE_TASK_POOL_FRAME || desc->pool == DE_TASK_POOL_BACKGROUND ||
          desc->pool == DE_TASK_POOL_MAIN_THREAD)) {
        de__graph_latch(g, DE_ERR_INVALID_ARGUMENT, "add: desc NULL, fn NULL or bad pool");
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_task_graph_add: desc NULL, fn NULL or bad pool");
        return h;
    }
    if (g->descs.count >= DE__GRAPH_MAX_NODES) {
        de__graph_latch(g, DE_ERR_INVALID_ARGUMENT, "add: too many nodes");
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_task_graph_add: more than %u nodes",
                      DE__GRAPH_MAX_NODES);
        return h;
    }
    const de__graph_desc d = {desc->name, desc->fn, desc->user, (u32)desc->pool};
    if (de_array_push(&g->descs, &d) != DE_OK) {
        de__graph_latch(g, DE_ERR_OUT_OF_MEMORY, "add: out of memory");
        return h;
    }
    h.id = (g->salt << DE__GRAPH_INDEX_BITS) | g->descs.count;
    return h;
}

/* The node index of `h` in `g`, or UINT32_MAX when `h` is not one of its handles. */
static u32 de__graph_index(const de_task_graph *g, de_task_node h) {
    const u32 i = h.id & DE__GRAPH_MAX_NODES;
    if ((h.id >> DE__GRAPH_INDEX_BITS) != g->salt || i == 0 || i > g->descs.count)
        return UINT32_MAX;
    return i - 1;
}

void de_task_graph_depend(de_task_graph *g, de_task_node before, de_task_node after) {
    DE_ASSERT(g != NULL);
    if (g == NULL)
        return;
    de__graph_assert_idle(g);
    de__graph_modified(g);
    const de__graph_edge e = {de__graph_index(g, before), de__graph_index(g, after)};
    if (e.before == UINT32_MAX || e.after == UINT32_MAX) {
        de__graph_latch(g, DE_ERR_INVALID_ARGUMENT, "depend: a handle is not a node of this graph");
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                      "de_task_graph_depend: handle %#x or %#x is not a node of this graph",
                      before.id, after.id);
        return;
    }
    if (de_array_push(&g->edges, &e) != DE_OK)
        de__graph_latch(g, DE_ERR_OUT_OF_MEMORY, "depend: out of memory");
}

/* ---------- Compile ---------- */

/* Appends `s` to the message buffer; marks truncation with "...". */
static void de__msg_append(char *buf, size_t cap, size_t *len, const char *s) {
    if (*len + 1 >= cap)
        return;
    const int n = snprintf(buf + *len, cap - *len, "%s", s);
    if (n < 0)
        return;
    if ((size_t)n >= cap - *len) {
        *len = cap - 1;
        memcpy(buf + cap - 4, "...", 4);
    } else {
        *len += (size_t)n;
    }
}

static void de__msg_append_node(char *buf, size_t cap, size_t *len, const de__graph_desc *d,
                                u32 i) {
    char tmp[32];
    const char *name = d[i].name;
    if (name == NULL) {
        snprintf(tmp, sizeof tmp, "#%u", i);
        name = tmp;
    }
    de__msg_append(buf, cap, len, name);
}

/* Finds a cycle among the nodes Kahn's algorithm left (`indeg[v] > 0`) and reports it. `pred_off`
 * and `pred` are the predecessor lists (CSR); `pos` is scratch of n entries. */
static de_result de__graph_report_cycle(const de_task_graph *g, const u32 *indeg,
                                        const u32 *pred_off, const u32 *pred, u32 *pos, u32 *path,
                                        u32 left) {
    const u32 n = g->descs.count;
    const de__graph_desc *d = DE_ARRAY_DATA((de_array *)&g->descs, de__graph_desc);
    u32 v = 0;
    while (indeg[v] == 0)
        ++v;
    for (u32 i = 0; i < n; ++i)
        pos[i] = UINT32_MAX;
    /* Walk predecessors that are left too; every left node has one. */
    u32 len = 0;
    while (pos[v] == UINT32_MAX) {
        pos[v] = len;
        path[len++] = v;
        u32 u = UINT32_MAX;
        for (u32 k = pred_off[v]; k < pred_off[v + 1]; ++k) {
            if (indeg[pred[k]] != 0) {
                u = pred[k];
                break;
            }
        }
        DE_ASSERT(u != UINT32_MAX);
        v = u;
    }
    /* path[j..len-1] with path[i+1] -> path[i] edges, and v = path[j] -> path[len-1]: in edge
     * order the cycle is path[j], path[len-1], path[len-2], ..., path[j+1], path[j]. */
    const u32 j = pos[v];
    char msg[DE_ERROR_MESSAGE_MAX - 64];
    size_t mlen = 0;
    msg[0] = '\0';
    de__msg_append_node(msg, sizeof msg, &mlen, d, path[j]);
    for (u32 i = len; i-- > j + 1;) {
        de__msg_append(msg, sizeof msg, &mlen, " -> ");
        de__msg_append_node(msg, sizeof msg, &mlen, d, path[i]);
    }
    de__msg_append(msg, sizeof msg, &mlen, " -> ");
    de__msg_append_node(msg, sizeof msg, &mlen, d, path[j]);
    return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                   "de_task_graph_compile: cycle: %s (%u of %u nodes are on or after a cycle)", msg,
                   left, n);
}

de_result de_task_graph_compile(de_task_graph *g) {
    if (g == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_task_graph_compile: graph is NULL");
    de__graph_assert_idle(g);
    if (g->error != DE_OK)
        return DE_FAIL(g->error, "de_task_graph_compile: an earlier %s (%s)", g->error_what,
                       de_result_str(g->error));
    de__graph_free_block(g);
    g->dirty = false;

    const u32 n = g->descs.count;
    const u32 e = g->edges.count;
    const de__graph_desc *descs = DE_ARRAY_DATA(&g->descs, de__graph_desc);
    const de__graph_edge *edges = DE_ARRAY_DATA(&g->edges, de__graph_edge);

    /* Scratch: succ_off[n+1], succ_tmp[e], mark[n], indeg[n], queue[n], pred_off[n+1], pred[e]. */
    const size_t scratch_words = (size_t)n * 6u + 2u + (size_t)e * 2u;
    const size_t scratch_size = scratch_words * sizeof(u32);
    u32 *scratch = (u32 *)DE_ALLOC(g->a, scratch_size > 0 ? scratch_size : sizeof(u32),
                                   DE_ALIGNOF(u32), g->ts->tag);
    if (scratch == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_task_graph_compile: scratch for %u nodes", n);
    u32 *succ_off = scratch;
    u32 *succ_tmp = succ_off + n + 1;
    u32 *mark = succ_tmp + e;
    u32 *indeg = mark + n;
    u32 *queue = indeg + n;
    u32 *pred_off = queue + n;
    u32 *pred = pred_off + n + 1;
    u32 *pos = mark; /* reused by the cycle report */

    /* Successor lists by counting sort on `before`, in edge order. */
    memset(succ_off, 0, sizeof(u32) * ((size_t)n + 1));
    for (u32 k = 0; k < e; ++k)
        ++succ_off[edges[k].before + 1];
    for (u32 i = 0; i < n; ++i)
        succ_off[i + 1] += succ_off[i];
    for (u32 i = 0; i < n; ++i)
        queue[i] = succ_off[i]; /* fill cursors */
    for (u32 k = 0; k < e; ++k)
        succ_tmp[queue[edges[k].before]++] = edges[k].after;

    /* Drop duplicate edges in place (mark[s] = last predecessor that listed s); count the
     * in-degrees of what is left. */
    for (u32 i = 0; i < n; ++i) {
        mark[i] = UINT32_MAX;
        indeg[i] = 0;
    }
    u32 kept = 0;
    for (u32 i = 0; i < n; ++i) {
        const u32 begin = succ_off[i], end = succ_off[i + 1];
        succ_off[i] = kept;
        for (u32 k = begin; k < end; ++k) {
            const u32 s = succ_tmp[k];
            if (mark[s] == i)
                continue;
            mark[s] = i;
            succ_tmp[kept++] = s;
            ++indeg[s];
        }
    }
    succ_off[n] = kept;

    /* Kahn's algorithm on a copy of the in-degrees (pred_off doubles as the copy). */
    u32 *deg = pred_off;
    u32 head = 0, tail = 0, roots = 0;
    for (u32 i = 0; i < n; ++i) {
        deg[i] = indeg[i];
        if (indeg[i] == 0) {
            queue[tail++] = i;
            ++roots;
        }
    }
    while (head < tail) {
        const u32 v = queue[head++];
        for (u32 k = succ_off[v]; k < succ_off[v + 1]; ++k) {
            if (--deg[succ_tmp[k]] == 0)
                queue[tail++] = succ_tmp[k];
        }
    }
    if (tail < n) {
        /* deg[v] > 0 marks the nodes left. Build the predecessor lists, then find a cycle. */
        u32 *left_deg = indeg; /* indeg is not needed any more */
        memcpy(left_deg, deg, sizeof(u32) * n);
        memset(pred_off, 0, sizeof(u32) * ((size_t)n + 1));
        for (u32 i = 0; i < n; ++i)
            for (u32 k = succ_off[i]; k < succ_off[i + 1]; ++k)
                ++pred_off[succ_tmp[k] + 1];
        for (u32 i = 0; i < n; ++i)
            pred_off[i + 1] += pred_off[i];
        for (u32 i = 0; i < n; ++i)
            queue[i] = pred_off[i];
        for (u32 i = 0; i < n; ++i)
            for (u32 k = succ_off[i]; k < succ_off[i + 1]; ++k)
                pred[queue[succ_tmp[k]]++] = i;
        /* queue is free again: it holds the walk path. */
        const de_result r =
            de__graph_report_cycle(g, left_deg, pred_off, pred, pos, queue, n - tail);
        DE_FREE(g->a, scratch, scratch_size > 0 ? scratch_size : sizeof(u32));
        return r;
    }

    /* The compiled block: nodes, successors, roots. */
    const size_t nodes_size = sizeof(de__graph_node) * n;
    const size_t block_size = nodes_size + sizeof(u32) * ((size_t)kept + roots);
    void *block = NULL;
    if (block_size > 0) {
        block = DE_ALLOC(g->a, block_size, DE_ALIGNOF(de__graph_node), g->ts->tag);
        if (block == NULL) {
            DE_FREE(g->a, scratch, scratch_size > 0 ? scratch_size : sizeof(u32));
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_task_graph_compile: %zu bytes for %u nodes",
                           block_size, n);
        }
    }
    g->block = block;
    g->block_size = block_size;
    g->nodes = (de__graph_node *)block;
    g->succ = block != NULL ? (u32 *)(void *)((u8 *)block + nodes_size) : NULL;
    g->roots = block != NULL ? g->succ + kept : NULL;
    g->node_count = n;
    g->root_count = roots;
    for (u32 i = 0; i < n; ++i) {
        de__graph_node *node = &g->nodes[i];
        node->fn = descs[i].fn;
        node->user = descs[i].user;
        node->graph = g;
        node->name = descs[i].name;
        node->pool = descs[i].pool;
        node->initial = indeg[i];
        node->succ_begin = succ_off[i];
        node->succ_count = succ_off[i + 1] - succ_off[i];
        de_atomic_init_u32(&node->pending, 0);
        de__task_node_prof_init(&node->prof, descs[i].name);
    }
    if (kept > 0)
        memcpy(g->succ, succ_tmp, sizeof(u32) * kept);
    /* Roots in node order (the queue holds them first). */
    for (u32 i = 0; i < roots; ++i)
        g->roots[i] = queue[i];
    DE_FREE(g->a, scratch, scratch_size > 0 ? scratch_size : sizeof(u32));
    g->compiled = true;
    return DE_OK;
}

/* ---------- Run ---------- */

/* Runs one node. Keep the node-run path in this one function: DENG-293 adds its profile hooks
 * here. */
static void graph_run_node(void *p) {
    de__graph_node *node = (de__graph_node *)p;
    de_task_graph *g = node->graph;
    de_task_system *ts = g->ts;
    de__task_counter *done = g->done;

    DE__TASK_NODE_BEGIN(prof_scope, &node->prof);
    node->fn(node->user);
    DE__TASK_NODE_END(prof_scope, &node->prof);

    de__graph_node *nodes = g->nodes;
    const u32 *succ = g->succ + node->succ_begin;
    for (u32 k = 0; k < node->succ_count; ++k) {
        de__graph_node *s = &nodes[succ[k]];
        if (de_atomic_fetch_sub_u32(&s->pending, 1, DE_ATOMIC_ACQ_REL) == 1)
            de__task_submit_charged(ts, (de_task_pool)s->pool, graph_run_node, s, done);
    }
#if DE__GRAPH_CHECKS
    de_atomic_fetch_sub_u32(&g->nodes_left, 1, DE_ATOMIC_RELEASE);
#endif
    /* Nothing touches the graph after this. The scheduler takes the node off `done` once this
     * function has returned and its completion is counted (de__task_execute); the waiter may
     * destroy the graph as soon as `done` reaches zero. */
}

void de_task_graph_run(de_task_graph *g, de_task_counter *done) {
    DE_ASSERT(g != NULL && done != NULL);
    DE_ASSERT(g->compiled && !g->dirty && "de_task_graph_run: compile the graph after changing it");
    de__graph_assert_idle(g);
    if (!g->compiled || g->dirty) {
        /* Release builds: compile now rather than run a stale graph. */
        DE_ERROR(g->ts->log, "de_task_graph_run: graph not compiled since its last change");
        if (de_task_graph_compile(g) != DE_OK)
            DE_PANIC("de_task_graph_run: compile failed: %s",
                     de_error_last() != NULL ? de_error_last()->message : "?");
    }
    const u32 n = g->node_count;
    if (n == 0)
        return;
    de__task_counter *c = de__counter(done);
    const u64 old = de_atomic_fetch_add_u64(&c->state, n, DE_ATOMIC_RELAXED);
    DE_ASSERT((u64)DE__COUNTER_PENDING(old) + n <= 0xFFFFFFFFull);
    (void)old;
    g->done = c;
#if DE__GRAPH_CHECKS
    de_atomic_store_u32(&g->nodes_left, n, DE_ATOMIC_RELAXED);
#endif
    de__graph_node *nodes = g->nodes;
    for (u32 i = 0; i < n; ++i)
        de_atomic_store_u32(&nodes[i].pending, nodes[i].initial, DE_ATOMIC_RELAXED);
    /* The submits publish the resets and `done` (deque push and queue push are releases). */
    for (u32 i = 0; i < g->root_count; ++i) {
        de__graph_node *r = &nodes[g->roots[i]];
        de__task_submit_charged(g->ts, (de_task_pool)r->pool, graph_run_node, r, c);
    }
}

/* ---------- Statistics (DENG-293) ---------- */

de_result de_task_graph_node_stats(const de_task_graph *g, de_task_node node,
                                   de_task_node_stats *out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_task_graph_node_stats: out is NULL");
    memset(out, 0, sizeof *out);
    out->last_worker = DE_TASK_WORKER_NONE;
    if (g == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_task_graph_node_stats: graph is NULL");
    const u32 i = de__graph_index(g, node);
    if (i == UINT32_MAX || !g->compiled || i >= g->node_count)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_task_graph_node_stats: %#x is not a compiled node of this graph",
                       node.id);
    if (!de__task_node_prof_read(&g->nodes[i].prof, out))
        return DE_FAIL(DE_ERR_UNSUPPORTED,
                       "de_task_graph_node_stats: statistics need a Debug or Dev build");
    return DE_OK;
}
