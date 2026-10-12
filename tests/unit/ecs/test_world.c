/* test_world.c: world create/destroy, the component registry and tag components (DENG-344). */
#include "ecs_test_util.h"

/* ---- Counting hooks ------------------------------------------------------------------------- */

typedef struct THooked {
    u32 value;
} THooked;

static int g_construct_count;
static int g_destruct_count;

static void hooked_construct(void *obj) {
    ((THooked *)obj)->value = 0xC0FFEEu;
    ++g_construct_count;
}
static void hooked_destruct(void *obj) {
    ((THooked *)obj)->value = 0xDEADu;
    ++g_destruct_count;
}
static void hooked_copy(void *dst, const void *src) {
    memcpy(dst, src, sizeof(THooked));
}

static de_type_id register_hooked(de_type_registry *r, bool with_copy) {
    static const de_field_info fields[] = {DE_FIELD(THooked, value, DE_FK_U32, DE_FIELD_DEFAULT)};
    de_type_desc d = DE_TYPE_DESC(THooked, 1, fields);
    d.is_pod = false;
    d.hooks.construct = hooked_construct;
    d.hooks.destruct = hooked_destruct;
    d.hooks.copy = with_copy ? hooked_copy : NULL;
    return de_type_register(r, &d);
}

/* ---- world.* -------------------------------------------------------------------------------- */

TEST_F(world, create_destroy_no_leak, ecs_fx) {
    /* The fixture's world: populate it, then destroy it; teardown checks the tracker again. */
    EXPECT_EQ(de_world_tick(f->w), 0u);
    const u32 sets[][3] = {{1, 0, 0}, {1, 2, 0}, {1, 2, 5}, {3, 4, 0}};
    const u32 set_sizes[] = {1, 2, 3, 2};
    for (u32 i = 0; i < 5000; ++i) {
        const de_entity e = de_entity_spawn(f->w);
        ASSERT_TRUE(de_entity_alive(f->w, e));
        ecs_test_set(f->w, e, sets[i % 4], set_sizes[i % 4]);
        if (i % 7 == 0)
            ecs_test_despawn(f->w, e);
    }
    const de_tracking_stats live = de_tracking_allocator_stats(&f->track);
    EXPECT_GT(live.bytes_live, 0u);
    ecs_fx_destroy_world(f); /* checks bytes_live == 0 and blocks_live == 0 */

    /* Bad arguments. */
    de_world *w = (de_world *)(void *)&w;
    de_world_desc d;
    memset(&d, 0, sizeof d);
    EXPECT_EQ(de_world_create(NULL, &w), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(w == NULL);
    EXPECT_EQ(de_world_create(&d, &w), DE_ERR_INVALID_ARGUMENT); /* no type registry */
    d.types = f->types;
    EXPECT_EQ(de_world_create(&d, NULL), DE_ERR_INVALID_ARGUMENT);
    d.chunk_bytes = ECS_MAX_CHUNK_BYTES + 1u;
    EXPECT_EQ(de_world_create(&d, &w), DE_ERR_INVALID_ARGUMENT);
    de_world_destroy(NULL);
    EXPECT_EQ(de_world_tick(NULL), 0u);

    /* A NULL allocator means the default heap; small chunk sizes are raised and rounded. */
    d.chunk_bytes = 100;
    d.initial_entity_capacity = 64;
    ASSERT_EQ(de_world_create(&d, &w), DE_OK);
    EXPECT_EQ(w->chunk_bytes, DE_ECS_MIN_CHUNK_BYTES);
    EXPECT_GE(w->records.capacity, 64u);
    de_world_destroy(w);
    d.chunk_bytes = 5000;
    ASSERT_EQ(de_world_create(&d, &w), DE_OK);
    EXPECT_EQ(w->chunk_bytes, 5056u); /* next multiple of 64 */
    de_world_destroy(w);
}

/* Every allocation of create, registration and spawning fails in turn: each failure is clean
 * and leaks nothing. */
TEST_F(world, create_oom_no_leak, ecs_fx) {
    ecs_fx_destroy_world(f);
    bool done = false;
    for (i64 n = 0; n < 1000 && !done; ++n) {
        de_tracking_allocator_fail_after(&f->track, n);
        de_world_desc d;
        memset(&d, 0, sizeof d);
        d.allocator = &f->alloc;
        d.types = f->types;
        d.initial_entity_capacity = 4;
        de_world *w = NULL;
        const de_result r = de_world_create(&d, &w);
        if (r != DE_OK) {
            EXPECT_EQ(r, DE_ERR_OUT_OF_MEMORY);
            EXPECT_TRUE(w == NULL);
        } else {
            const de_component c = de_component_register(w, f->t_pos, 0);
            bool ok = DE_HANDLE_VALID(c);
            for (u32 i = 0; ok && i < 300; ++i) {
                const de_entity e = de_entity_spawn(w);
                ok = de_entity_alive(w, e);
                if (ok) {
                    const u32 id = c.id;
                    ecs_archetype *a = NULL;
                    ok = ecs_archetype_get_or_create(w, &id, 1, &a) == DE_OK &&
                         ecs_entity_move(w, e, a) == DE_OK;
                    /* A failed move leaves the entity where it was. */
                    EXPECT_TRUE(de_entity_alive(w, e));
                }
            }
            done = ok;
            de_world_destroy(w);
        }
        const de_tracking_stats s = de_tracking_allocator_stats(&f->track);
        EXPECT_EQ(s.bytes_live, 0u);
        EXPECT_EQ(s.blocks_live, 0u);
    }
    de_tracking_allocator_fail_after(&f->track, -1);
    EXPECT_TRUE(done);
}

TEST_F(world, destroy_runs_destructors, ecs_fx) {
    const de_type_id t = register_hooked(f->types, true);
    const de_component h = de_component_register(f->w, t, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(h));
    g_construct_count = 0;
    g_destruct_count = 0;
    const u32 set[] = {f->pos.id, h.id};
    for (u32 i = 0; i < 1000; ++i)
        ecs_test_set(f->w, de_entity_spawn(f->w), set, 2);
    EXPECT_EQ(g_construct_count, 1000);
    EXPECT_EQ(g_destruct_count, 0);
    ecs_fx_destroy_world(f);
    EXPECT_EQ(g_destruct_count, 1000);
}

TEST_F(world, component_register_find, ecs_fx) {
    /* Ids are dense, in registration order (the fixture checked 1..5). */
    EXPECT_EQ(de_component_find(f->w, "TPos").id, f->pos.id);
    EXPECT_EQ(de_component_find(f->w, "TVel").id, f->vel.id);
    EXPECT_EQ(de_component_find(f->w, "THealth").id, f->health.id);
    EXPECT_EQ(de_component_find(f->w, "TEnemy").id, f->enemy.id);
    EXPECT_EQ(de_component_find(f->w, "TRot").id, f->rot.id);
    EXPECT_EQ(de_component_find(f->w, "TNope").id, 0u);
    EXPECT_EQ(de_component_find(f->w, NULL).id, 0u);
    EXPECT_EQ(de_component_find(NULL, "TPos").id, 0u);

    /* Size, alignment and flags come from reflection. */
    const ecs_component_info *pos = ecs_component_get(f->w, f->pos.id);
    ASSERT_TRUE(pos != NULL);
    EXPECT_EQ(pos->size, (u32)sizeof(TPos));
    EXPECT_EQ(pos->align, (u32)DE_ALIGNOF(TPos));
    EXPECT_EQ(pos->flags, (u32)DE_COMP_SNAPSHOT);
    EXPECT_STREQ(pos->name, "TPos");
    const ecs_component_info *rot = ecs_component_get(f->w, f->rot.id);
    ASSERT_TRUE(rot != NULL);
    EXPECT_EQ(rot->size, 16u);
    EXPECT_EQ(rot->align, 16u);
    EXPECT_TRUE(ecs_component_get(f->w, 0) == NULL);
    EXPECT_TRUE(ecs_component_get(f->w, 6) == NULL);

    /* Registering again: same flags -> same id; other flags -> DE_ERR_ALREADY_EXISTS. */
    EXPECT_EQ(de_component_register(f->w, f->t_pos, DE_COMP_SNAPSHOT).id, f->pos.id);
    de_error_clear();
    EXPECT_EQ(de_component_register(f->w, f->t_pos, 0).id, 0u);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_ALREADY_EXISTS);

    /* Bad arguments. */
    EXPECT_EQ(de_component_register(NULL, f->t_pos, 0).id, 0u);
    EXPECT_EQ(de_component_register(f->w, (de_type_id){0}, 0).id, 0u);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_component_register(f->w, (de_type_id){0xFFFFFu}, 0).id, 0u);
    EXPECT_EQ(de_component_register(f->w, f->t_vel, 1u << 9).id, 0u);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);

    /* Not POD and no copy hook: refused (SPEC §7.1). */
    const de_type_id no_copy = register_hooked(f->types, false);
    ASSERT_TRUE(DE_HANDLE_VALID(no_copy));
    EXPECT_EQ(de_component_register(f->w, no_copy, 0).id, 0u);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    /* ... but fine as a tag, which has no data. */
    const de_component no_copy_tag = de_component_register(f->w, no_copy, DE_COMP_TAG);
    EXPECT_EQ(no_copy_tag.id, 6u);

    /* Alignment above DE_ECS_MAX_COMPONENT_ALIGN, and a component no chunk can hold. */
    static const de_field_info one_byte[] = {{"b", 0, 1, DE_FK_U8, 0, {0, 0, 0, 0, 0, 0}}};
    de_type_desc wide;
    memset(&wide, 0, sizeof wide);
    wide.name = "TOverAligned";
    wide.size = 128;
    wide.align = 128;
    wide.version = 1;
    wide.fields = one_byte;
    wide.field_count = 1;
    wide.is_pod = true;
    const de_type_id over_aligned = de_type_register(f->types, &wide);
    ASSERT_TRUE(DE_HANDLE_VALID(over_aligned));
    EXPECT_EQ(de_component_register(f->w, over_aligned, 0).id, 0u);
    EXPECT_EQ(de_error_last()->result, DE_ERR_UNSUPPORTED);
    wide.name = "THuge";
    wide.size = DE_ECS_DEFAULT_CHUNK_BYTES;
    wide.align = 4;
    const de_type_id huge = de_type_register(f->types, &wide);
    ASSERT_TRUE(DE_HANDLE_VALID(huge));
    EXPECT_EQ(de_component_register(f->w, huge, 0).id, 0u);
    EXPECT_EQ(de_error_last()->result, DE_ERR_UNSUPPORTED);
    wide.name = "TBig";
    wide.size = DE_ECS_DEFAULT_CHUNK_BYTES - 64u;
    const de_type_id big = de_type_register(f->types, &wide);
    ASSERT_TRUE(DE_HANDLE_VALID(big));
    const de_component big_c = de_component_register(f->w, big, 0);
    EXPECT_EQ(big_c.id, 7u);
    /* It fits alone (one row per chunk), but not together with another large component. */
    ecs_archetype *a = NULL;
    ASSERT_EQ(ecs_archetype_get_or_create(f->w, &big_c.id, 1, &a), DE_OK);
    EXPECT_EQ(a->rows_per_chunk, 1u);
    wide.name = "TBig2";
    const de_type_id big2 = de_type_register(f->types, &wide);
    const de_component big2_c = de_component_register(f->w, big2, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(big2_c));
    const u32 both[] = {big_c.id, big2_c.id};
    const u32 archetypes_before = ecs_archetype_count(f->w);
    EXPECT_EQ(ecs_archetype_get_or_create(f->w, both, 2, &a), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(ecs_archetype_count(f->w), archetypes_before);

    /* Failed registrations took no id. */
    EXPECT_EQ(de_component_find(f->w, "TOverAligned").id, 0u);
    EXPECT_EQ(de_component_find(f->w, "TBig").id, big_c.id);
    EXPECT_EQ(f->w->components.count, 8u);
}

TEST_F(world, tag_zero_size, ecs_fx) {
    const ecs_component_info *tag = ecs_component_get(f->w, f->enemy.id);
    ASSERT_TRUE(tag != NULL);
    EXPECT_EQ(tag->size, 0u);
    EXPECT_EQ(tag->flags, (u32)DE_COMP_TAG);

    const de_entity e = de_entity_spawn(f->w);
    const de_entity plain = de_entity_spawn(f->w);
    const u32 set[] = {f->enemy.id, f->pos.id};
    ecs_test_set(f->w, e, set, 2);
    ecs_test_set(f->w, plain, set + 1, 1);

    /* The tag is in the set but has no column. */
    const ecs_archetype *a = ecs_test_archetype_of(f->w, e);
    EXPECT_EQ(a->component_count, 2u);
    EXPECT_EQ(a->column_count, 1u);
    EXPECT_EQ(a->columns[0].component, f->pos.id);
    EXPECT_EQ(ecs_archetype_column(a, f->enemy.id), ECS_INVALID_INDEX);
    EXPECT_NE(ecs_archetype_find_component(a, f->enemy.id), ECS_INVALID_INDEX);

    /* Tag-only archetype: same rows per chunk as the empty archetype (entity ids only). */
    const de_entity only_tag = de_entity_spawn(f->w);
    ecs_test_set(f->w, only_tag, set, 1);
    const ecs_archetype *ta = ecs_test_archetype_of(f->w, only_tag);
    EXPECT_EQ(ta->column_count, 0u);
    EXPECT_EQ(ta->rows_per_chunk, ecs_archetype_at(f->w, ECS_EMPTY_ARCHETYPE)->rows_per_chunk);
    EXPECT_EQ(ta->rows_per_chunk, f->w->chunk_bytes / (u32)sizeof(de_entity));

    /* has / get on tags. */
    EXPECT_TRUE(de_entity_has(f->w, e, f->enemy));
    EXPECT_TRUE(de_entity_has(f->w, only_tag, f->enemy));
    EXPECT_FALSE(de_entity_has(f->w, plain, f->enemy));
    EXPECT_TRUE(de_entity_get(f->w, e, f->enemy) != NULL);
    EXPECT_TRUE(de_entity_get_const(f->w, e, f->enemy) != NULL);
    EXPECT_TRUE(de_entity_get(f->w, plain, f->enemy) == NULL);

    /* Removing the tag keeps the data. */
    TPos *p = (TPos *)de_entity_get(f->w, e, f->pos);
    ASSERT_TRUE(p != NULL);
    p->v.x = 42.0f;
    ecs_test_set(f->w, e, set + 1, 1);
    EXPECT_FALSE(de_entity_has(f->w, e, f->enemy));
    const TPos *q = (const TPos *)de_entity_get_const(f->w, e, f->pos);
    ASSERT_TRUE(q != NULL);
    EXPECT_FLOAT_EQ(q->v.x, 42.0, 0.0);
}
