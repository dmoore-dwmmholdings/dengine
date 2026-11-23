#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "de_test.h"
#include "de_alloc_guard.h"
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include "asset/backend_internal.h"
#include "asset/load_internal.h"
#include "../anim/anim_test_assets.h"
#include <dengine/de_platform.h>
#include <math.h>

/* DENG-709: the production asset.animationPreview handler against a real asset system with a held
 * backend. Only the clock is controlled; loads complete when the test says so. The error cases
 * follow the DIDE mock (mock-engine/src/animation.rs, DIDE-308). */
static u64 test_now;
static u64 anim_clock(void) {
    return test_now;
}
#define de_time_now_ns              anim_clock
#define de__ed_animation_register   anim_register
#define de__ed_animation_invalidate anim_invalidate
#define de__ed_animation_shutdown   anim_shutdown
#include "host/editor/editor_animation.c"
#undef de_time_now_ns
#undef de__ed_animation_register
#undef de__ed_animation_invalidate
#undef de__ed_animation_shutdown

typedef struct anim_fx {
    de_engine engine;
    de__editor editor;
    u64 arena[8192];
    char output[32768];
    char reply[32768];
    size_t reply_size;
    int fake_anim;
    u32 submits;
    anim_blob skel_blob, clip_blob;
    de__anim_asset skeleton, clip, model;
} anim_fx;

static void held_submit(void *user, de_asset_system *s, u32 slot, de__load_priority priority) {
    (void)s;
    (void)slot;
    (void)priority;
    ++((anim_fx *)user)->submits;
}
static void held_cancel(void *user, de_asset_system *s, u32 slot) {
    (void)user;
    (void)s;
    (void)slot;
}
static void held_priority(void *user, de_asset_system *s, u32 slot, de__load_priority priority) {
    (void)user;
    (void)s;
    (void)slot;
    (void)priority;
}
static void anim_fx_setup(anim_fx *f) {
    test_now = 1000;
    ASSERT_EQ(de_log_init(&(de_log_desc){.no_console_sink = true}), DE_OK);
    ASSERT_EQ(de_fs_mount("animpreview", DE_ANIM_PREVIEW_DATA, DE_MOUNT_READ_ONLY, 0), DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.no_builtin_types = true},
                                     &f->engine.assets),
              DE_OK);
    static const char *const types[] = {"model", "anim_clip", "skeleton", "texture"};
    for (u32 i = 0; i < 4; ++i)
        ASSERT_TRUE(DE_HANDLE_VALID(
            de_asset_type_register(f->engine.assets, &(de_asset_type_desc){.name = types[i]})));
    de__load_backend backend = {held_submit, held_cancel, held_priority, f, NULL};
    de__asset_set_backend(f->engine.assets, &backend);
    ASSERT_EQ(de_asset_manifest_load(f->engine.assets, "animpreview:/assets.toml"), DE_OK);
    f->skel_blob = skeleton_blob(3);
    f->clip_blob = clip_blob(3);
    f->skeleton.kind = 1;
    ASSERT_TRUE(de_cooked_skeleton_parse(f->skel_blob.data, f->skel_blob.bytes,
                                         &f->skeleton.skeleton, NULL));
    f->clip.kind = 2;
    ASSERT_TRUE(
        de_cooked_clip_parse(f->clip_blob.data, f->clip_blob.bytes, &f->clip.clip, NULL));
    f->clip.clip.skeleton_id = de_asset_id_from("rig").hash;
    f->engine.anim = (de_anim_system *)(void *)&f->fake_anim;
    f->editor.e = &f->engine;
    f->editor.arena = (u8 *)f->arena;
    f->editor.arena_cap = sizeof f->arena;
    f->editor.out = f->output;
    f->editor.out_cap = sizeof f->output;
    f->editor.connected = f->editor.authed = true;
    anim_register(&f->editor);
}
static u32 refs_of(anim_fx *f, const char *name) {
    u32 slot = de__slot_find(f->engine.assets, de_asset_id_from(name));
    u32 refs = 0;
    if (slot)
        de__slot_refs(de__slot_get(f->engine.assets, slot), &refs, NULL);
    return refs;
}
static void anim_fx_teardown(anim_fx *f) {
    anim_shutdown(&f->editor);
    EXPECT_EQ(preview.count, 0u);
    static const char *const names[] = {"wave", "rig", "hero", "c1", "c2", "c3",
                                        "c4",   "c5",  "c6",   "c7"};
    for (u32 i = 0; i < sizeof names / sizeof names[0]; ++i) {
        EXPECT_EQ(refs_of(f, names[i]), 0u);
        u32 slot = de__slot_find(f->engine.assets, de_asset_id_from(names[i]));
        if (slot)
            de_atomic_store_ptr(&de__slot_get(f->engine.assets, slot)->object, NULL,
                                DE_ATOMIC_RELEASE);
    }
    de_asset_system_destroy(f->engine.assets);
    free(f->skel_blob.data);
    free(f->clip_blob.data);
    (void)de_fs_unmount("animpreview", DE_ANIM_PREVIEW_DATA);
    de_log_shutdown();
    de_error_clear();
}
typedef struct anim_query {
    const char *id, *parent;
    f32 time;
    bool versions;
    u32 clip_version, skeleton_version;
} anim_query;
#define Q(i, p, t) ((anim_query){.id = (i), .parent = (p), .time = (t)})
static void request(anim_fx *f, u64 request_id, anim_query q) {
    char params[512];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    mpack_start_map(&w, 2 + (q.parent != NULL) + (q.versions ? 2 : 0));
    mpack_write_cstr(&w, "id");
    mpack_write_cstr(&w, q.id);
    mpack_write_cstr(&w, "timeSeconds");
    mpack_write_float(&w, q.time);
    if (q.parent) {
        mpack_write_cstr(&w, "parent");
        mpack_write_cstr(&w, q.parent);
    }
    if (q.versions) {
        mpack_write_cstr(&w, "clipVersion");
        mpack_write_u32(&w, q.clip_version);
        mpack_write_cstr(&w, "skeletonVersion");
        mpack_write_u32(&w, q.skeleton_version);
    }
    mpack_finish_map(&w);
    size_t bytes = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    de_ep_request req = {.id = request_id, .params = {params, bytes}};
    de_ep_arena a = {(u8 *)f->arena, sizeof f->arena, 0};
    de__ed_respond_begin(&f->editor, &w);
    f->editor.handlers[DE_EP_METHOD_ASSET_ANIMATION_PREVIEW](&f->editor, &req, &a, &w);
    de__ed_respond_limit(&f->editor, &w, request_id);
    f->reply_size = mpack_writer_buffer_used(&w);
    ASSERT_LE(f->reply_size, sizeof f->reply);
    memcpy(f->reply, w.buffer, f->reply_size);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
}
/* The reply's error code, or 0 for a result. `message` receives the error message. */
static int reply_error(anim_fx *f, char *message, size_t cap) {
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, f->reply, f->reply_size);
    mpack_tree_parse(&tree);
    mpack_node_t root = mpack_tree_root(&tree);
    int code = 0;
    if (mpack_node_map_contains_cstr(root, "error")) {
        mpack_node_t e = mpack_node_map_cstr(root, "error");
        code = mpack_node_i32(mpack_node_map_cstr(e, "code"));
        if (message)
            mpack_node_copy_cstr(mpack_node_map_cstr(e, "message"), message, cap);
    }
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    return code;
}
static int code_of(anim_fx *f, u64 id, anim_query q) {
    request(f, id, q);
    return reply_error(f, NULL, 0);
}
static void expect_busy(anim_fx *f, const char *why) {
    char message[256] = {0};
    EXPECT_EQ(reply_error(f, message, sizeof message), DE_EP_ERR_BUSY);
    EXPECT_STREQ(message, why);
}
static void complete(anim_fx *f, const char *name, void *object) {
    u32 slot = de__slot_find(f->engine.assets, de_asset_id_from(name));
    ASSERT_NE(slot, 0u);
    de_atomic_store_ptr(&de__slot_get(f->engine.assets, slot)->object, object, DE_ATOMIC_RELEASE);
    de__slot_complete(f->engine.assets, slot, DE_OK, 64);
    de__load_update(f->engine.assets);
}
typedef struct anim_reply {
    u32 clip_version, skeleton_version, joints, pose;
    f32 time, duration;
    bool metadata;
} anim_reply;
static anim_reply result_of(anim_fx *f) {
    anim_reply r = {0};
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, f->reply, f->reply_size);
    mpack_tree_parse(&tree);
    mpack_node_t res = mpack_node_map_cstr(mpack_tree_root(&tree), "result");
    r.clip_version = mpack_node_u32(mpack_node_map_cstr(res, "clipVersion"));
    r.skeleton_version = mpack_node_u32(mpack_node_map_cstr(res, "skeletonVersion"));
    r.time = mpack_node_float(mpack_node_map_cstr(res, "timeSeconds"));
    r.duration = mpack_node_float(mpack_node_map_cstr(res, "durationSeconds"));
    mpack_node_t pose = mpack_node_map_cstr(res, "pose");
    r.pose = (u32)mpack_node_array_length(pose);
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(mpack_node_array_at(pose, 0), "matrix")),
              12u);
    r.metadata = mpack_node_map_contains_cstr(res, "metadata") &&
                 mpack_node_type(mpack_node_map_cstr(res, "metadata")) == mpack_type_map;
    if (r.metadata)
        r.joints = (u32)mpack_node_array_length(
            mpack_node_map_cstr(mpack_node_map_cstr(res, "metadata"), "joints"));
    EXPECT_EQ(mpack_tree_error(&tree), mpack_ok);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    return r;
}
/* wave and its skeleton rig: Busy while each loads, then a sampled pose. */
static anim_reply make_ready(anim_fx *f) {
    request(f, 1, Q("wave", NULL, 5));
    expect_busy(f, "cooked animation source is loading; retry");
    complete(f, "wave", &f->clip);
    request(f, 2, Q("wave", NULL, 5));
    expect_busy(f, "cooked animation source is loading; retry");
    complete(f, "rig", &f->skeleton);
    request(f, 3, Q("wave", NULL, 5));
    EXPECT_EQ(reply_error(f, NULL, 0), 0);
    return result_of(f);
}

TEST_F(editor_animation_service, busy_then_cooked_pose_and_version_elision, anim_fx) {
    bool advertised = false;
    for (u32 i = 0; i < f->editor.cap_count; ++i)
        advertised |= strcmp(f->editor.caps[i], "assets.animationPreview") == 0;
    EXPECT_TRUE(advertised);
    anim_reply first = make_ready(f);
    EXPECT_EQ(first.pose, 3u);
    EXPECT_TRUE(first.metadata);
    EXPECT_EQ(first.joints, 3u);
    EXPECT_FLOAT_EQ(first.time, 1, .0001); /* clamped to the cooked duration */
    EXPECT_FLOAT_EQ(first.duration, 1, .0001);
    EXPECT_EQ(preview.count, 2u);
    EXPECT_EQ(refs_of(f, "wave"), 1u);
    EXPECT_EQ(refs_of(f, "rig"), 1u);
    size_t full_bytes = f->reply_size;
    request(f, 4,
            (anim_query){.id = "wave", .time = .25f, .versions = true, .clip_version = first.clip_version, .skeleton_version = first.skeleton_version});
    anim_reply elided = result_of(f);
    EXPECT_FALSE(elided.metadata);
    EXPECT_EQ(elided.pose, 3u);
    EXPECT_TRUE(f->reply_size < full_bytes);
    request(f, 5,
            (anim_query){.id = "wave", .time = -3, .versions = true, .clip_version = first.clip_version, .skeleton_version = first.skeleton_version + 1});
    anim_reply stale = result_of(f);
    EXPECT_TRUE(stale.metadata);
    EXPECT_FLOAT_EQ(stale.time, 0, .0001);
    /* Repeated warm requests use only the request arena and the output buffer. */
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    for (u32 i = 0; i < 100; ++i)
        request(f, 100 + i,
                (anim_query){.id = "wave", .time = (f32)i / 100, .versions = true, .clip_version = first.clip_version, .skeleton_version = first.skeleton_version});
    de_alloc_guard_end(&guard);
    EXPECT_EQ(de_alloc_guard_offences(&guard), 0u);
    EXPECT_EQ(reply_error(f, NULL, 0), 0);
}

TEST_F(editor_animation_service, explicit_errors_match_the_mock, anim_fx) {
    EXPECT_EQ(code_of(f, 1, Q("nope", NULL, 0)), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(code_of(f, 2, Q("tex", NULL, 0)), DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(code_of(f, 3, Q("wave", NULL, NAN)), DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(code_of(f, 4, Q("wave", NULL, INFINITY)), DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(code_of(f, 5, Q("tex#Spin", "tex", 0)), DE_EP_ERR_INVALID_PARAMS);
    EXPECT_EQ(code_of(f, 6, Q("ghost#Spin", "ghost", 0)), DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(code_of(f, 7, Q("wave", "hero", 0)), DE_EP_ERR_INVALID_PARAMS);
    /* A half version pair is invalid. */
    char params[64];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    mpack_start_map(&w, 3);
    mpack_write_cstr(&w, "id");
    mpack_write_cstr(&w, "wave");
    mpack_write_cstr(&w, "timeSeconds");
    mpack_write_float(&w, 0);
    mpack_write_cstr(&w, "clipVersion");
    mpack_write_u32(&w, 1);
    mpack_finish_map(&w);
    size_t bytes = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    de_ep_request req = {.id = 8, .params = {params, bytes}};
    de_ep_arena a = {(u8 *)f->arena, sizeof f->arena, 0};
    de__ed_respond_begin(&f->editor, &w);
    f->editor.handlers[DE_EP_METHOD_ASSET_ANIMATION_PREVIEW](&f->editor, &req, &a, &w);
    f->reply_size = mpack_writer_buffer_used(&w);
    memcpy(f->reply, w.buffer, f->reply_size);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    EXPECT_EQ(reply_error(f, NULL, 0), DE_EP_ERR_INVALID_PARAMS);
    /* Errors before a load retain nothing. */
    EXPECT_EQ(preview.count, 0u);
    /* A generated child is declared only once its model parent is loaded. */
    request(f, 9, Q("hero#Spin", "hero", 0));
    expect_busy(f, "cooked animation source is loading; retry");
    EXPECT_EQ(refs_of(f, "hero"), 1u);
    complete(f, "hero", &f->model);
    EXPECT_EQ(code_of(f, 10, Q("hero#Spin", "hero", 0)), DE_EP_ERR_NOT_FOUND);
    /* A clip without a skeletal binding is invalid. */
    request(f, 11, Q("c1", NULL, 0));
    expect_busy(f, "cooked animation source is loading; retry");
    de__anim_asset unbound = f->clip;
    unbound.clip.skeleton_id = 0;
    complete(f, "c1", &unbound);
    EXPECT_EQ(code_of(f, 12, Q("c1", NULL, 0)), DE_EP_ERR_INVALID_PARAMS);
    /* A request arena too small for the pose fails cleanly. */
    request(f, 13, Q("wave", NULL, 0));
    complete(f, "wave", &f->clip);
    request(f, 13, Q("wave", NULL, 0));
    expect_busy(f, "cooked animation source is loading; retry");
    complete(f, "rig", &f->skeleton);
    char params2[64];
    mpack_writer_init(&w, params2, sizeof params2);
    mpack_start_map(&w, 2);
    mpack_write_cstr(&w, "id");
    mpack_write_cstr(&w, "wave");
    mpack_write_cstr(&w, "timeSeconds");
    mpack_write_float(&w, 0);
    mpack_finish_map(&w);
    bytes = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    req = (de_ep_request){.id = 14, .params = {params2, bytes}};
    u8 small[64];
    a = (de_ep_arena){small, sizeof small, 0};
    de__ed_respond_begin(&f->editor, &w);
    f->editor.handlers[DE_EP_METHOD_ASSET_ANIMATION_PREVIEW](&f->editor, &req, &a, &w);
    f->reply_size = mpack_writer_buffer_used(&w);
    memcpy(f->reply, w.buffer, f->reply_size);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    EXPECT_EQ(reply_error(f, NULL, 0), DE_EP_ERR_INTERNAL);
    EXPECT_TRUE(a.used <= sizeof small);
}

TEST_F(editor_animation_service, bounded_set_idles_out_and_cleans_up, anim_fx) {
    make_ready(f);
    /* wave + rig + six clip loads fill the set; rig is shared. */
    static const char *const clips[] = {"c1", "c2", "c3", "c4", "c5", "c6"};
    for (u32 i = 0; i < 6; ++i) {
        request(f, 10 + i, Q(clips[i], NULL, 0));
        expect_busy(f, "cooked animation source is loading; retry");
    }
    EXPECT_EQ(preview.count, DE__ED_ANIM_SOURCES);
    /* The declared check comes first, then the bound. */
    EXPECT_EQ(code_of(f, 20, Q("nope", NULL, 0)), DE_EP_ERR_NOT_FOUND);
    request(f, 21, Q("c7", NULL, 0));
    expect_busy(f, "animation source set is full");
    EXPECT_EQ(refs_of(f, "c7"), 0u);
    /* A retained source is still served when the set is full. */
    test_now += DE__ED_ANIM_IDLE_NS / 2;
    EXPECT_EQ(code_of(f, 22, Q("wave", NULL, 0)), 0);
    /* Idle sources are released; the recently used wave and rig stay. */
    test_now += DE__ED_ANIM_IDLE_NS / 2 + 1;
    request(f, 23, Q("c7", NULL, 0));
    expect_busy(f, "cooked animation source is loading; retry");
    EXPECT_EQ(preview.count, 3u);
    for (u32 i = 0; i < 6; ++i)
        EXPECT_EQ(refs_of(f, clips[i]), 0u);
    /* The frame tick idles out the rest. */
    test_now += DE__ED_ANIM_IDLE_NS;
    tick(&f->editor);
    EXPECT_EQ(preview.count, 0u);
    EXPECT_EQ(refs_of(f, "wave"), 0u);
    /* Disconnect (the editor's reset hooks) releases every retained source. */
    request(f, 24, Q("wave", NULL, 0));
    EXPECT_EQ(preview.count, 2u);
    for (u32 i = 0; i < f->editor.reset_count; ++i)
        f->editor.resets[i](&f->editor);
    EXPECT_EQ(preview.count, 0u);
    EXPECT_EQ(refs_of(f, "wave"), 0u);
    EXPECT_EQ(refs_of(f, "rig"), 0u);
    /* Reimport or hot reload invalidates the set too. */
    request(f, 25, Q("wave", NULL, 0));
    EXPECT_EQ(preview.count, 2u);
    anim_invalidate(&f->editor);
    EXPECT_EQ(preview.count, 0u);
}

TEST_F(editor_animation_service, absent_capability_is_unsupported, anim_fx) {
    anim_shutdown(&f->editor);
    f->engine.anim = NULL;
    f->editor.cap_count = 0;
    anim_register(&f->editor);
    EXPECT_EQ(f->editor.cap_count, 0u);
    EXPECT_EQ(code_of(f, 1, Q("wave", NULL, 0)), DE_EP_ERR_UNSUPPORTED);
    EXPECT_EQ(preview.count, 0u);
}
