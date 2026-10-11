/* Tests for file watching (DENG-248).
 *
 * Each test works in its own directory under DE_IO_TEST_TMP_DIR (the build tree), emptied
 * first and mounted as scratch:/. Changes are made with native calls (stdio, rename) or with
 * the VFS (de_file_write_all_atomic). OS notification latency varies, so the tests poll in a
 * loop until the expected events arrive (with a generous timeout), then keep polling for a
 * quiet period to prove that nothing more comes. CTest runs every test in its own process. */
#include "de_test.h"

#include <dengine/de_debug.h>
#include <dengine/de_platform.h>

#include "platform/vfs_internal.h"
#include "platform/watch_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_IO_TEST_TMP_DIR
#error "DE_IO_TEST_TMP_DIR must be defined by tests/unit/platform/CMakeLists.txt"
#endif

/* Buffers for a test directory plus a relative path (larger than DE_FS_PATH_MAX, so snprintf
 * cannot truncate; GCC checks this with -Wformat-truncation). */
#define SUBPATH_MAX (DE_FS_PATH_MAX + 128)
#define MS          1000000ull
/* How long to wait for an expected event (FSEvents and loaded CI machines can be slow). */
#define WAIT_TIMEOUT_MS 10000u

#if DE__WATCH_ENABLED

/* ---------- recorder ---------- */

#define REC_MAX 64

typedef struct rec_event {
    char path[DE_FS_PATH_MAX];
    char old[DE_FS_PATH_MAX];
    bool has_old;
    de_watch_change change;
    bool is_dir;
    u64 time_ns;
    u64 thread;
} rec_event;

typedef struct recorder {
    rec_event ev[REC_MAX];
    u32 count;
    /* Thread checks (poll_thread_only): set around de_file_watch_poll by the test. */
    volatile bool in_poll;
    u64 poll_thread;
    u32 outside_poll; /* callbacks while in_poll was false */
    u32 wrong_thread; /* callbacks on another thread than poll_thread */
} recorder;

static void on_event(const de_watch_event *e, void *user) {
    recorder *r = (recorder *)user;
    if (!r->in_poll)
        r->outside_poll++;
    if (r->poll_thread != 0 && de_thread_current_id() != r->poll_thread)
        r->wrong_thread++;
    if (r->count < REC_MAX) {
        rec_event *x = &r->ev[r->count];
        snprintf(x->path, sizeof x->path, "%s", e->path);
        x->has_old = e->old_path != NULL;
        snprintf(x->old, sizeof x->old, "%s", e->old_path != NULL ? e->old_path : "");
        x->change = e->change;
        x->is_dir = e->is_dir;
        x->time_ns = de_time_now_ns();
        x->thread = de_thread_current_id();
    }
    r->count++;
}

static void poll_once(de_file_watch *w, recorder *r) {
    r->in_poll = true;
    (void)de_file_watch_poll(w);
    r->in_poll = false;
}

/* Polls until `want` events arrived (or the timeout), then for `quiet_ms` more. */
static void pump(de_file_watch *w, recorder *r, u32 want, u32 quiet_ms) {
    u64 deadline = de_time_now_ns() + (u64)WAIT_TIMEOUT_MS * MS;
    while (r->count < want && de_time_now_ns() < deadline) {
        poll_once(w, r);
        de_sleep_ns(2 * MS);
    }
    u64 quiet_end = de_time_now_ns() + (u64)quiet_ms * MS;
    while (de_time_now_ns() < quiet_end) {
        poll_once(w, r);
        de_sleep_ns(2 * MS);
    }
}

static int find_event(const recorder *r, const char *path, de_watch_change change) {
    for (u32 i = 0; i < r->count && i < REC_MAX; ++i) {
        if (r->ev[i].change == change && strcmp(r->ev[i].path, path) == 0)
            return (int)i;
    }
    return -1;
}

static void dump(const recorder *r) {
    static const char *names[] = {"CREATED", "MODIFIED", "DELETED", "RENAMED"};
    for (u32 i = 0; i < r->count && i < REC_MAX; ++i)
        fprintf(stderr, "  event %u: %s %s%s%s%s\n", (unsigned)i, names[r->ev[i].change],
                r->ev[i].path, r->ev[i].has_old ? " (from " : "", r->ev[i].old,
                r->ev[i].has_old ? ")" : "");
}

/* ---------- files ---------- */

static bool rm_entry(const char *name, bool is_dir, void *user);

/* Deletes everything inside the native directory `dir`. */
static void rm_tree(const char *dir) {
    (void)de__native_dir_list(dir, rm_entry, (void *)dir);
}

static bool rm_entry(const char *name, bool is_dir, void *user) {
    char child[SUBPATH_MAX];
    snprintf(child, sizeof child, "%s/%s", (const char *)user, name);
    if (is_dir)
        rm_tree(child);
    (void)de__native_remove(child);
    return true;
}

typedef struct watch_fx {
    char dir[DE_FS_PATH_MAX];
    de_file_watch *w;
    recorder *r;
} watch_fx;

static void watch_fx_setup(watch_fx *f) {
    f->r = (recorder *)calloc(1, sizeof *f->r);
    ASSERT_TRUE(f->r != NULL);
}

static void watch_fx_teardown(watch_fx *f) {
    de_file_watch_destroy(f->w);
    f->w = NULL;
    free(f->r);
}

/* Creates DE_IO_TEST_TMP_DIR/<name>, empty, and mounts it as scratch:/. */
static void fresh_dir(watch_fx *f, const char *name) {
    snprintf(f->dir, sizeof f->dir, "%s/%s", DE_IO_TEST_TMP_DIR, name);
    ASSERT_EQ(de__native_mkdirs(f->dir), DE_OK);
    rm_tree(f->dir);
    ASSERT_EQ(de_fs_mount("scratch", f->dir, DE_MOUNT_WRITABLE, 0), DE_OK);
}

static void native(const watch_fx *f, const char *rel, char *buf, size_t cap) {
    snprintf(buf, cap, "%s/%s", f->dir, rel);
}

static void write_file(const watch_fx *f, const char *rel, const char *text) {
    char p[SUBPATH_MAX];
    native(f, rel, p, sizeof p);
    de_file *file = NULL;
    ASSERT_EQ(de__file_open_native(p, DE_FILE_WRITE, de_heap_allocator(), &file), DE_OK);
    ASSERT_EQ(de_file_write(file, text, strlen(text)), DE_OK);
    de_file_close(file);
}

static void make_dir(const watch_fx *f, const char *rel) {
    char p[SUBPATH_MAX];
    native(f, rel, p, sizeof p);
    ASSERT_EQ(de__native_mkdirs(p), DE_OK);
}

static void remove_path(const watch_fx *f, const char *rel) {
    char p[SUBPATH_MAX];
    native(f, rel, p, sizeof p);
    ASSERT_EQ(de__native_remove(p), DE_OK);
}

static void rename_path(const watch_fx *f, const char *from, const char *to) {
    char a[SUBPATH_MAX], b[SUBPATH_MAX];
    native(f, from, a, sizeof a);
    native(f, to, b, sizeof b);
    ASSERT_EQ(de__native_rename_replace(a, b), DE_OK);
}

static void start(watch_fx *f, const char *vdir, bool recursive, u32 debounce_ms) {
    de_file_watch_desc d = {0};
    d.dir = vdir;
    d.recursive = recursive;
    d.fn = on_event;
    d.user = f->r;
    d.debounce_ms = debounce_ms;
    ASSERT_EQ(de_file_watch_create(&d, &f->w), DE_OK);
    ASSERT_TRUE(f->w != NULL);
}

/* ---------- tests ---------- */

TEST_F(watch, create, watch_fx) {
    fresh_dir(f, "create");
    start(f, "scratch:/", false, 100);
    write_file(f, "a.txt", "hello");
    make_dir(f, "d");
    u64 last_change = de_time_now_ns();
    pump(f->w, f->r, 2, 400);
    dump(f->r);
    ASSERT_EQ(f->r->count, 2u);
    int a = find_event(f->r, "scratch:/a.txt", DE_WATCH_CREATED);
    int d = find_event(f->r, "scratch:/d", DE_WATCH_CREATED);
    ASSERT_TRUE(a >= 0);
    ASSERT_TRUE(d >= 0);
    EXPECT_FALSE(f->r->ev[a].is_dir);
    EXPECT_TRUE(f->r->ev[d].is_dir);
    EXPECT_FALSE(f->r->ev[a].has_old);
    /* Never before the debounce window has passed since the last change. */
    EXPECT_GE(f->r->ev[a].time_ns, last_change + 100 * MS);
    EXPECT_LT(a, d); /* oldest first */
}

TEST_F(watch, modify_debounced, watch_fx) {
    fresh_dir(f, "modify");
    write_file(f, "a.txt", "v0");
    start(f, "scratch:/", true, 250);
    write_file(f, "a.txt", "v1");
    de_sleep_ns(8 * MS);
    write_file(f, "a.txt", "v2");
    de_sleep_ns(8 * MS);
    write_file(f, "a.txt", "v3");
    u64 last_write = de_time_now_ns();
    pump(f->w, f->r, 1, 600);
    dump(f->r);
    ASSERT_EQ(f->r->count, 1u);
    ASSERT_STREQ(f->r->ev[0].path, "scratch:/a.txt");
    ASSERT_EQ(f->r->ev[0].change, DE_WATCH_MODIFIED);
    EXPECT_GE(f->r->ev[0].time_ns, last_write + 250 * MS);
}

TEST_F(watch, delete, watch_fx) {
    fresh_dir(f, "delete");
    write_file(f, "a.txt", "a");
    write_file(f, "b.txt", "b");
    make_dir(f, "gone");
    start(f, "scratch:/", true, 100);
    remove_path(f, "a.txt");
    remove_path(f, "gone");
    /* Created and deleted within one window: nothing to report. */
    write_file(f, "brief.txt", "x");
    remove_path(f, "brief.txt");
    pump(f->w, f->r, 2, 500);
    dump(f->r);
    ASSERT_EQ(f->r->count, 2u);
    ASSERT_TRUE(find_event(f->r, "scratch:/a.txt", DE_WATCH_DELETED) >= 0);
    int g = find_event(f->r, "scratch:/gone", DE_WATCH_DELETED);
    ASSERT_TRUE(g >= 0);
    EXPECT_TRUE(f->r->ev[g].is_dir);
}

TEST_F(watch, rename, watch_fx) {
    fresh_dir(f, "rename");
    write_file(f, "a.txt", "a");
    make_dir(f, "d1");
    write_file(f, "d1/inside.txt", "i");
    start(f, "scratch:/", true, 100);
    rename_path(f, "a.txt", "b.txt");
    rename_path(f, "d1", "d2");
    pump(f->w, f->r, 2, 500);
    dump(f->r);
    ASSERT_EQ(f->r->count, 2u);
    int b = find_event(f->r, "scratch:/b.txt", DE_WATCH_RENAMED);
    ASSERT_TRUE(b >= 0);
    EXPECT_TRUE(f->r->ev[b].has_old);
    EXPECT_STREQ(f->r->ev[b].old, "scratch:/a.txt");
    EXPECT_FALSE(f->r->ev[b].is_dir);
    int d = find_event(f->r, "scratch:/d2", DE_WATCH_RENAMED);
    ASSERT_TRUE(d >= 0);
    EXPECT_STREQ(f->r->ev[d].old, "scratch:/d1");
    EXPECT_TRUE(f->r->ev[d].is_dir);

    /* Renamed out of the watched subtree: deleted; renamed in: created. */
    de_file_watch_destroy(f->w);
    f->w = NULL;
    memset(f->r, 0, sizeof *f->r);
    make_dir(f, "watched");
    make_dir(f, "outside");
    write_file(f, "watched/leaving.txt", "l");
    write_file(f, "outside/arriving.txt", "a");
    start(f, "scratch:/watched", true, 100);
    rename_path(f, "watched/leaving.txt", "outside/leaving.txt");
    rename_path(f, "outside/arriving.txt", "watched/arriving.txt");
    pump(f->w, f->r, 2, 500);
    dump(f->r);
    ASSERT_EQ(f->r->count, 2u);
    EXPECT_TRUE(find_event(f->r, "scratch:/watched/leaving.txt", DE_WATCH_DELETED) >= 0);
    EXPECT_TRUE(find_event(f->r, "scratch:/watched/arriving.txt", DE_WATCH_CREATED) >= 0);
}

TEST_F(watch, recursive, watch_fx) {
    fresh_dir(f, "recursive");
    make_dir(f, "sub/deep");
    write_file(f, "sub/deep/y.txt", "y0");
    start(f, "scratch:/", true, 100);
    write_file(f, "sub/deep/y.txt", "y1");
    pump(f->w, f->r, 1, 300);
    dump(f->r);
    ASSERT_EQ(f->r->count, 1u);
    ASSERT_STREQ(f->r->ev[0].path, "scratch:/sub/deep/y.txt");
    ASSERT_EQ(f->r->ev[0].change, DE_WATCH_MODIFIED);

    /* A new nested tree: every level is reported once (inotify adds watches as it goes). */
    memset(f->r, 0, sizeof *f->r);
    make_dir(f, "new");
    make_dir(f, "new/inner");
    write_file(f, "new/inner/z.txt", "z");
    pump(f->w, f->r, 3, 500);
    dump(f->r);
    ASSERT_EQ(f->r->count, 3u);
    EXPECT_TRUE(find_event(f->r, "scratch:/new", DE_WATCH_CREATED) >= 0);
    EXPECT_TRUE(find_event(f->r, "scratch:/new/inner", DE_WATCH_CREATED) >= 0);
    EXPECT_TRUE(find_event(f->r, "scratch:/new/inner/z.txt", DE_WATCH_CREATED) >= 0);
    /* And changes inside the new tree after that. */
    memset(f->r, 0, sizeof *f->r);
    write_file(f, "new/inner/z.txt", "z2");
    pump(f->w, f->r, 1, 300);
    dump(f->r);
    ASSERT_EQ(f->r->count, 1u);
    EXPECT_TRUE(find_event(f->r, "scratch:/new/inner/z.txt", DE_WATCH_MODIFIED) >= 0);

    /* A watch that is not recursive ignores the subdirectories. */
    de_file_watch_destroy(f->w);
    f->w = NULL;
    memset(f->r, 0, sizeof *f->r);
    start(f, "scratch:/", false, 100);
    write_file(f, "sub/deep/y.txt", "y2");
    write_file(f, "top.txt", "t");
    pump(f->w, f->r, 1, 500);
    dump(f->r);
    ASSERT_EQ(f->r->count, 1u);
    EXPECT_STREQ(f->r->ev[0].path, "scratch:/top.txt");
    EXPECT_EQ(f->r->ev[0].change, DE_WATCH_CREATED);

    /* A subdirectory as the watched directory: paths stay virtual, below it. */
    de_file_watch_destroy(f->w);
    f->w = NULL;
    memset(f->r, 0, sizeof *f->r);
    start(f, "scratch:/sub/./deep/", true, 100);
    write_file(f, "sub/deep/y.txt", "y3");
    pump(f->w, f->r, 1, 300);
    dump(f->r);
    ASSERT_EQ(f->r->count, 1u);
    EXPECT_STREQ(f->r->ev[0].path, "scratch:/sub/deep/y.txt");
}

TEST_F(watch, atomic_save, watch_fx) {
    fresh_dir(f, "atomic");
    write_file(f, "target.txt", "old contents");
    start(f, "scratch:/", true, 100);
    const char text[] = "new contents";
    ASSERT_EQ(de_file_write_all_atomic("scratch:/target.txt", text, sizeof text - 1), DE_OK);
    pump(f->w, f->r, 1, 500);
    dump(f->r);
    /* One MODIFIED for the target, nothing for the temporary file. */
    ASSERT_EQ(f->r->count, 1u);
    ASSERT_STREQ(f->r->ev[0].path, "scratch:/target.txt");
    ASSERT_EQ(f->r->ev[0].change, DE_WATCH_MODIFIED);

    /* The same pattern for a file that did not exist: one event, MODIFIED (documented). */
    memset(f->r, 0, sizeof *f->r);
    ASSERT_EQ(de_file_write_all_atomic("scratch:/fresh.txt", text, sizeof text - 1), DE_OK);
    pump(f->w, f->r, 1, 500);
    dump(f->r);
    ASSERT_EQ(f->r->count, 1u);
    ASSERT_STREQ(f->r->ev[0].path, "scratch:/fresh.txt");
    ASSERT_EQ(f->r->ev[0].change, DE_WATCH_MODIFIED);
}

typedef struct poll_thread_ctx {
    de_file_watch *w;
    recorder *r;
    u64 id;
} poll_thread_ctx;

static i32 poll_thread(void *user) {
    poll_thread_ctx *c = (poll_thread_ctx *)user;
    c->id = de_thread_current_id();
    c->r->poll_thread = c->id;
    u64 deadline = de_time_now_ns() + (u64)WAIT_TIMEOUT_MS * MS;
    while (c->r->count < 2 && de_time_now_ns() < deadline) {
        poll_once(c->w, c->r);
        de_sleep_ns(2 * MS);
    }
    u64 quiet_end = de_time_now_ns() + 300 * MS;
    while (de_time_now_ns() < quiet_end) {
        poll_once(c->w, c->r);
        de_sleep_ns(2 * MS);
    }
    return 0;
}

TEST_F(watch, poll_thread_only, watch_fx) {
    fresh_dir(f, "poll_thread");
    start(f, "scratch:/", true, 50);
    write_file(f, "one.txt", "1");
    write_file(f, "two.txt", "2");
    /* Long past the debounce window and the OS latency, without polling: no callback. */
    u64 deadline = de_time_now_ns() + (u64)WAIT_TIMEOUT_MS * MS;
    while (de__watch_pending_count(f->w) < 2 && de_time_now_ns() < deadline)
        de_sleep_ns(2 * MS);
    ASSERT_GE(de__watch_pending_count(f->w), 2u);
    de_sleep_ns(200 * MS);
    ASSERT_EQ(f->r->count, 0u);

    /* Poll from another thread: the callbacks run there, inside de_file_watch_poll. */
    poll_thread_ctx c = {f->w, f->r, 0};
    de_thread *t = NULL;
    ASSERT_EQ(de_thread_create(poll_thread, &c, "watch-poller", DE_THREAD_NORMAL, 0, &t), DE_OK);
    de_thread_join(t);
    dump(f->r);
    ASSERT_EQ(f->r->count, 2u);
    EXPECT_NE(c.id, de_thread_current_id());
    EXPECT_EQ(f->r->ev[0].thread, c.id);
    EXPECT_EQ(f->r->ev[1].thread, c.id);
    EXPECT_EQ(f->r->outside_poll, 0u);
    EXPECT_EQ(f->r->wrong_thread, 0u);
    EXPECT_EQ(de__watch_pending_count(f->w), 0u);
}

/* ---------- counting allocator (leak check for destroy_pending) ---------- */

typedef struct counting {
    const de_allocator *base;
    i64 bytes;
    i64 blocks;
} counting;

static void *cnt_alloc(void *user, size_t size, size_t align, u32 tag) {
    counting *c = (counting *)user;
    void *p = DE_ALLOC(c->base, size, align, tag);
    if (p != NULL) {
        c->bytes += (i64)size;
        c->blocks++;
    }
    return p;
}

static void *cnt_realloc(void *user, void *ptr, size_t old_size, size_t new_size, size_t align,
                         u32 tag) {
    counting *c = (counting *)user;
    void *p = DE_REALLOC(c->base, ptr, old_size, new_size, align, tag);
    if (p != NULL) {
        c->bytes += (i64)new_size - (ptr != NULL ? (i64)old_size : 0);
        if (ptr == NULL)
            c->blocks++;
    }
    return p;
}

static void cnt_free(void *user, void *ptr, size_t size) {
    counting *c = (counting *)user;
    if (ptr == NULL)
        return;
    c->bytes -= (i64)size;
    c->blocks--;
    DE_FREE(c->base, ptr, size);
}

TEST_F(watch, destroy_pending, watch_fx) {
    fresh_dir(f, "destroy_pending");
    counting cnt = {de_heap_allocator(), 0, 0};
    de_allocator a = {cnt_alloc, cnt_realloc, cnt_free, &cnt};
    for (int round = 0; round < 3; ++round) {
        de_file_watch_desc d = {0};
        d.dir = "scratch:/";
        d.recursive = true;
        d.fn = on_event;
        d.user = f->r;
        d.debounce_ms = 60000; /* nothing matures */
        d.allocator = &a;
        de_file_watch *w = NULL;
        ASSERT_EQ(de_file_watch_create(&d, &w), DE_OK);
        ASSERT_GT(cnt.blocks, 0);
        char name[64];
        for (int i = 0; i < 40; ++i) {
            snprintf(name, sizeof name, "r%d_f%d.txt", round, i);
            write_file(f, name, "pending");
        }
        u64 deadline = de_time_now_ns() + (u64)WAIT_TIMEOUT_MS * MS;
        while (de__watch_pending_count(w) == 0 && de_time_now_ns() < deadline)
            de_sleep_ns(1 * MS);
        ASSERT_GT(de__watch_pending_count(w), 0u);
        poll_once(w, f->r); /* nothing is old enough */
        /* Changes keep arriving while it is destroyed. */
        for (int i = 0; i < 20; ++i) {
            snprintf(name, sizeof name, "r%d_f%d.txt", round, i);
            write_file(f, name, "again");
        }
        de_file_watch_destroy(w);
        EXPECT_EQ(cnt.bytes, 0);
        EXPECT_EQ(cnt.blocks, 0);
    }
    EXPECT_EQ(f->r->count, 0u);
}

TEST_F(watch, invalid_args, watch_fx) {
    fresh_dir(f, "invalid");
    write_file(f, "file.txt", "x");
    de_file_watch *w = (de_file_watch *)(uintptr_t)1;
    de_file_watch_desc d = {0};
    EXPECT_EQ(de_file_watch_create(NULL, &w), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(w == NULL);
    EXPECT_EQ(de_file_watch_create(&d, &w), DE_ERR_INVALID_ARGUMENT); /* no dir, no fn */
    d.fn = on_event;
    d.dir = "scratch:/";
    EXPECT_EQ(de_file_watch_create(&d, NULL), DE_ERR_INVALID_ARGUMENT);
    d.dir = "/etc";
    EXPECT_EQ(de_file_watch_create(&d, &w), DE_ERR_INVALID_ARGUMENT);
    d.dir = "scratch:/../x";
    EXPECT_EQ(de_file_watch_create(&d, &w), DE_ERR_INVALID_ARGUMENT);
    d.dir = "scratch:/missing";
    EXPECT_EQ(de_file_watch_create(&d, &w), DE_ERR_NOT_FOUND);
    d.dir = "scratch:/file.txt";
    EXPECT_EQ(de_file_watch_create(&d, &w), DE_ERR_INVALID_ARGUMENT);
    d.dir = "nosuchroot:/";
    EXPECT_EQ(de_file_watch_create(&d, &w), DE_ERR_NOT_FOUND);
    EXPECT_TRUE(w == NULL);
    /* NULL watches are ignored. */
    EXPECT_EQ(de_file_watch_poll(NULL), 0u);
    de_file_watch_destroy(NULL);
}

TEST_F(watch, debounce_cvar, watch_fx) {
    fresh_dir(f, "cvar");
    EXPECT_EQ(de_cvar_find("fs.watch_debounce_ms").id, 0u); /* registered lazily */
    start(f, "scratch:/", false, 0);
    de_cvar cv = de_cvar_find("fs.watch_debounce_ms");
    ASSERT_NE(cv.id, 0u);
    EXPECT_EQ(de_cvar_int(cv), (i64)DE_WATCH_DEBOUNCE_DEFAULT_MS);
    /* The window is read at every poll. */
    ASSERT_EQ(de_cvar_set(cv, "300"), DE_OK);
    write_file(f, "a.txt", "a");
    u64 changed = de_time_now_ns();
    pump(f->w, f->r, 1, 100);
    ASSERT_EQ(f->r->count, 1u);
    EXPECT_GE(f->r->ev[0].time_ns, changed + 300 * MS);
    /* A second watch reuses the cvar. */
    de_file_watch *w2 = NULL;
    de_file_watch_desc d = {0};
    d.dir = "scratch:/";
    d.fn = on_event;
    d.user = f->r;
    ASSERT_EQ(de_file_watch_create(&d, &w2), DE_OK);
    EXPECT_EQ(de_cvar_find("fs.watch_debounce_ms").id, cv.id);
    EXPECT_EQ(de_cvar_int(cv), 300);
    de_file_watch_destroy(w2);
}

#else /* Release, Shipping */

TEST(watch, unsupported_build) {
    de_file_watch_desc d = {0};
    d.dir = "scratch:/";
    de_file_watch *w = (de_file_watch *)(uintptr_t)1;
    EXPECT_EQ(de_file_watch_create(&d, &w), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(w == NULL);
    EXPECT_EQ(de_file_watch_poll(NULL), 0u);
    de_file_watch_destroy(NULL);
}

#endif
