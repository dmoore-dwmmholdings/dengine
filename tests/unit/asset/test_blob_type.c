/*
 * test_blob_type.c: the built-in `blob` type and de_blob_data (DENG-215, SPEC §11.1). CTest names
 * asset_scene.blob.<test>.
 *
 * The fixtures are in data/scenes/ (mounted read-only as scn:/): heart.bin holds NUL bytes, CR LF
 * and all 256 byte values; empty.bin is empty.
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "asset/slots_internal.h"

#include <stdio.h>
#include <string.h>

#define WAIT_NS ((u64)60 * 1000000000u)

static void blob_mount(void) {
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("scn", DE_ASSET_TEST_DATA_DIR "/scenes", 0, 0), DE_OK);
        mounted = true;
    }
}

typedef struct blob_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_task_system *tasks;
    de_asset_system *s;
} blob_fx;

static void blob_fx_setup(blob_fx *f) {
    blob_mount();
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    de_fp_env_init_thread();
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &f->tasks), DE_OK);
    de_asset_system_desc d = {.allocator = &f->alloc, .tasks = f->tasks, .io_threads = 1};
    ASSERT_EQ(de_asset_system_create(&d, &f->s), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->s, "scn:/assets.toml"), DE_OK);
}

static void blob_fx_teardown(blob_fx *f) {
    de_asset_system_destroy(f->s);
    de_task_system_destroy(f->tasks);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}

static de_load_state load_wait(de_asset_system *s, const char *id, de_load_handle *out) {
    de_load_handle h = de_asset_load(s, de_asset_id_from(id), 0);
    de_load_state st = de_asset_wait(s, h, WAIT_NS);
    de_load_info info;
    (void)de_asset_status(s, h, &info);
    if (st != DE_LOAD_READY && info.first_error != DE_OK)
        printf("  %s: %s\n", id, info.first_error_msg);
    *out = h;
    return st;
}

/* The built-in types exist after create, unless the desc opts out. */
TEST(blob, builtin_types_registered) {
    de_asset_system *s = NULL;
    ASSERT_EQ(de_asset_system_create(NULL, &s), DE_OK);
    EXPECT_TRUE(DE_HANDLE_VALID(de__asset_type_find(s, "blob")));
    EXPECT_TRUE(DE_HANDLE_VALID(de__asset_type_find(s, "scene")));
    EXPECT_TRUE(DE_HANDLE_VALID(de__asset_type_find(s, "prefab")));
    /* A game cannot register a second `blob`. */
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_type_register(s, &(de_asset_type_desc){.name = "blob"})));
    de_asset_system_destroy(s);

    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.no_builtin_types = true}, &s), DE_OK);
    EXPECT_FALSE(DE_HANDLE_VALID(de__asset_type_find(s, "blob")));
    EXPECT_TRUE(DE_HANDLE_VALID(de_asset_type_register(s, &(de_asset_type_desc){.name = "blob"})));
    de_asset_system_destroy(s);

    /* An allocation failure during the registration fails the create cleanly. */
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_ASSET);
    bool created = false;
    for (i64 n = 0; n < 64 && !created; ++n) {
        de_tracking_allocator_fail_after(&t, n);
        de_asset_system *s2 = NULL;
        de_result r = de_asset_system_create(&(de_asset_system_desc){.allocator = &a}, &s2);
        de_tracking_allocator_fail_after(&t, -1);
        if (r == DE_OK) {
            created = true;
            EXPECT_TRUE(DE_HANDLE_VALID(de__asset_type_find(s2, "prefab")));
            de_asset_system_destroy(s2);
        } else {
            EXPECT_EQ(r, DE_ERR_OUT_OF_MEMORY);
            EXPECT_TRUE(s2 == NULL);
        }
        EXPECT_EQ(de_tracking_allocator_stats(&t).blocks_live, 0u);
    }
    EXPECT_TRUE(created);
}

/* test_blob_roundtrip: de_blob_data returns exactly the source bytes. */
TEST_F(blob, roundtrip, blob_fx) {
    de_asset_system *s = f->s;
    void *src = NULL;
    u64 src_size = 0;
    ASSERT_EQ(de_file_read_all("scn:/assets/heart.bin", de_heap_allocator(), &src, &src_size),
              DE_OK);
    ASSERT_EQ(src_size, 270u);

    de_blob b = de_asset_blob(s, de_asset_id_from("ui/heart"));
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    u64 n = 99;
    EXPECT_TRUE(de_blob_data(s, b, &n) == NULL); /* not loaded yet */
    EXPECT_EQ(n, 0u);

    de_load_handle h;
    ASSERT_EQ((int)load_wait(s, "ui/heart", &h), (int)DE_LOAD_READY);
    const u8 *p = (const u8 *)de_blob_data(s, b, &n);
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(n, src_size);
    EXPECT_MEMEQ(p, src, (size_t)n);
    EXPECT_EQ(p[n], 0u); /* a NUL follows the bytes */
    EXPECT_TRUE(de_blob_data(s, b, NULL) == p);
    /* The resident size is the blob's size. */
    const de__slot *sl = de__slot_get(s, b.id);
    ASSERT_TRUE(sl != NULL);
    EXPECT_EQ(sl->bytes, src_size);
    DE_FREE(de_heap_allocator(), src, (size_t)src_size + 1u);

    /* The empty blob is "" with size 0. */
    de_load_handle he;
    ASSERT_EQ((int)load_wait(s, "ui/empty", &he), (int)DE_LOAD_READY);
    de_blob e = de_asset_blob(s, de_asset_id_from("ui/empty"));
    n = 99;
    p = (const u8 *)de_blob_data(s, e, &n);
    ASSERT_TRUE(p != NULL);
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(p[0], 0u);

    /* Wrong handles: NULL and size 0. */
    n = 7;
    EXPECT_TRUE(de_blob_data(s, (de_blob){0}, &n) == NULL);
    EXPECT_EQ(n, 0u);
    EXPECT_TRUE(de_blob_data(NULL, b, &n) == NULL);
    de_prefab notblob = de_asset_prefab(s, de_asset_id_from("props/crate"));
    ASSERT_TRUE(DE_HANDLE_VALID(notblob));
    EXPECT_TRUE(de_blob_data(s, (de_blob){notblob.id}, &n) == NULL);
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_blob(s, de_asset_id_from("levels/level"))));

    /* A missing source fails the load. */
    de_load_handle hm;
    EXPECT_EQ((int)load_wait(s, "ui/missing", &hm), (int)DE_LOAD_FAILED);
    EXPECT_TRUE(de_blob_data(s, de_asset_blob(s, de_asset_id_from("ui/missing")), &n) == NULL);

    de_asset_release(s, hm);
    de_asset_release(s, he);
    de_asset_release(s, h);
    /* Evicted: the handle stays, the data is gone. */
    de_asset_flush_unused(s);
    EXPECT_TRUE(de_blob_data(s, b, &n) == NULL);
}
