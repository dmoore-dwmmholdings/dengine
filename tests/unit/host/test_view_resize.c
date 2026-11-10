#include "de_test.h"
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include "render/world/render_world.h"
#include <dengine/de_gpu.h>
#include <dengine/de_render.h>

/* The real handler and bridge layout, with traps before any renderer/resource work.
 * No device, renderer, world, window or transport is created by these tests. */
static u32 resource_calls;
static _Noreturn void forbidden_resource_call(void) {
    ++resource_calls;
    FAIL("invalid resize reached renderer/resource work");
}
static void resize_wait(de_renderer *renderer) {
    (void)renderer;
    forbidden_resource_call();
}
static void resize_view_destroy(de_renderer *renderer, de_view view) {
    (void)renderer;
    (void)view;
    forbidden_resource_call();
}
static de_view resize_view_create(de_renderer *renderer, const de_view_desc *desc) {
    (void)renderer;
    (void)desc;
    forbidden_resource_call();
}
static void resize_texture_destroy(de_gpu_device *device, de_gpu_texture texture) {
    (void)device;
    (void)texture;
    forbidden_resource_call();
}
static de_gpu_texture resize_texture_create(de_gpu_device *device,
                                            const de_gpu_texture_desc *desc) {
    (void)device;
    (void)desc;
    forbidden_resource_call();
}
/* Keep the traps indirect: an optimized MSVC build otherwise diagnoses code after
 * their calls in the included handler as unreachable. Each target still fails
 * immediately; no renderer/resource operation or substitute handle is allowed. */
static void (*volatile resize_wait_call)(de_renderer *) = resize_wait;
static void (*volatile resize_view_destroy_call)(de_renderer *, de_view) = resize_view_destroy;
static de_view (*volatile resize_view_create_call)(de_renderer *,
                                                   const de_view_desc *) = resize_view_create;
static void (*volatile resize_texture_destroy_call)(de_gpu_device *,
                                                    de_gpu_texture) = resize_texture_destroy;
static de_gpu_texture (*volatile resize_texture_create_call)(
    de_gpu_device *, const de_gpu_texture_desc *) = resize_texture_create;
#define de__render_world_wait_idle resize_wait_call
#define de_view_destroy            resize_view_destroy_call
#define de_view_create             resize_view_create_call
#define de_gpu_texture_destroy     resize_texture_destroy_call
#define de_gpu_texture_create      resize_texture_create_call
#include "host/editor/editor_views.c"
#undef de__render_world_wait_idle
#undef de_view_destroy
#undef de_view_create
#undef de_gpu_texture_destroy
#undef de_gpu_texture_create

static ed_view *seed_view(bool game) {
    memset(&g_views, 0, sizeof g_views);
    resource_calls = 0;
    g_views.next_id = 37;
    ed_view *v = &g_views.views[0];
    v->id = 7;
    v->game = game;
    v->paused = v->hidden_mask = true;
    v->w = 640;
    v->h = 480;
    v->sw = 800;
    v->sh = 600;
    v->scale = 1.25f;
    v->max_fps = 30;
    v->next_readback_ns = 1234;
    v->rv = (de_view){0x1234};
    v->target = (de_gpu_texture){0x5678};
    v->cam = (de_entity){.bits = 0x9012};
    v->cam_dirty = true;
    v->cam_pos[0] = 10;
    v->cam_rot[3] = 1;
    v->camera.fov_y = 1;
    snprintf(v->render_mode, sizeof v->render_mode, "lit");
    for (u32 i = 0; i < ED_RB_SLOTS; ++i) {
        v->rb[i] = (de_gpu_buffer){100 + i};
        v->rb_map[i] = (const u8 *)&g_views;
    }
    v->rb_pitch = 6400;
    v->rb_w = 800;
    v->rb_h = 600;
    de_atomic_store_u64(&v->produced, 45, DE_ATOMIC_RELAXED);
    v->consumed = 42;
    v->frames = 39;
    v->focused = v->captured = true;
    v->show = 123;
    v->ring_count = 2;
    v->next_generation = 19;
    v->fence_value = 23;
    for (u32 i = 0; i < v->ring_count; ++i) {
        ed_ring *ring = &v->rings[i];
        ring->generation = 17 + i;
        ring->w = 800;
        ring->h = 600;
        ring->last_fence = 13 + i;
        ring->released = i == 0;
#if ED_MAC_VIEWS
        ring->bytes = 800 * 600 * 4 + 64;
        for (u32 j = 0; j < ED_RING; ++j) {
            snprintf(ring->path[j], sizeof ring->path[j], "unused-ring-%u-%u", i, j);
            ring->map[j] = &g_views;
        }
#else
        for (u32 j = 0; j < ED_RING; ++j) {
            ring->tex[j] = (ID3D12Resource *)(uintptr_t)(100 + i * ED_RING + j);
            ring->handle[j] = (HANDLE)(uintptr_t)(200 + i * ED_RING + j);
        }
#endif
    }
    return v;
}

static void resize_request(u32 view, u32 width, u32 height, bool malformed,
                           de_ep_error expected_error) {
    de__ed_views before;
    memcpy(&before, &g_views, sizeof before);
    char params[128], response[512];
    u64 storage[128];
    mpack_writer_t writer;
    mpack_writer_init(&writer, params, sizeof params);
    if (malformed) {
        mpack_start_map(&writer, 4);
        mpack_write_cstr(&writer, "view");
        mpack_write_u32(&writer, view);
        mpack_write_cstr(&writer, "width");
        mpack_write_cstr(&writer, "8193");
        mpack_write_cstr(&writer, "height");
        mpack_write_u32(&writer, height);
        mpack_write_cstr(&writer, "scale");
        mpack_write_float(&writer, 2);
        mpack_finish_map(&writer);
    } else {
        de_ep_write_view_resize_params(
            &writer, &(de_ep_view_resize_params){
                         .view = view, .width = width, .height = height, .scale = 2});
    }
    size_t size = mpack_writer_buffer_used(&writer);
    ASSERT_EQ(mpack_writer_destroy(&writer), mpack_ok);
    de_ep_request request = {.id = 57, .params = {params, size}};
    de_ep_arena arena = {(u8 *)storage, sizeof storage, 0};
    de_engine engine = {0};
    de__editor editor = {.e = &engine};
    mpack_writer_init(&writer, response, sizeof response);
    on_resize(&editor, &request, &arena, &writer);
    size = mpack_writer_buffer_used(&writer);
    ASSERT_EQ(mpack_writer_destroy(&writer), mpack_ok);
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, response, size);
    mpack_tree_parse(&tree);
    mpack_node_t root = mpack_tree_root(&tree);
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(root, "id")), 57u);
    ASSERT_TRUE(mpack_node_map_contains_cstr(root, "error"));
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(mpack_node_map_cstr(root, "error"), "code")),
              expected_error);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    EXPECT_EQ(resource_calls, 0u);
    EXPECT_MEMEQ(&g_views, &before, sizeof before);
}

TEST(editor_view_resize, oversized_scene_and_game_requests_preserve_complete_state) {
    static const u32 sizes[][2] = {{8193, 1},       {1, 8193}, {UINT32_MAX, 1},
                                   {1, UINT32_MAX}, {8193, 0}, {0, 8193}};
    for (u32 game = 0; game < 2; ++game)
        for (u32 i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
            ed_view *view = seed_view(game != 0);
            resize_request(view->id, sizes[i][0], sizes[i][1], false, DE_EP_ERR_INVALID_PARAMS);
        }
}

TEST(editor_view_resize, unknown_view_retains_not_found_precedence_without_mutation) {
    seed_view(false);
    resize_request(99, 8193, 1, false, DE_EP_ERR_NOT_FOUND);
}

TEST(editor_view_resize, malformed_dimensions_fail_before_mutation) {
    ed_view *view = seed_view(false);
    resize_request(view->id, 8193, 1, true, DE_EP_ERR_INVALID_PARAMS);
}
