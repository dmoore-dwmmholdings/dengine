#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "host/editor/editor_gizmo.h"
#include "host/host_internal.h"
#include <dengine/de_asset.h>
#include <dengine/de_ecs.h>
#include <dengine/de_math.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>
#include <math.h>
#include <stdio.h>

/* Actual editor handlers and CPU gizmo input; no renderer, window or transport. */
typedef struct safety_fx {
    de_engine engine;
    de__host host;
    de__editor ed;
    de_builtin_component_ids ids;
    de_task_system *tasks;
    unsigned char arena[65536];
    char note[32768];
} safety_fx;

static void safety_fx_setup(safety_fx *f) {
    memset(f, 0, sizeof *f);
    ASSERT_EQ(de_log_init(&(de_log_desc){.no_console_sink = true}), DE_OK);
    ASSERT_EQ(de_type_registry_create(NULL, &f->engine.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->engine.types}, &f->engine.world), DE_OK);
    f->ids = de_builtin_components(f->engine.world);
    f->ed.e = &f->engine;
    f->ed.h = &f->host;
    f->ed.arena = f->arena;
    f->ed.arena_cap = sizeof f->arena;
    f->ed.note = f->note;
    f->ed.note_cap = sizeof f->note;
    de__ed_gizmo_reset(&f->ed);
}
static void safety_fx_teardown(safety_fx *f) {
    de__ed_gizmo_reset(&f->ed);
    de_world_destroy(f->engine.world);
    if (f->engine.assets)
        de_asset_system_destroy(f->engine.assets);
    if (f->tasks)
        de_task_system_destroy(f->tasks);
    de_type_registry_destroy(f->engine.types);
    de_log_shutdown();
}
static de_entity actor(safety_fx *f, f64 x) {
    de_entity e = de_entity_spawn(f->engine.world);
    ASSERT_TRUE(de_entity_alive(f->engine.world, e));
    DeTransform *t = de_entity_add(f->engine.world, e, f->ids.transform);
    ASSERT_TRUE(t != NULL);
    *t = de_transform_identity();
    t->position.x = x;
    return e;
}
static DeTransform *transform(safety_fx *f, de_entity e) {
    DeTransform *t = de_entity_get(f->engine.world, e, f->ids.transform);
    ASSERT_TRUE(t != NULL);
    return t;
}
static void tool(const char *name, const char *space, const char *pivot) {
    const de_ep_gizmo_tool_params q = {
        .tool = de_ep_cstr(name), .space = de_ep_cstr(space), .pivot = de_ep_cstr(pivot)};
    de__ed_gizmo_set_tool(&q);
}
static de__ed_cam camera(void) {
    return (de__ed_cam){.pos = {0, -10, 0}, .rot = {0, 0, 0, 1}, .ortho_h = 10, .w = 800, .h = 600};
}
static void pointer(safety_fx *f, const de__ed_cam *c, const char *action, f32 x, f32 y) {
    const de_ep_input_event e = {
        .type = de_ep_cstr("pointer"), .has_action = true, .action = de_ep_cstr(action)};
    de__ed_gizmo_input(&f->ed, 1, c, &e, x, y);
}
TEST_F(editor_safety, empty_and_invalid_selections_cannot_start_a_drag, safety_fx) {
    const de__ed_cam c = camera();
    const de_entity live = actor(f, 0), dead = actor(f, 5);
    de_entity_despawn(f->engine.world, dead);
    const de_entity bare = de_entity_spawn(f->engine.world);
    const u64 ids[] = {0, dead.bits, bare.bits};
    tool("move", "world", "origin");
    for (u32 count = 0; count <= 3; ++count) {
        de__ed_gizmo_set_selection(ids, count);
        pointer(f, &c, "down", 400, 300);
        pointer(f, &c, "move", 460, 300);
        pointer(f, &c, "up", 460, 300);
        EXPECT_FLOAT_EQ(transform(f, live)->position.x, 0, 0);
        EXPECT_EQ(f->ed.seq, 0u);
    }
}
TEST_F(editor_safety, first_valid_entity_and_center_ignore_invalid_entries, safety_fx) {
    const de__ed_cam c = camera();
    const de_entity a = actor(f, 0), b = actor(f, 2);
    const de_entity bare = de_entity_spawn(f->engine.world);
    const u64 ids[] = {0, bare.bits, a.bits, 0, b.bits};
    for (u32 center = 0; center < 2; ++center) {
        tool("move", "world", center ? "center" : "origin");
        de__ed_gizmo_set_selection(ids, 5);
        const f32 start = center ? 460.0f : 400.0f;
        pointer(f, &c, "down", start, 300);
        pointer(f, &c, "move", start + 60, 300);
        EXPECT_FLOAT_EQ(transform(f, a)->position.x, 1, 1e-6);
        EXPECT_FLOAT_EQ(transform(f, b)->position.x, 3, 1e-6);
        pointer(f, &c, "cancel", start + 60, 300);
        EXPECT_FLOAT_EQ(transform(f, a)->position.x, 0, 0);
        EXPECT_FLOAT_EQ(transform(f, b)->position.x, 2, 0);
    }
    EXPECT_EQ(f->ed.seq, 4u); /* Each drag emits a preview and its cancellation. */
}
TEST_F(editor_safety, local_axis_uses_the_first_valid_entity_rotation, safety_fx) {
    const de__ed_cam c = camera();
    const de_entity a = actor(f, 0), b = actor(f, 2);
    transform(f, a)->rotation = (de_quat){0, (f32)sin(DE_PI / 4), 0, (f32)cos(DE_PI / 4)};
    const u64 ids[] = {0, a.bits, b.bits};
    tool("move", "local", "origin");
    de__ed_gizmo_set_selection(ids, 3);
    /* The first valid entity's local X points down the view, along world -Z. */
    pointer(f, &c, "down", 400, 360);
    pointer(f, &c, "move", 400, 420);
    EXPECT_FLOAT_EQ(transform(f, a)->position.z, -1, 1e-5);
    EXPECT_FLOAT_EQ(transform(f, b)->position.z, -1, 1e-5);
    EXPECT_FLOAT_EQ(transform(f, a)->position.x, 0, 1e-5);
    EXPECT_FLOAT_EQ(transform(f, b)->position.x, 2, 1e-5);
    pointer(f, &c, "cancel", 400, 420);
    EXPECT_FLOAT_EQ(transform(f, a)->position.z, 0, 0);
    EXPECT_FLOAT_EQ(transform(f, b)->position.z, 0, 0);
}
TEST_F(editor_safety, visible_view_ring_rotates_and_cancel_restores_the_transform, safety_fx) {
    const de__ed_cam c = camera();
    const de_entity e = actor(f, 0);
    tool("rotate", "world", "origin");
    de__ed_gizmo_set_selection(&e.bits, 1);
    /* At 600px high the outer view ring has radius 600 * .12 * 1.1 = 79.2px. */
    pointer(f, &c, "down", 479.2f, 300);
    pointer(f, &c, "move", 400, 220.8f);
    const de_quat q = transform(f, e)->rotation;
    EXPECT_FLOAT_EQ(fabsf(q.y), sqrt(.5), 1e-5);
    EXPECT_FLOAT_EQ(fabsf(q.w), sqrt(.5), 1e-5);
    EXPECT_FLOAT_EQ(q.x, 0, 1e-6);
    EXPECT_FLOAT_EQ(q.z, 0, 1e-6);
    EXPECT_EQ(f->ed.seq, 1u);
    pointer(f, &c, "cancel", 400, 220.8f);
    EXPECT_FLOAT_EQ(transform(f, e)->rotation.w, 1, 0);
    EXPECT_FLOAT_EQ(transform(f, e)->rotation.y, 0, 0);
}
TEST_F(editor_safety, clipped_ring_arcs_do_not_bridge_across_the_gizmo_center, safety_fx) {
    de__ed_cam c = camera();
    c.pos[1] = 0;
    c.fov_y = 1;
    c.near = 1;
    const de_entity e = actor(f, 0);
    DeTransform *t = transform(f, e);
    t->position.y = 1.01;
    t->rotation = (de_quat){0, 0, (f32)sin(DE_PI / 8), (f32)cos(DE_PI / 8)};
    const de_quat original = t->rotation;
    tool("rotate", "local", "origin");
    de__ed_gizmo_set_selection(&e.bits, 1);
    /* The oblique axis rings cross the near plane. Their separated visible arcs
     * must not become an artificial selectable segment through the centre. */
    pointer(f, &c, "down", 400, 300);
    pointer(f, &c, "move", 410, 300);
    pointer(f, &c, "up", 410, 300);
    EXPECT_EQ(f->ed.seq, 0u);
    EXPECT_MEMEQ(&transform(f, e)->rotation, &original, sizeof original);
    /* An actual visible arc endpoint still starts a drag. */
    pointer(f, &c, "down", 400, 364.8f);
    pointer(f, &c, "move", 404, 364.8f);
    EXPECT_EQ(f->ed.seq, 1u);
    pointer(f, &c, "cancel", 404, 364.8f);
    EXPECT_MEMEQ(&transform(f, e)->rotation, &original, sizeof original);
}

#endif
