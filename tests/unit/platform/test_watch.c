/* Tests for file watching (DENG-248).
 *
 * Each test works in its own directory under DE_IO_TEST_TMP_DIR (the build tree), emptied
 * first and mounted as scratch:/. Changes are made with native calls (stdio, rename) or with
 * the VFS (de_file_write_all_atomic). OS notification latency varies, so the tests poll in a
 * loop until the expected events arrive (with a generous timeout), then keep polling for a
 * quiet period to prove that nothing more comes. CTest runs every test in its own process.
 *
 * Debounce bounds take the time just BEFORE the change that starts the window: the backend
 * stamps a change when its thread posts it, which can be before the call that made the change
 * returns (inotify posts at close(), inside write_file). A time taken after the call is not a
 * lower bound (DENG-370: 0.22 ms too late under ASan on CI). */
#if defined(__APPLE__)
#include <CoreServices/CoreServices.h> /* kFSEventStreamEventFlag* for watch.fsevents_transient */
#endif

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
    u64 a_start = de_time_now_ns();
    write_file(f, "a.txt", "hello");
    u64 d_start = de_time_now_ns();
    make_dir(f, "d"); /* a one-letter name: the shortest ReadDirectoryChangesW record */
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
    /* Never before the debounce window has passed since the path's last change. */
    EXPECT_GE(f->r->ev[a].time_ns, a_start + 100 * MS);
    EXPECT_GE(f->r->ev[d].time_ns, d_start + 100 * MS);
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
    u64 last_write = de_time_now_ns(); /* before the write: see the top of the file */
    write_file(f, "a.txt", "v3");
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
    make_dir(f, "gone/inner");
    write_file(f, "gone/inner/x.txt", "x");
    start(f, "scratch:/", true, 100);
    remove_path(f, "a.txt");
    /* A tree that existed before the watch: Windows cannot stat a deleted path, so is_dir comes
     * from the directories its backend knows (DENG-370). */
    remove_path(f, "gone/inner/x.txt");
    remove_path(f, "gone/inner");
    remove_path(f, "gone");
    /* Created and deleted within one window: nothing to report. */
    write_file(f, "brief.txt", "x");
    remove_path(f, "brief.txt");
    make_dir(f, "brief_dir");
    remove_path(f, "brief_dir");
    pump(f->w, f->r, 4, 500);
    dump(f->r);
    ASSERT_EQ(f->r->count, 4u);
    int a = find_event(f->r, "scratch:/a.txt", DE_WATCH_DELETED);
    ASSERT_TRUE(a >= 0);
    EXPECT_FALSE(f->r->ev[a].is_dir);
    int x = find_event(f->r, "scratch:/gone/inner/x.txt", DE_WATCH_DELETED);
    ASSERT_TRUE(x >= 0);
    EXPECT_FALSE(f->r->ev[x].is_dir);
    int i = find_event(f->r, "scratch:/gone/inner", DE_WATCH_DELETED);
    ASSERT_TRUE(i >= 0);
    EXPECT_TRUE(f->r->ev[i].is_dir);
    int g = find_event(f->r, "scratch:/gone", DE_WATCH_DELETED);
    ASSERT_TRUE(g >= 0);
    EXPECT_TRUE(f->r->ev[g].is_dir);

    /* A directory made and moved after the start is known too: deleting it reports is_dir. */
    de_file_watch_destroy(f->w);
    f->w = NULL;
    memset(f->r, 0, sizeof *f->r);
    start(f, "scratch:/", true, 100);
    make_dir(f, "later/sub");
    pump(f->w, f->r, 2, 300);
    memset(f->r, 0, sizeof *f->r);
    rename_path(f, "later", "moved");
    pump(f->w, f->r, 1, 300);
    memset(f->r, 0, sizeof *f->r);
    remove_path(f, "moved/sub");
    remove_path(f, "moved");
    pump(f->w, f->r, 2, 500);
    dump(f->r);
    ASSERT_EQ(f->r->count, 2u);
    int s = find_event(f->r, "scratch:/moved/sub", DE_WATCH_DELETED);
    int m = find_event(f->r, "scratch:/moved", DE_WATCH_DELETED);
    ASSERT_TRUE(s >= 0);
    ASSERT_TRUE(m >= 0);
    EXPECT_TRUE(f->r->ev[s].is_dir);
    EXPECT_TRUE(f->r->ev[m].is_dir);
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

/* ---------- merge rules, fed synthetic backend changes (DENG-371) ---------- */

static void post(de_file_watch *w, de__watch_raw kind, const char *rel, bool is_dir, u64 cookie) {
    de__watch_post(w, kind, rel, strlen(rel), is_dir, cookie);
}

/* A path created and deleted in one window is reported never, whatever the backend repeats.
 * The changes go straight into the common layer through the internal header, so the rules are
 * checked without depending on OS event timing. The real backend watches an empty directory
 * that nothing changes, so it adds nothing. */
TEST_F(watch, merge_transient, watch_fx) {
    fresh_dir(f, "merge_transient");
    start(f, "scratch:/", true, 200);
    de_file_watch *w = f->w;
    /* Create and remove reported together (one event with both flags). */
    post(w, DE__WATCH_RAW_CREATED, "coalesced.txt", false, 0);
    post(w, DE__WATCH_RAW_DELETED, "coalesced.txt", false, 0);
    /* Split over separate deliveries, and the removal reported twice (DENG-371: FSEvents gave
     * `Created` after the file was gone, then `Created|Removed`). */
    post(w, DE__WATCH_RAW_CREATED, "split.txt", false, 0);
    post(w, DE__WATCH_RAW_DELETED, "split.txt", false, 0);
    de_sleep_ns(20 * MS);
    post(w, DE__WATCH_RAW_DELETED, "split.txt", false, 0);
    post(w, DE__WATCH_RAW_CREATED, "split_dir", true, 0);
    post(w, DE__WATCH_RAW_DELETED, "split_dir", true, 0);
    de_sleep_ns(20 * MS);
    post(w, DE__WATCH_RAW_DELETED, "split_dir", true, 0);
    /* Gone, then the rename it turned out to be: an atomic save of a new target, MODIFIED as
     * documented and not RENAMED from the temporary (S03 exit run under load). The temporary
     * reports nothing, also when its removal repeats after the rename. */
    post(w, DE__WATCH_RAW_CREATED, "tmp.txt", false, 0);
    post(w, DE__WATCH_RAW_DELETED, "tmp.txt", false, 0);
    post(w, DE__WATCH_RAW_MOVED_FROM, "tmp.txt", false, 71);
    post(w, DE__WATCH_RAW_MOVED_TO, "saved.txt", false, 71);
    post(w, DE__WATCH_RAW_DELETED, "tmp.txt", false, 0);
    /* Gone, then written again: new to the consumer. */
    post(w, DE__WATCH_RAW_CREATED, "again.txt", false, 0);
    post(w, DE__WATCH_RAW_DELETED, "again.txt", false, 0);
    post(w, DE__WATCH_RAW_MODIFIED, "again.txt", false, 0);
    /* Gone, then created again. */
    post(w, DE__WATCH_RAW_CREATED, "back.txt", false, 0);
    post(w, DE__WATCH_RAW_DELETED, "back.txt", false, 0);
    post(w, DE__WATCH_RAW_CREATED, "back.txt", false, 0);
    /* A path that existed before: its removal is reported, once, even when repeated. */
    post(w, DE__WATCH_RAW_DELETED, "old.txt", false, 0);
    post(w, DE__WATCH_RAW_DELETED, "old.txt", false, 0);
    pump(w, f->r, 4, 400);
    dump(f->r);
    ASSERT_EQ(f->r->count, 4u);
    EXPECT_TRUE(find_event(f->r, "scratch:/saved.txt", DE_WATCH_MODIFIED) >= 0);
    EXPECT_TRUE(find_event(f->r, "scratch:/again.txt", DE_WATCH_CREATED) >= 0);
    EXPECT_TRUE(find_event(f->r, "scratch:/back.txt", DE_WATCH_CREATED) >= 0);
    int old = find_event(f->r, "scratch:/old.txt", DE_WATCH_DELETED);
    ASSERT_TRUE(old >= 0);
    EXPECT_FALSE(f->r->ev[old].is_dir);
    EXPECT_EQ(de__watch_pending_count(w), 0u);
}

/* A new file whose writer still has it open is held past the debounce window until the next
 * change of its path (DENG-383). On Linux CI, the fsync in de_file_write_all_atomic took up to
 * 167 ms under I/O load: the temporary's CREATED was delivered before the rename, which then
 * came out as RENAMED from the temporary. Synthetic posts, so the rules are checked without OS
 * timing. */
TEST_F(watch, merge_held, watch_fx) {
    fresh_dir(f, "merge_held");
    start(f, "scratch:/", true, 50);
    de_file_watch *w = f->w;
    /* Created and still open, for five windows: nothing is delivered. */
    post(w, DE__WATCH_RAW_CREATED_OPEN, "save.txt.tmp", false, 0);
    post(w, DE__WATCH_RAW_CREATED_OPEN, "new.txt", false, 0);
    post(w, DE__WATCH_RAW_CREATED_OPEN, "gone.tmp", false, 0);
    pump(w, f->r, 0, 250);
    dump(f->r);
    ASSERT_EQ(f->r->count, 0u);
    ASSERT_EQ(de__watch_pending_count(w), 3u);
    /* The writers close: the temporary is renamed over a new target (an atomic save, one
     * MODIFIED), a file stays (CREATED), a file is removed (nothing). The window counts from
     * the close. */
    u64 closed = de_time_now_ns();
    post(w, DE__WATCH_RAW_MODIFIED, "save.txt.tmp", false, 0);
    post(w, DE__WATCH_RAW_MOVED_FROM, "save.txt.tmp", false, 83);
    post(w, DE__WATCH_RAW_MOVED_TO, "save.txt", false, 83);
    post(w, DE__WATCH_RAW_MODIFIED, "new.txt", false, 0);
    post(w, DE__WATCH_RAW_DELETED, "gone.tmp", false, 0);
    pump(w, f->r, 2, 300);
    dump(f->r);
    ASSERT_EQ(f->r->count, 2u);
    int saved = find_event(f->r, "scratch:/save.txt", DE_WATCH_MODIFIED);
    int created = find_event(f->r, "scratch:/new.txt", DE_WATCH_CREATED);
    ASSERT_TRUE(saved >= 0);
    ASSERT_TRUE(created >= 0);
    EXPECT_GE(f->r->ev[saved].time_ns, closed + 50 * MS);
    EXPECT_GE(f->r->ev[created].time_ns, closed + 50 * MS);
    EXPECT_EQ(de__watch_pending_count(w), 0u);
    /* A writer that never closes: CREATED once the hold limit passes. */
    memset(f->r, 0, sizeof *f->r);
    de__watch_set_hold_limit(w, 200);
    u64 opened = de_time_now_ns();
    post(w, DE__WATCH_RAW_CREATED_OPEN, "kept_open.log", false, 0);
    pump(w, f->r, 1, 100);
    dump(f->r);
    ASSERT_EQ(f->r->count, 1u);
    EXPECT_STREQ(f->r->ev[0].path, "scratch:/kept_open.log");
    EXPECT_EQ(f->r->ev[0].change, DE_WATCH_CREATED);
    EXPECT_GE(f->r->ev[0].time_ns, opened + 200 * MS);
    EXPECT_EQ(de__watch_pending_count(w), 0u);
}

/* The two halves of a rename that the backend reports as a certain pair (Windows) arrive
 * several windows apart, as when a pending ReadDirectoryChangesW completes on the first record
 * and a busy machine delays the watcher thread before the second (DENG-385). The source is
 * held until its MOVED_TO arrives, so an atomic save is still one MODIFIED of the target. */
TEST_F(watch, merge_pair_held, watch_fx) {
    fresh_dir(f, "merge_pair_held");
    start(f, "scratch:/", true, 50);
    de_file_watch *w = f->w;
    /* An atomic save of a new target and a plain rename, each split after its first half. */
    post(w, DE__WATCH_RAW_CREATED, "fresh.txt.tmp", false, 0);
    post(w, DE__WATCH_RAW_MODIFIED, "fresh.txt.tmp", false, 0);
    post(w, DE__WATCH_RAW_MOVED_FROM_PAIRED, "fresh.txt.tmp", false, 41);
    post(w, DE__WATCH_RAW_MOVED_FROM_PAIRED, "old.txt", false, 42);
    pump(w, f->r, 0, 250); /* five windows: nothing is delivered */
    dump(f->r);
    ASSERT_EQ(f->r->count, 0u);
    ASSERT_EQ(de__watch_pending_count(w), 2u);
    u64 paired = de_time_now_ns();
    post(w, DE__WATCH_RAW_MOVED_TO, "fresh.txt", false, 41);
    post(w, DE__WATCH_RAW_MOVED_TO, "new.txt", false, 42);
    pump(w, f->r, 2, 300);
    dump(f->r);
    ASSERT_EQ(f->r->count, 2u);
    int saved = find_event(f->r, "scratch:/fresh.txt", DE_WATCH_MODIFIED);
    int renamed = find_event(f->r, "scratch:/new.txt", DE_WATCH_RENAMED);
    ASSERT_TRUE(saved >= 0);
    ASSERT_TRUE(renamed >= 0);
    EXPECT_STREQ(f->r->ev[renamed].old, "scratch:/old.txt");
    EXPECT_GE(f->r->ev[saved].time_ns, paired + 50 * MS);
    EXPECT_EQ(de__watch_pending_count(w), 0u);
    /* A pair whose second half is lost (an overflow): DELETED once the hold limit passes. */
    memset(f->r, 0, sizeof *f->r);
    de__watch_set_hold_limit(w, 200);
    u64 moved = de_time_now_ns();
    post(w, DE__WATCH_RAW_MOVED_FROM_PAIRED, "lost.txt", false, 43);
    pump(w, f->r, 1, 100);
    dump(f->r);
    ASSERT_EQ(f->r->count, 1u);
    EXPECT_STREQ(f->r->ev[0].path, "scratch:/lost.txt");
    EXPECT_EQ(f->r->ev[0].change, DE_WATCH_DELETED);
    EXPECT_GE(f->r->ev[0].time_ns, moved + 200 * MS);
    EXPECT_EQ(de__watch_pending_count(w), 0u);
}

/* A backend thread delayed part-way through one OS report (the CI trace: the Windows watcher
 * posted an atomic save's temporary, then stalled for 400 ms before the rest of the same
 * report). While its batch is open, poll delivers nothing, so the save is one MODIFIED of the
 * target (DENG-385). */
TEST_F(watch, merge_batch, watch_fx) {
    fresh_dir(f, "merge_batch");
    start(f, "scratch:/", true, 50);
    de_file_watch *w = f->w;
    post(w, DE__WATCH_RAW_MODIFIED, "before.txt", false, 0); /* already pending */
    de__watch_batch(w, true);
    post(w, DE__WATCH_RAW_CREATED, "target.txt.tmp", false, 0);
    pump(w, f->r, 0, 250); /* five windows with the batch open: nothing is delivered */
    dump(f->r);
    ASSERT_EQ(f->r->count, 0u);
    post(w, DE__WATCH_RAW_MODIFIED, "target.txt.tmp", false, 0);
    post(w, DE__WATCH_RAW_DELETED, "target.txt", false, 0);
    post(w, DE__WATCH_RAW_MOVED_FROM, "target.txt.tmp", false, 51);
    post(w, DE__WATCH_RAW_MOVED_TO, "target.txt", false, 51);
    u64 closed = de_time_now_ns();
    de__watch_batch(w, false);
    pump(w, f->r, 2, 300);
    dump(f->r);
    ASSERT_EQ(f->r->count, 2u);
    EXPECT_STREQ(f->r->ev[0].path, "scratch:/before.txt"); /* matured first, delivered first */
    EXPECT_EQ(f->r->ev[0].change, DE_WATCH_MODIFIED);
    EXPECT_STREQ(f->r->ev[1].path, "scratch:/target.txt");
    EXPECT_EQ(f->r->ev[1].change, DE_WATCH_MODIFIED);
    EXPECT_GE(f->r->ev[1].time_ns, closed);
    EXPECT_EQ(de__watch_pending_count(w), 0u);
}

#if defined(__linux__)
/* Opens `rel` for writing (created), writes `text` and keeps it open. */
static de_file *open_writer(const watch_fx *f, const char *rel, const char *text) {
    char p[SUBPATH_MAX];
    native(f, rel, p, sizeof p);
    de_file *file = NULL;
    EXPECT_EQ(de__file_open_native(p, DE_FILE_WRITE, de_heap_allocator(), &file), DE_OK);
    if (file != NULL)
        EXPECT_EQ(de_file_write(file, text, strlen(text)), DE_OK);
    return file;
}

/* An atomic save whose writer keeps the temporary open for longer than the debounce window,
 * as a slow fsync does under I/O load (DENG-383), is still one MODIFIED for the target. inotify
 * reports the writer's close; FSEvents and ReadDirectoryChangesW do not, so this is Linux only
 * (platform: File Watching, known gaps). */
TEST_F(watch, atomic_save_slow_writer, watch_fx) {
    fresh_dir(f, "atomic_slow");
    write_file(f, "target.txt", "old contents");
    start(f, "scratch:/", true, 100);
    const char *targets[] = {"target.txt", "fresh.txt"}; /* existing, then new */
    for (int i = 0; i < 2; ++i) {
        memset(f->r, 0, sizeof *f->r);
        char tmp[64], vpath[64];
        snprintf(tmp, sizeof tmp, "%s.tmp-slow", targets[i]);
        snprintf(vpath, sizeof vpath, "scratch:/%s", targets[i]);
        de_file *file = open_writer(f, tmp, "new contents");
        ASSERT_TRUE(file != NULL);
        pump(f->w, f->r, 0, 400); /* four windows with the temporary open */
        u32 early = f->r->count;
        de_file_close(file);
        rename_path(f, tmp, targets[i]);
        pump(f->w, f->r, 1, 500);
        dump(f->r);
        ASSERT_EQ(early, 0u);
        ASSERT_EQ(f->r->count, 1u);
        ASSERT_STREQ(f->r->ev[0].path, vpath);
        ASSERT_EQ(f->r->ev[0].change, DE_WATCH_MODIFIED);
    }
}
#endif

#if defined(__APPLE__)
/* File IDs far above any the volume has handed out: "created after the start" to the backend,
 * and never on disk, so lstat fails as for a file that is already gone. */
#define FAKE_INO (1ull << 52)

static void inject(de_file_watch *w, const char *rel, u32 flags, u64 ino) {
    de__watch_fsevents_inject(w, rel, flags, ino);
}

/* The FSEvents classification of a file created and deleted at once (DENG-371). With the
 * sticky flags, its events are `Created|Modified` and then `Created|Modified|Removed`, the same
 * inode on both. On the CI runner the first was handled after the file was gone. Here the
 * second event also comes after the debounce window, when the common layer has already dropped
 * the path, so only the backend can tell that its removal was reported already. */
TEST_F(watch, fsevents_transient, watch_fx) {
    const u32 created = kFSEventStreamEventFlagItemCreated;
    const u32 removed = kFSEventStreamEventFlagItemRemoved;
    const u32 renamed = kFSEventStreamEventFlagItemRenamed;
    const u32 modified = kFSEventStreamEventFlagItemModified;
    const u32 is_file = kFSEventStreamEventFlagItemIsFile;
    const u32 is_dir = kFSEventStreamEventFlagItemIsDir;
    fresh_dir(f, "fse_transient");
    start(f, "scratch:/", true, 100);
    de_file_watch *w = f->w;
    inject(w, "brief.txt", created | modified | is_file, FAKE_INO + 1);
    inject(w, "brief_dir", created | is_dir, FAKE_INO + 2);
    inject(w, "tmp.txt", created | modified | is_file, FAKE_INO + 3);
    /* A file that existed before the start, removed: reported, once. */
    inject(w, "old.txt", removed | is_file, FAKE_INO + 4);
    pump(w, f->r, 1, 300); /* every window above has passed and was delivered */
    dump(f->r);
    ASSERT_EQ(f->r->count, 1u);
    EXPECT_TRUE(find_event(f->r, "scratch:/old.txt", DE_WATCH_DELETED) >= 0);
    memset(f->r, 0, sizeof *f->r);

    inject(w, "brief.txt", created | modified | removed | is_file, FAKE_INO + 1);
    inject(w, "brief_dir", created | removed | is_dir, FAKE_INO + 2);
    inject(w, "old.txt", removed | is_file, FAKE_INO + 4);
    /* One event with both flags, for a path the watch never saw. */
    inject(w, "coalesced.txt", created | modified | removed | is_file, FAKE_INO + 5);
    /* Reported as created and gone (and dropped by the common layer since), then shown to be a
     * rename: CREATED + MOVED_FROM go out again so that a MOVED_TO could pair with it.
     * Unpaired, it is the move of a file created in this window: nothing is reported. */
    inject(w, "tmp.txt", created | modified | renamed | is_file, FAKE_INO + 3);
    inject(w, "tmp.txt", created | modified | renamed | is_file, FAKE_INO + 3);
    pump(w, f->r, 0, 400);
    dump(f->r);
    EXPECT_EQ(f->r->count, 0u);
    EXPECT_EQ(de__watch_pending_count(w), 0u);
}

/* An atomic save of a new file on FSEvents when the temporary's first event is handled after
 * the rename (S03 exit run under load: `RENAMED fresh.txt (from fresh.txt.tmp-…)`). The
 * temporary's `Created|Modified` finds no file (CREATED + DELETED), then its sticky
 * `Created|Modified|Renamed` must still give the MOVED_FROM that pairs with the target, and the
 * result is the documented one: one MODIFIED for the target. The target's MOVED_TO is posted as
 * the backend posts it for a file that exists with the event's inode (the fake inode is not on
 * disk, so it cannot go through the backend). */
TEST_F(watch, fsevents_atomic_save, watch_fx) {
    const u32 created = kFSEventStreamEventFlagItemCreated;
    const u32 renamed = kFSEventStreamEventFlagItemRenamed;
    const u32 modified = kFSEventStreamEventFlagItemModified;
    const u32 is_file = kFSEventStreamEventFlagItemIsFile;
    fresh_dir(f, "fse_atomic");
    start(f, "scratch:/", true, 100);
    de_file_watch *w = f->w;
    inject(w, "fresh.txt.tmp-1", created | modified | is_file, FAKE_INO + 1);
    inject(w, "fresh.txt.tmp-1", created | modified | renamed | is_file, FAKE_INO + 1);
    post(w, DE__WATCH_RAW_MOVED_TO, "fresh.txt", false, FAKE_INO + 1);
    /* The same with the temporary's events in one: CREATED + MOVED_FROM. */
    inject(w, "other.txt.tmp-2", created | modified | renamed | is_file, FAKE_INO + 2);
    post(w, DE__WATCH_RAW_MOVED_TO, "other.txt", false, FAKE_INO + 2);
    pump(w, f->r, 2, 400);
    dump(f->r);
    ASSERT_EQ(f->r->count, 2u);
    EXPECT_TRUE(find_event(f->r, "scratch:/fresh.txt", DE_WATCH_MODIFIED) >= 0);
    EXPECT_TRUE(find_event(f->r, "scratch:/other.txt", DE_WATCH_MODIFIED) >= 0);
}
#endif

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

/* The known-directory set the Windows backend keeps (DENG-370). Portable, so tested here. */
static bool dirset_has(const de__watch_dirset *s, const char *rel) {
    return de__watch_dirset_has(s, rel, strlen(rel));
}

static bool dirset_add(de__watch_dirset *s, const char *rel) {
    return de__watch_dirset_add(s, rel, strlen(rel));
}

static bool dirset_remove(de__watch_dirset *s, const char *rel) {
    return de__watch_dirset_remove_tree(s, rel, strlen(rel));
}

TEST_F(watch, dirset, watch_fx) {
    counting cnt = {de_heap_allocator(), 0, 0};
    de_allocator a = {cnt_alloc, cnt_realloc, cnt_free, &cnt};
    de__watch_dirset s;
    de__watch_dirset_init(&s, &a, false);
    EXPECT_FALSE(dirset_has(&s, "a"));
    EXPECT_FALSE(dirset_remove(&s, "a"));
    EXPECT_TRUE(dirset_add(&s, "")); /* the watched directory itself: not stored */
    EXPECT_EQ(s.count, 0u);
    ASSERT_TRUE(dirset_add(&s, "a"));
    ASSERT_TRUE(dirset_add(&s, "a/b"));
    ASSERT_TRUE(dirset_add(&s, "a/b/c"));
    ASSERT_TRUE(dirset_add(&s, "a/bc"));
    ASSERT_TRUE(dirset_add(&s, "ab"));
    ASSERT_TRUE(dirset_add(&s, "a/b")); /* again: no duplicate */
    EXPECT_EQ(s.count, 5u);
    EXPECT_TRUE(dirset_has(&s, "a/b"));
    EXPECT_FALSE(dirset_has(&s, "a/b/"));
    EXPECT_FALSE(dirset_has(&s, "A/B")); /* case matters unless the set says otherwise */
    /* A removal takes the path and everything below it, not names that only share a prefix. */
    EXPECT_TRUE(dirset_remove(&s, "a/b"));
    EXPECT_EQ(s.count, 3u);
    EXPECT_FALSE(dirset_has(&s, "a/b"));
    EXPECT_FALSE(dirset_has(&s, "a/b/c"));
    EXPECT_TRUE(dirset_has(&s, "a"));
    EXPECT_TRUE(dirset_has(&s, "a/bc"));
    EXPECT_TRUE(dirset_has(&s, "ab"));
    EXPECT_FALSE(dirset_remove(&s, "a/b/c")); /* already gone */

    /* Enough entries to grow the table many times and give long probe runs, then remove every
     * other subtree: the entries that stay must all still be found (backward-shift deletion). */
    char p[64];
    for (u32 i = 0; i < 500; ++i) {
        snprintf(p, sizeof p, "t%u", (unsigned)i);
        ASSERT_TRUE(dirset_add(&s, p));
        for (u32 j = 0; j < 3; ++j) {
            snprintf(p, sizeof p, "t%u/s%u", (unsigned)i, (unsigned)j);
            ASSERT_TRUE(dirset_add(&s, p));
        }
    }
    EXPECT_EQ(s.count, 2003u);
    for (u32 i = 0; i < 500; i += 2) {
        snprintf(p, sizeof p, "t%u", (unsigned)i);
        EXPECT_TRUE(dirset_remove(&s, p));
    }
    EXPECT_EQ(s.count, 1003u);
    u32 wrong = 0;
    for (u32 i = 0; i < 500; ++i) {
        snprintf(p, sizeof p, "t%u", (unsigned)i);
        wrong += dirset_has(&s, p) != (i % 2 == 1) ? 1u : 0u;
        for (u32 j = 0; j < 3; ++j) {
            snprintf(p, sizeof p, "t%u/s%u", (unsigned)i, (unsigned)j);
            wrong += dirset_has(&s, p) != (i % 2 == 1) ? 1u : 0u;
        }
    }
    EXPECT_EQ(wrong, 0u);
    de__watch_dirset_clear(&s);
    EXPECT_EQ(s.count, 0u);
    EXPECT_FALSE(dirset_has(&s, "t1"));

    /* Windows names: ASCII letters compare without case. */
    de__watch_dirset n;
    de__watch_dirset_init(&n, &a, true);
    ASSERT_TRUE(dirset_add(&n, "Gone"));
    ASSERT_TRUE(dirset_add(&n, "Gone/Inner"));
    ASSERT_TRUE(dirset_add(&n, "gone/inner")); /* the same directory */
    EXPECT_EQ(n.count, 2u);
    EXPECT_TRUE(dirset_has(&n, "GONE/inner"));
    EXPECT_TRUE(dirset_remove(&n, "gONE"));
    EXPECT_EQ(n.count, 0u);
    de__watch_dirset_free(&n);

    /* Scans read the tree on disk: directories only, as deep as the watch reports. */
    fresh_dir(f, "dirset");
    make_dir(f, "x/y/z");
    make_dir(f, "w");
    write_file(f, "x/file.txt", "f");
    write_file(f, "top.txt", "t");
    de__watch_dirset_scan(&s, f->dir, "", 0, true);
    EXPECT_EQ(s.count, 4u);
    EXPECT_TRUE(dirset_has(&s, "x"));
    EXPECT_TRUE(dirset_has(&s, "x/y"));
    EXPECT_TRUE(dirset_has(&s, "x/y/z"));
    EXPECT_TRUE(dirset_has(&s, "w"));
    EXPECT_FALSE(dirset_has(&s, "x/file.txt"));
    EXPECT_FALSE(dirset_has(&s, "top.txt"));
    de__watch_dirset_clear(&s);
    de__watch_dirset_scan(&s, f->dir, "", 0, false); /* not recursive: the top level only */
    EXPECT_EQ(s.count, 2u);
    EXPECT_TRUE(dirset_has(&s, "x"));
    EXPECT_TRUE(dirset_has(&s, "w"));
    de__watch_dirset_clear(&s);
    de__watch_dirset_scan(&s, f->dir, "x", 1, true); /* a directory that arrived: it and below */
    EXPECT_EQ(s.count, 3u);
    EXPECT_TRUE(dirset_has(&s, "x/y/z"));
    de__watch_dirset_clear(&s);
    de__watch_dirset_scan(&s, f->dir, "x", 1, false);
    EXPECT_EQ(s.count, 1u);
    de__watch_dirset_scan(&s, f->dir, "missing", 7, true); /* gone again: just the name */
    EXPECT_TRUE(dirset_has(&s, "missing"));
    de__watch_dirset_free(&s);
    EXPECT_EQ(cnt.bytes, 0);
    EXPECT_EQ(cnt.blocks, 0);
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
    u64 changed = de_time_now_ns(); /* before the write: see the top of the file */
    write_file(f, "a.txt", "a");
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
