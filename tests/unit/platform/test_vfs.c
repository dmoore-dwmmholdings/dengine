/* Tests for the virtual file system and the synchronous file API (DENG-247).
 *
 * Each test works in its own directory under DE_IO_TEST_TMP_DIR (the build tree), which it
 * empties first and mounts under the roots it needs; nothing is written into the real user
 * profile. CTest runs every test in its own process. */
#include "de_test.h"

#include <dengine/de_debug.h>
#include <dengine/de_platform.h>

#include "platform/vfs_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_IO_TEST_TMP_DIR
#error "DE_IO_TEST_TMP_DIR must be defined by tests/unit/platform/CMakeLists.txt"
#endif

/* Size of a buffer that holds a DE_FS_PATH_MAX test directory plus a short suffix. It is larger
 * than DE_FS_PATH_MAX so that snprintf can never truncate: GCC rejects the possible truncation
 * with -Werror=format-truncation (DENG-360; DENG-356 fixed test_error.c the same way). */
#define SUBPATH_MAX (DE_FS_PATH_MAX + 64)

/* ---------- helpers ---------- */

typedef struct name_list {
    char names[64][256];
    bool is_dir[64];
    u32 count;
    u32 stop_after; /* 0 = never stop */
} name_list;

static bool collect(const char *name, bool is_dir, void *user) {
    name_list *l = (name_list *)user;
    if (l->count < 64) {
        snprintf(l->names[l->count], sizeof l->names[0], "%s", name);
        l->is_dir[l->count] = is_dir;
    }
    l->count++;
    return l->stop_after == 0 || l->count < l->stop_after;
}

static int find_name(const name_list *l, const char *name) {
    for (u32 i = 0; i < l->count && i < 64; ++i) {
        if (strcmp(l->names[i], name) == 0)
            return (int)i;
    }
    return -1;
}

/* Deletes everything below `vpath` (a directory on a writable mount). */
static void rm_tree(const char *vpath) {
    name_list *l = (name_list *)calloc(1, sizeof *l);
    ASSERT_TRUE(l != NULL);
    if (de_dir_list(vpath, collect, l) == DE_OK) {
        for (u32 i = 0; i < l->count && i < 64; ++i) {
            char child[DE_FS_PATH_MAX];
            snprintf(child, sizeof child, "%s/%s", vpath, l->names[i]);
            if (l->is_dir[i])
                rm_tree(child);
            (void)de_file_delete(child);
        }
    }
    free(l);
    de_error_clear();
}

/* Creates DE_IO_TEST_TMP_DIR/<name>, empty, and writes its native path into `dir`. */
static void fresh_dir(const char *name, char *dir, size_t cap) {
    snprintf(dir, cap, "%s/%s", DE_IO_TEST_TMP_DIR, name);
    ASSERT_EQ(de__native_mkdirs(dir), DE_OK);
    ASSERT_EQ(de_fs_mount("scratch", dir, DE_MOUNT_WRITABLE, 0), DE_OK);
    rm_tree("scratch:/");
    ASSERT_EQ(de_fs_unmount("scratch", dir), DE_OK);
}

static void write_text(const char *vpath, const char *text) {
    de_file *f = NULL;
    ASSERT_EQ(de_file_open(vpath, DE_FILE_WRITE, &f), DE_OK);
    ASSERT_EQ(de_file_write(f, text, strlen(text)), DE_OK);
    de_file_close(f);
}

/* Reads a whole file as text; "" with a failed expectation if it cannot. Static buffer. */
static const char *read_text(const char *vpath) {
    static char buf[4096];
    void *data = NULL;
    u64 size = 0;
    buf[0] = '\0';
    de_result r = de_file_read_all(vpath, NULL, &data, &size);
    EXPECT_EQ(r, DE_OK);
    if (r != DE_OK)
        return buf;
    EXPECT_EQ(((const char *)data)[size], 0); /* NUL after the data */
    snprintf(buf, sizeof buf, "%.*s", (int)size, (const char *)data);
    DE_FREE(de_heap_allocator(), data, (size_t)size + 1);
    return buf;
}

/* ---------- tests ---------- */

TEST(vfs, roundtrip_user) {
    char dir[DE_FS_PATH_MAX];
    fresh_dir("roundtrip_user", dir, sizeof dir);
    ASSERT_EQ(de_fs_mount("user", dir, DE_MOUNT_WRITABLE, 100), DE_OK);

    /* Parent directory first: opening for write does not create directories. */
    de_file *f = NULL;
    EXPECT_EQ(de_file_open("user:/a/b.txt", DE_FILE_WRITE, &f), DE_ERR_NOT_FOUND);
    EXPECT_TRUE(f == NULL);
    ASSERT_EQ(de_dir_create("user:/a"), DE_OK);
    ASSERT_EQ(de_dir_create("user:/a"), DE_OK); /* existing: fine */
    EXPECT_TRUE(de_path_exists("user:/a"));

    write_text("user:/a/b.txt", "hello, file");
    EXPECT_TRUE(de_path_exists("user:/a/b.txt"));
    EXPECT_TRUE(de_path_exists("user:/a//./b.txt"));
    EXPECT_STREQ(read_text("user:/a/b.txt"), "hello, file");

    /* Positional reads, size, short read at the end. */
    ASSERT_EQ(de_file_open("user:/a/b.txt", DE_FILE_READ, &f), DE_OK);
    EXPECT_EQ(de_file_size(f), 11u);
    char buf[32] = {0};
    u64 got = 0;
    EXPECT_EQ(de_file_read(f, buf, 7, 4, &got), DE_OK);
    EXPECT_EQ(got, 4u);
    EXPECT_MEMEQ(buf, "file", 4);
    EXPECT_EQ(de_file_read(f, buf, 7, 20, &got), DE_OK);
    EXPECT_EQ(got, 4u);
    EXPECT_EQ(de_file_read(f, buf, 100, 4, &got), DE_OK);
    EXPECT_EQ(got, 0u);
    EXPECT_EQ(de_file_write(f, "x", 1), DE_ERR_INVALID_ARGUMENT);
    de_file_close(f);

    /* Append adds at the end; write truncates. */
    ASSERT_EQ(de_file_open("user:/a/b.txt", DE_FILE_APPEND, &f), DE_OK);
    ASSERT_EQ(de_file_write(f, "!!", 2), DE_OK);
    EXPECT_EQ(de_file_read(f, buf, 0, 1, &got), DE_ERR_INVALID_ARGUMENT);
    de_file_close(f);
    EXPECT_STREQ(read_text("user:/a/b.txt"), "hello, file!!");
    write_text("user:/a/b.txt", "new");
    EXPECT_STREQ(read_text("user:/a/b.txt"), "new");

    /* resolve gives the native path in the mount. */
    char native[DE_FS_PATH_MAX], expect[SUBPATH_MAX];
    ASSERT_EQ(de_fs_resolve("user:/a/./b.txt", native, sizeof native), DE_OK);
    snprintf(expect, sizeof expect, "%s/a/b.txt", dir);
    EXPECT_STREQ(native, expect);
    EXPECT_EQ(de_fs_resolve("user:/a/b.txt", native, 4), DE_ERR_INVALID_ARGUMENT);

    /* Opening a directory or a missing file. */
    EXPECT_EQ(de_file_open("user:/a", DE_FILE_READ, &f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_file_open("user:/a/missing.txt", DE_FILE_READ, &f), DE_ERR_NOT_FOUND);
    EXPECT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_file_open("nomount:/x", DE_FILE_READ, &f), DE_ERR_NOT_FOUND);

    /* Delete: file, then the empty directory. */
    EXPECT_EQ(de_file_delete("user:/a"), DE_ERR_IO); /* not empty */
    EXPECT_EQ(de_file_delete("user:/a/b.txt"), DE_OK);
    EXPECT_FALSE(de_path_exists("user:/a/b.txt"));
    EXPECT_EQ(de_file_delete("user:/a/b.txt"), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_file_delete("user:/a"), DE_OK);
    EXPECT_FALSE(de_path_exists("user:/a"));

    /* Nested directory creation. */
    ASSERT_EQ(de_dir_create("user:/x/y/z"), DE_OK);
    EXPECT_TRUE(de_path_exists("user:/x/y/z"));
    write_text("user:/x/y/file", "f");
    EXPECT_EQ(de_dir_create("user:/x/y/file/sub"), DE_ERR_IO); /* a file is in the way */

    ASSERT_EQ(de_fs_unmount("user", dir), DE_OK);
    EXPECT_EQ(de_fs_unmount("user", dir), DE_ERR_NOT_FOUND);
    EXPECT_FALSE(de_path_exists("user:/x"));
}

TEST(vfs, reject_escape) {
    char dir[DE_FS_PATH_MAX];
    fresh_dir("reject_escape", dir, sizeof dir);
    ASSERT_EQ(de_fs_mount("game", dir, DE_MOUNT_READ_ONLY, 0), DE_OK);
    de_file *f = NULL;

    /* Escapes and native paths. */
    EXPECT_EQ(de_file_open("game:/../etc/passwd", DE_FILE_READ, &f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_file_open("game:/a/../../etc/passwd", DE_FILE_READ, &f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_file_open("game:/./..", DE_FILE_READ, &f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_file_open("/etc/passwd", DE_FILE_READ, &f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_file_open("etc/passwd", DE_FILE_READ, &f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_file_open("C:\\Windows\\win.ini", DE_FILE_READ, &f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_file_open("C:/Windows/win.ini", DE_FILE_READ, &f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_file_open("game:/..\\x", DE_FILE_READ, &f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_file_open("game:/a:b", DE_FILE_READ, &f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_file_open(NULL, DE_FILE_READ, &f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(f == NULL);
    EXPECT_FALSE(de_path_exists("game:/../etc/passwd"));
    EXPECT_FALSE(de_path_exists("/etc/passwd"));

    /* de_path_exists probes quietly. */
    de_error_clear();
    EXPECT_FALSE(de_path_exists("game:/nope"));
    EXPECT_FALSE(de_path_exists("game:/../nope"));
    EXPECT_TRUE(de_error_last() == NULL);

    /* Normalisation. */
    char buf[DE_FS_PATH_MAX];
    ASSERT_EQ(de_path_normalize("user:/a/./b//c/", buf, sizeof buf), DE_OK);
    EXPECT_STREQ(buf, "user:/a/b/c");
    ASSERT_EQ(de_path_normalize("user:/a/../b", buf, sizeof buf), DE_OK);
    EXPECT_STREQ(buf, "user:/b");
    ASSERT_EQ(de_path_normalize("user:/a/b/../../c/..", buf, sizeof buf), DE_OK);
    EXPECT_STREQ(buf, "user:/");
    ASSERT_EQ(de_path_normalize("dev:/", buf, sizeof buf), DE_OK);
    EXPECT_STREQ(buf, "dev:/");
    ASSERT_EQ(de_path_normalize("my_root-2:/..x/.y", buf, sizeof buf), DE_OK);
    EXPECT_STREQ(buf, "my_root-2:/..x/.y"); /* only exact "." and ".." are special */
    EXPECT_EQ(de_path_normalize("user:/a/b", buf, 6), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_path_normalize("User:/x", buf, sizeof buf), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_path_normalize("user:x", buf, sizeof buf), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_path_normalize(":/x", buf, sizeof buf), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_path_normalize("abcdefghijklmnop:/x", buf, sizeof buf), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_path_normalize("user:/a\x01", buf, sizeof buf), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_path_normalize("user:/\xff", buf, sizeof buf), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_path_normalize("user:/\xc0\xaf", buf, sizeof buf), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_path_normalize("user:/\xed\xa0\x80", buf, sizeof buf), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_path_normalize("user:/\xe6\x97", buf, sizeof buf), DE_ERR_INVALID_ARGUMENT);

    /* Mount arguments. */
    EXPECT_EQ(de_fs_mount("pack", dir, DE_MOUNT_READ_ONLY, 0), DE_ERR_UNSUPPORTED);
    /* DENG-393: pack:/ is reserved and never mounted, so its paths never resolve. */
    EXPECT_EQ(de_fs_mount("pack", dir, DE_MOUNT_WRITABLE, 10), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(de_fs_resolve("pack:/game.dpak", buf, sizeof buf), DE_ERR_NOT_FOUND);
    void *pack_bytes = NULL;
    u64 pack_size = 0;
    EXPECT_EQ(de_file_read_all("pack:/data/hello", NULL, &pack_bytes, &pack_size),
              DE_ERR_NOT_FOUND);
    EXPECT_TRUE(pack_bytes == NULL);
    de_error_clear();
    EXPECT_EQ(de_fs_mount("Bad", dir, DE_MOUNT_READ_ONLY, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_fs_mount("", dir, DE_MOUNT_READ_ONLY, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_fs_mount("x", NULL, DE_MOUNT_READ_ONLY, 0), DE_ERR_INVALID_ARGUMENT);
    char missing[SUBPATH_MAX];
    snprintf(missing, sizeof missing, "%s/does_not_exist", dir);
    EXPECT_EQ(de_fs_mount("x", missing, DE_MOUNT_READ_ONLY, 0), DE_ERR_NOT_FOUND);

    ASSERT_EQ(de_fs_unmount("game", dir), DE_OK);
}

TEST(vfs, readonly_mount) {
    char dir[DE_FS_PATH_MAX];
    fresh_dir("readonly_mount", dir, sizeof dir);
    /* The same directory, writable as rw:/ and read-only as ro:/. */
    ASSERT_EQ(de_fs_mount("rw", dir, DE_MOUNT_WRITABLE, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("ro", dir, DE_MOUNT_READ_ONLY, 0), DE_OK);
    write_text("rw:/f.txt", "data");
    ASSERT_EQ(de_dir_create("rw:/d"), DE_OK);

    EXPECT_STREQ(read_text("ro:/f.txt"), "data");
    de_file *f = NULL;
    EXPECT_EQ(de_file_open("ro:/f.txt", DE_FILE_WRITE, &f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_file_open("ro:/f.txt", DE_FILE_APPEND, &f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_file_open("ro:/new.txt", DE_FILE_WRITE, &f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_file_write_all_atomic("ro:/f.txt", "x", 1), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_file_delete("ro:/f.txt"), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_dir_create("ro:/d2"), DE_ERR_INVALID_ARGUMENT);
    EXPECT_FALSE(de_path_exists("rw:/new.txt"));
    EXPECT_FALSE(de_path_exists("rw:/d2"));
    EXPECT_STREQ(read_text("rw:/f.txt"), "data");

    ASSERT_EQ(de_fs_unmount("ro", dir), DE_OK);
    ASSERT_EQ(de_fs_unmount("rw", dir), DE_OK);
}

TEST(vfs, mount_priority) {
    char base[DE_FS_PATH_MAX], low[SUBPATH_MAX], high[SUBPATH_MAX];
    fresh_dir("mount_priority", base, sizeof base);
    snprintf(low, sizeof low, "%s/low", base);
    snprintf(high, sizeof high, "%s/high/", base); /* trailing separator is ignored */
    ASSERT_EQ(de__native_mkdirs(low), DE_OK);
    ASSERT_EQ(de__native_mkdirs(high), DE_OK);
    ASSERT_EQ(de_fs_mount("lo", low, DE_MOUNT_WRITABLE, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("hi", high, DE_MOUNT_WRITABLE, 0), DE_OK);
    write_text("lo:/both.txt", "low");
    write_text("hi:/both.txt", "high");
    write_text("lo:/low_only.txt", "low only");

    /* data:/ = high (priority 10, read-only) over low (priority 0, writable). Mount the
     * higher one first, to show that the priority, not the order, decides. */
    ASSERT_EQ(de_fs_mount("data", high, DE_MOUNT_READ_ONLY, 10), DE_OK);
    ASSERT_EQ(de_fs_mount("data", low, DE_MOUNT_WRITABLE, 0), DE_OK);
    EXPECT_STREQ(read_text("data:/both.txt"), "high");
    EXPECT_STREQ(read_text("data:/low_only.txt"), "low only");

    /* Writes go to the first writable mount (low). */
    write_text("data:/new.txt", "written");
    EXPECT_TRUE(de_path_exists("lo:/new.txt"));
    EXPECT_FALSE(de_path_exists("hi:/new.txt"));
    char native[DE_FS_PATH_MAX], expect[SUBPATH_MAX];
    ASSERT_EQ(de_fs_resolve("data:/both.txt", native, sizeof native), DE_OK);
    snprintf(expect, sizeof expect, "%s/high/both.txt", base);
    EXPECT_STREQ(native, expect);
    ASSERT_EQ(de_fs_resolve("data:/not_yet.txt", native, sizeof native), DE_OK);
    snprintf(expect, sizeof expect, "%s/low/not_yet.txt", base);
    EXPECT_STREQ(native, expect); /* where a write would create it */
    /* Delete only touches writable mounts: both.txt in high stays. */
    EXPECT_EQ(de_file_delete("data:/both.txt"), DE_OK);
    EXPECT_STREQ(read_text("data:/both.txt"), "high");
    EXPECT_FALSE(de_path_exists("lo:/both.txt"));
    write_text("lo:/both.txt", "low");

    /* Equal priority: the newer mount is searched first. */
    ASSERT_EQ(de_fs_mount("eq", low, DE_MOUNT_READ_ONLY, 5), DE_OK);
    ASSERT_EQ(de_fs_mount("eq", high, DE_MOUNT_READ_ONLY, 5), DE_OK);
    EXPECT_STREQ(read_text("eq:/both.txt"), "high");
    ASSERT_EQ(de_fs_unmount("eq", high), DE_OK);
    EXPECT_STREQ(read_text("eq:/both.txt"), "low");
    ASSERT_EQ(de_fs_unmount("eq", low), DE_OK);

    /* Without the high mount, low shows through. */
    ASSERT_EQ(de_fs_unmount("data", high), DE_OK);
    EXPECT_STREQ(read_text("data:/both.txt"), "low");
    ASSERT_EQ(de_fs_unmount("data", low), DE_OK);
    ASSERT_EQ(de_fs_unmount("lo", low), DE_OK);
    ASSERT_EQ(de_fs_unmount("hi", high), DE_OK);
}

typedef struct reentrant_ctx {
    u32 calls;
    bool read_ok;
} reentrant_ctx;

/* Opens a file from inside the listing callback: the mount lock is not held. */
static bool list_and_read(const char *name, bool is_dir, void *user) {
    reentrant_ctx *c = (reentrant_ctx *)user;
    c->calls++;
    if (!is_dir && strcmp(name, "b.txt") == 0) {
        void *data = NULL;
        u64 size = 0;
        c->read_ok = de_file_read_all("ls:/b.txt", NULL, &data, &size) == DE_OK && size == 1;
        if (data != NULL)
            DE_FREE(de_heap_allocator(), data, (size_t)size + 1);
        char extra[DE_FS_PATH_MAX];
        c->read_ok = c->read_ok && de_fs_resolve("ls:/b.txt", extra, sizeof extra) == DE_OK;
    }
    return true;
}

TEST(vfs, dir_list) {
    char base[DE_FS_PATH_MAX], lower[SUBPATH_MAX], upper[SUBPATH_MAX];
    fresh_dir("dir_list", base, sizeof base);
    snprintf(lower, sizeof lower, "%s/lower", base);
    snprintf(upper, sizeof upper, "%s/upper", base);
    ASSERT_EQ(de__native_mkdirs(lower), DE_OK);
    ASSERT_EQ(de__native_mkdirs(upper), DE_OK);
    ASSERT_EQ(de_fs_mount("lw", lower, DE_MOUNT_WRITABLE, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("up", upper, DE_MOUNT_WRITABLE, 0), DE_OK);
    write_text("lw:/a.txt", "a");
    write_text("lw:/b.txt", "b");
    ASSERT_EQ(de_dir_create("lw:/sub/deeper"), DE_OK);
    write_text("lw:/sub/in_sub.txt", "s");
    write_text("up:/a.txt", "A");
    write_text("up:/c.txt", "c");
    ASSERT_EQ(de_dir_create("up:/sub"), DE_OK);

    ASSERT_EQ(de_fs_mount("ls", upper, DE_MOUNT_READ_ONLY, 1), DE_OK);
    ASSERT_EQ(de_fs_mount("ls", lower, DE_MOUNT_WRITABLE, 0), DE_OK);

    name_list *l = (name_list *)calloc(1, sizeof *l);
    ASSERT_TRUE(l != NULL);
    ASSERT_EQ(de_dir_list("ls:/", collect, l), DE_OK);
    EXPECT_EQ(l->count, 4u); /* a.txt once, b.txt, c.txt, sub once */
    int ia = find_name(l, "a.txt"), ib = find_name(l, "b.txt");
    int ic = find_name(l, "c.txt"), is = find_name(l, "sub");
    ASSERT_TRUE(ia >= 0 && ib >= 0 && ic >= 0 && is >= 0);
    EXPECT_FALSE(l->is_dir[ia]);
    EXPECT_FALSE(l->is_dir[ib]);
    EXPECT_TRUE(l->is_dir[is]);
    /* The higher mount's entries come first. */
    EXPECT_LT(ia, ib);
    EXPECT_LT(ic, ib);

    /* A subdirectory that only one mount has content in. */
    memset(l, 0, sizeof *l);
    ASSERT_EQ(de_dir_list("ls:/sub", collect, l), DE_OK);
    EXPECT_EQ(l->count, 2u);
    EXPECT_TRUE(find_name(l, "in_sub.txt") >= 0);
    EXPECT_TRUE(find_name(l, "deeper") >= 0);

    /* Stopping early. */
    memset(l, 0, sizeof *l);
    l->stop_after = 2;
    ASSERT_EQ(de_dir_list("ls:/", collect, l), DE_OK);
    EXPECT_EQ(l->count, 2u);

    /* Errors. */
    memset(l, 0, sizeof *l);
    EXPECT_EQ(de_dir_list("ls:/missing", collect, l), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_dir_list("nomount:/", collect, l), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_dir_list("ls:/../x", collect, l), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_dir_list("ls:/", NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(l->count, 0u);
    free(l);

    /* The callback may use the file API (and the mount lock) itself. */
    reentrant_ctx c = {0};
    ASSERT_EQ(de_dir_list("ls:/", list_and_read, &c), DE_OK);
    EXPECT_EQ(c.calls, 4u);
    EXPECT_TRUE(c.read_ok);

    ASSERT_EQ(de_fs_unmount("ls", upper), DE_OK);
    ASSERT_EQ(de_fs_unmount("ls", lower), DE_OK);
    ASSERT_EQ(de_fs_unmount("lw", lower), DE_OK);
    ASSERT_EQ(de_fs_unmount("up", upper), DE_OK);
}

TEST(vfs, atomic_write) {
    char dir[DE_FS_PATH_MAX];
    fresh_dir("atomic_write", dir, sizeof dir);
    ASSERT_EQ(de_fs_mount("user", dir, DE_MOUNT_WRITABLE, 0), DE_OK);

    static const char *const contents[] = {"first version, rather long text", "2nd", ""};
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(de_file_write_all_atomic("user:/cfg.txt", contents[i], strlen(contents[i])),
                  DE_OK);
        EXPECT_STREQ(read_text("user:/cfg.txt"), contents[i]);
    }
    /* Larger than one write call chunk on no OS, but more than a page. */
    static u8 big[300000];
    for (size_t i = 0; i < sizeof big; ++i)
        big[i] = (u8)(i * 7u + 3u);
    ASSERT_EQ(de_file_write_all_atomic("user:/big.bin", big, sizeof big), DE_OK);
    void *data = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all("user:/big.bin", NULL, &data, &size), DE_OK);
    EXPECT_EQ(size, (u64)sizeof big);
    EXPECT_MEMEQ(data, big, sizeof big);
    DE_FREE(de_heap_allocator(), data, (size_t)size + 1);

    /* No temporary file is left behind. */
    name_list *l = (name_list *)calloc(1, sizeof *l);
    ASSERT_TRUE(l != NULL);
    ASSERT_EQ(de_dir_list("user:/", collect, l), DE_OK);
    EXPECT_EQ(l->count, 2u);
    EXPECT_TRUE(find_name(l, "cfg.txt") >= 0);
    EXPECT_TRUE(find_name(l, "big.bin") >= 0);
    free(l);

    /* Errors: missing parent, a directory in the way, a root, bad arguments. */
    EXPECT_EQ(de_file_write_all_atomic("user:/no_dir/x.txt", "x", 1), DE_ERR_NOT_FOUND);
    ASSERT_EQ(de_dir_create("user:/d"), DE_OK);
    EXPECT_EQ(de_file_write_all_atomic("user:/d", "x", 1), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_file_write_all_atomic("user:/", "x", 1), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_file_write_all_atomic("user:/n.txt", NULL, 1), DE_ERR_INVALID_ARGUMENT);
    EXPECT_FALSE(de_path_exists("user:/no_dir"));

    ASSERT_EQ(de_fs_unmount("user", dir), DE_OK);
}

TEST(vfs, utf8_names) {
    char dir[DE_FS_PATH_MAX];
    fresh_dir("utf8_names", dir, sizeof dir);
    ASSERT_EQ(de_fs_mount("user", dir, DE_MOUNT_WRITABLE, 0), DE_OK);
    /* Characters without Unicode decompositions, so no file system renormalises them. */
    static const char dname[] = "data_\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";       /* 日本語 */
    static const char fname[] = "\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab" /* ファイル */
                                "_\xce\xa9_\xf0\x9f\x98\x80 x.txt";                /* Ω 😀 */
    char dpath[256], fpath[512];
    snprintf(dpath, sizeof dpath, "user:/%s", dname);
    snprintf(fpath, sizeof fpath, "user:/%s/%s", dname, fname);
    ASSERT_EQ(de_dir_create(dpath), DE_OK);
    write_text(fpath, "unicode");
    EXPECT_TRUE(de_path_exists(fpath));
    EXPECT_STREQ(read_text(fpath), "unicode");

    name_list *l = (name_list *)calloc(1, sizeof *l);
    ASSERT_TRUE(l != NULL);
    ASSERT_EQ(de_dir_list(dpath, collect, l), DE_OK);
    EXPECT_EQ(l->count, 1u);
    EXPECT_STREQ(l->names[0], fname);
    memset(l, 0, sizeof *l);
    ASSERT_EQ(de_dir_list("user:/", collect, l), DE_OK);
    EXPECT_EQ(l->count, 1u);
    EXPECT_STREQ(l->names[0], dname);
    EXPECT_TRUE(l->is_dir[0]);
    free(l);

    EXPECT_EQ(de_file_write_all_atomic(fpath, "v2", 2), DE_OK);
    EXPECT_STREQ(read_text(fpath), "v2");
    EXPECT_EQ(de_file_delete(fpath), DE_OK);
    EXPECT_EQ(de_file_delete(dpath), DE_OK);
    ASSERT_EQ(de_fs_unmount("user", dir), DE_OK);
}

TEST(vfs, cvar_config) {
    char dir[DE_FS_PATH_MAX];
    fresh_dir("cvar_config", dir, sizeof dir);
    ASSERT_EQ(de_fs_mount("user", dir, DE_MOUNT_WRITABLE, 0), DE_OK);
    static const char cfg[] = "[vfstest]\nvalue = 42\nname = \"from config\"\n";
    ASSERT_EQ(de_file_write_all_atomic("user:/config.toml", cfg, sizeof cfg - 1), DE_OK);

    de_cvar v = de_cvar_register(
        &(de_cvar_desc){.name = "vfstest.value", .type = DE_CVAR_INT, .flags = DE_CVAR_SAVED});
    ASSERT_TRUE(DE_HANDLE_VALID(v));
    ASSERT_EQ(de_cvar_load("user:/config.toml"), DE_OK);
    EXPECT_EQ(de_cvar_int(v), 42);
    de_cvar n = de_cvar_register(
        &(de_cvar_desc){.name = "vfstest.name", .type = DE_CVAR_STRING, .flags = DE_CVAR_SAVED});
    EXPECT_STREQ(de_cvar_string(n), "from config"); /* was pending */

    /* Save replaces the file atomically and loads back. */
    ASSERT_EQ(de_cvar_set(v, "7"), DE_OK);
    ASSERT_EQ(de_cvar_save("user:/config.toml"), DE_OK);
    const char *text = read_text("user:/config.toml");
    EXPECT_TRUE(strstr(text, "value = 7\n") != NULL);
    EXPECT_TRUE(strstr(text, "name = \"from config\"\n") != NULL);
    ASSERT_EQ(de_cvar_set(v, "1"), DE_OK);
    ASSERT_EQ(de_cvar_load("user:/config.toml"), DE_OK);
    EXPECT_EQ(de_cvar_int(v), 7);

    EXPECT_EQ(de_cvar_load("user:/missing.toml"), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_cvar_load("/etc/passwd"), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de_fs_unmount("user", dir), DE_OK);
}

TEST(vfs, init_standard_roots) {
    char base[DE_FS_PATH_MAX], game[SUBPATH_MAX], user[SUBPATH_MAX];
    char cache[SUBPATH_MAX], dev[SUBPATH_MAX];
    fresh_dir("init_standard_roots", base, sizeof base);
    snprintf(game, sizeof game, "%s/game", base);
    snprintf(user, sizeof user, "%s/profile/user", base); /* created by init */
    snprintf(cache, sizeof cache, "%s/profile/cache", base);
    snprintf(dev, sizeof dev, "%s/project", base);
    ASSERT_EQ(de__native_mkdirs(game), DE_OK);
    ASSERT_EQ(de__native_mkdirs(dev), DE_OK);

    de_fs_desc desc = {
        .app = "test", .game_dir = game, .user_dir = user, .cache_dir = cache, .dev_dir = dev};
    ASSERT_EQ(de_fs_init(&desc), DE_OK);
    EXPECT_EQ(de_fs_init(&desc), DE_ERR_ALREADY_EXISTS);
    EXPECT_TRUE(de_path_exists("game:/"));
    EXPECT_TRUE(de_path_exists("user:/"));
    EXPECT_TRUE(de_path_exists("cache:/"));
    write_text("user:/u.txt", "u");
    write_text("cache:/c.txt", "c");
    de_file *f = NULL;
    EXPECT_EQ(de_file_open("game:/g.txt", DE_FILE_WRITE, &f), DE_ERR_INVALID_ARGUMENT);
#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
    EXPECT_TRUE(de_path_exists("dev:/"));
    write_text("dev:/d.txt", "d");
#else
    EXPECT_FALSE(de_path_exists("dev:/"));
#endif
    de_fs_shutdown();
    EXPECT_FALSE(de_path_exists("user:/u.txt"));
    EXPECT_EQ(de_file_open("user:/u.txt", DE_FILE_READ, &f), DE_ERR_NOT_FOUND);

    /* After shutdown, init works again; the defaults find the executable's directory. */
    de_fs_desc defaults = {.app = "test", .user_dir = user, .cache_dir = cache};
    ASSERT_EQ(de_fs_init(&defaults), DE_OK);
    char native[DE_FS_PATH_MAX];
    ASSERT_EQ(de_fs_resolve("game:/", native, sizeof native), DE_OK);
    EXPECT_TRUE(native[0] != '\0');
    EXPECT_TRUE(de_path_exists("game:/"));
    EXPECT_STREQ(read_text("user:/u.txt"), "u");
    de_fs_shutdown();
}

TEST(vfs, known_dirs) {
    /* Only computed, never created: the tests must not touch the real profile. */
    char buf[DE_FS_PATH_MAX];
    ASSERT_EQ(de__known_exe_dir(buf, sizeof buf), DE_OK);
    bool is_dir = false;
    EXPECT_EQ(de__native_stat(buf, &is_dir), DE_OK);
    EXPECT_TRUE(is_dir);
    ASSERT_EQ(de__known_user_dir("Org", "App", buf, sizeof buf), DE_OK);
    size_t len = strlen(buf);
    ASSERT_GT(len, 4u);
    EXPECT_STREQ(buf + len - 4, "/App");
    EXPECT_TRUE(buf[len - 1] != '/');
#if defined(__APPLE__)
    EXPECT_TRUE(strstr(buf, "/Library/Application Support/Org/App") != NULL);
#elif defined(_WIN32)
    EXPECT_TRUE(strstr(buf, "/Org/App") != NULL);
#else
    EXPECT_TRUE(strstr(buf, "Org") == NULL); /* XDG: no organisation level */
#endif
    ASSERT_EQ(de__known_cache_dir("Org", "App", buf, sizeof buf), DE_OK);
#if defined(__APPLE__)
    EXPECT_TRUE(strstr(buf, "/Library/Caches/Org/App") != NULL);
#elif defined(_WIN32)
    EXPECT_TRUE(strstr(buf, "/Org/App/cache") != NULL);
#else
    EXPECT_TRUE(strstr(buf, "/App") != NULL);
#endif
    EXPECT_EQ(de__known_user_dir(NULL, "App", buf, 8), DE_ERR_INVALID_ARGUMENT);
}
