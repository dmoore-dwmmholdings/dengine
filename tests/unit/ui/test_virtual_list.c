#include "ui_test_util.h"
#include "ui/widgets/advanced_internal.h"
TEST_F(ui_virtual_list, hundred_thousand_items_bounded_pool_and_exact_appearance_bind, ui_fx) {
    de_entity e = ui_node(f, (de_entity){0});
    DeUiNode *n = ui_node_edit(f, e);
    n->width = (de_ui_len){DE_UI_PX, 200};
    n->height = (de_ui_len){DE_UI_PX, 100};
    *(DeUiList *)de_entity_add(f->world, e, f->ui->advanced->list) =
        (DeUiList){.item_count = 100000, .item_height = 20};
    de_event_reader reader = de_event_reader_create(f->world, f->ui->event);
    ui_frame(f);
    ui_frame(f);
    ui_frame(f);
    EXPECT_EQ(de_entity_children_count(f->world, e), 7u);
    const de_ui_event *event;
    u32 binds = 0;
    while ((event = de_event_next(f->world, &reader)))
        if (event->kind == DE_UI_EV_BIND_ITEM)
            ++binds;
    /* First zero-size layout may initially allocate two; each row is bound once per appearance. */
    EXPECT_EQ(binds, 7u);
    ui_frame(f);
    binds = 0;
    while ((event = de_event_next(f->world, &reader)))
        if (event->kind == DE_UI_EV_BIND_ITEM)
            ++binds;
    EXPECT_EQ(binds, 0u);
    de_ui_scroll_to(f->ui, e, (de_vec2){0, 40});
    ui_frame(f);
    binds = 0;
    while ((event = de_event_next(f->world, &reader)))
        if (event->kind == DE_UI_EV_BIND_ITEM)
            ++binds;
    EXPECT_EQ(binds, 1u);
    de_ui_scroll_to(f->ui, e, (de_vec2){0, 1000000});
    ui_frame(f);
    binds = 0;
    while ((event = de_event_next(f->world, &reader)))
        if (event->kind == DE_UI_EV_BIND_ITEM)
            ++binds;
    EXPECT_EQ(binds, 7u);
    u64 before = 0, after = 0;
    for (u32 i = 0; i < 4; ++i)
        ui_frame(f);
    for (u32 i = 0; i < DE_MEM_TAG_BUILTIN_COUNT_; ++i)
        before += de_mem_stats_for_tag(i).alloc_count;
    for (u32 i = 0; i < 100; ++i)
        ui_frame(f);
    for (u32 i = 0; i < DE_MEM_TAG_BUILTIN_COUNT_; ++i)
        after += de_mem_stats_for_tag(i).alloc_count;
    EXPECT_EQ(after, before);
}
