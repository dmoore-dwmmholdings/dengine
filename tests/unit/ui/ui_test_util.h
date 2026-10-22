#ifndef DE_UI_TEST_UTIL_H
#define DE_UI_TEST_UTIL_H
#include "de_test.h"
#include "ui/ui_internal.h"
#if defined(DE_UI_TEST_FONTS) || defined(DE_UI_TEST_PACKED_ASSETS)
#include "../text/packed_test_util.h"
#endif
#include <math.h>
#include <string.h>
typedef struct ui_fx {
    de_type_registry *types;
    de_world *world;
    de_ui *ui;
    de_gpu_device *device;
    de_renderer *renderer;
    de_asset_system *assets;
    de_load_handle load;
    char asset_pack[256];
} ui_fx;
static inline void ui_fx_setup(ui_fx *f) {
    ASSERT_EQ(de_type_registry_create(NULL, &f->types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->types}, &f->world), DE_OK);
    ASSERT_EQ(de_ui_create(&(de_ui_desc){.reference_height = 100, .max_nodes = 6000}, &f->ui),
              DE_OK);
    de_ui_set_viewport(f->ui, 400, 100, (de_window_insets){0});
    de_ui_attach(f->ui, f->world);
    ASSERT_TRUE(f->ui->layout_system.id);
}
static inline void ui_fx_teardown(ui_fx *f) {
    de_ui_destroy(f->ui);
#if DE_HAS_RENDER
    if (f->assets) {
        if (f->load.id)
            de_asset_release(f->assets, f->load);
        de_asset_system_destroy(f->assets);
    }
    if (f->renderer)
        de_renderer_destroy(f->renderer);
    if (f->device)
        de_gpu_device_destroy(f->device);
#endif
#if defined(DE_UI_TEST_FONTS) || defined(DE_UI_TEST_PACKED_ASSETS)
    text_fixture_pack_cleanup(f->asset_pack);
#endif
#if defined(DE_UI_ASSET_ROOT)
    (void)de_fs_unmount("uiassets", DE_UI_ASSET_ROOT);
#endif
    de_world_destroy(f->world);
    de_type_registry_destroy(f->types);
}
static inline de_entity ui_node(ui_fx *f, de_entity parent) {
    de_entity e;
    ASSERT_EQ(de_entity_spawn_with(f->world, &f->ui->ids.node, 1, &e), DE_OK);
    if (parent.bits)
        ASSERT_EQ(de_entity_set_parent(f->world, e, parent), DE_OK);
    return e;
}
static inline DeUiNode *ui_node_edit(ui_fx *f, de_entity e) {
    return de_entity_get(f->world, e, f->ui->ids.node);
}
static inline de_vec4 ui_rect(ui_fx *f, de_entity e) {
    const DeUiNode *n = de_entity_get_const(f->world, e, f->ui->ids.node);
    ASSERT_TRUE(n);
    return n->computed_rect;
}
static inline void ui_frame(ui_fx *f) {
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_POST_UPDATE, 1.f / 60, 0), DE_OK);
}
static inline void expect_rect(de_vec4 r, f32 x, f32 y, f32 w, f32 h) {
    EXPECT_FLOAT_EQ(r.x, x, .5f);
    EXPECT_FLOAT_EQ(r.y, y, .5f);
    EXPECT_FLOAT_EQ(r.z, w, .5f);
    EXPECT_FLOAT_EQ(r.w, h, .5f);
}
#endif
