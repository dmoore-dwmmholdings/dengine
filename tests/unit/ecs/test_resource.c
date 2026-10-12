/* test_resource.c: typed singleton resources (DENG-348). */
#include "ecs_test_util.h"

#include <stdint.h>
#include <stdio.h>

/* ---- Resource types ---------------------------------------------------------------------------
 */

typedef struct RTime {
    f64 elapsed;
    u32 frame;
    u32 pad_;
} RTime;

/* 16-byte aligned, to check the value block's alignment. */
typedef struct RAligned {
    de_vec4 v;
} RAligned;

typedef struct RCounter {
    u32 value;
} RCounter;

/* Not POD: hooks count their calls. */
typedef struct RHooked {
    u32 value;
} RHooked;

static int g_construct;
static int g_destruct;
static int g_copy;

static void rhooked_construct(void *obj) {
    ((RHooked *)obj)->value = 0xC0FFEEu;
    ++g_construct;
}
static void rhooked_destruct(void *obj) {
    ((RHooked *)obj)->value = 0xDEADu;
    ++g_destruct;
}
static void rhooked_copy(void *dst, const void *src) {
    ((RHooked *)dst)->value = ((const RHooked *)src)->value;
    ++g_copy;
}

typedef struct res_types {
    de_type_id time, aligned, counter, hooked, hooked_nocopy;
} res_types;

static res_types register_res_types(de_type_registry *r) {
    static const de_field_info k_time[] = {
        DE_FIELD(RTime, elapsed, DE_FK_F64, DE_FIELD_DEFAULT),
        DE_FIELD(RTime, frame, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(RTime, pad_, DE_FK_U32, DE_FIELD_DEFAULT),
    };
    static const de_field_info k_aligned[] = {DE_FIELD(RAligned, v, DE_FK_VEC4, DE_FIELD_DEFAULT)};
    static const de_field_info k_counter[] = {
        DE_FIELD(RCounter, value, DE_FK_U32, DE_FIELD_DEFAULT)};
    static const de_field_info k_hooked[] = {DE_FIELD(RHooked, value, DE_FK_U32, DE_FIELD_DEFAULT)};
    res_types t;
    t.time = de_type_register(r, &DE_TYPE_DESC(RTime, 1, k_time));
    t.aligned = de_type_register(r, &DE_TYPE_DESC(RAligned, 1, k_aligned));
    t.counter = de_type_register(r, &DE_TYPE_DESC(RCounter, 1, k_counter));
    de_type_desc d = DE_TYPE_DESC(RHooked, 1, k_hooked);
    d.is_pod = false;
    d.hooks.construct = rhooked_construct;
    d.hooks.destruct = rhooked_destruct;
    d.hooks.copy = rhooked_copy;
    t.hooked = de_type_register(r, &d);
    /* Same layout under another name, without a copy hook: cannot be copied from `init`. */
    d.name = "RHookedNoCopy";
    d.hooks.copy = NULL;
    t.hooked_nocopy = de_type_register(r, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(t.time) && DE_HANDLE_VALID(t.aligned) &&
                DE_HANDLE_VALID(t.counter) && DE_HANDLE_VALID(t.hooked) &&
                DE_HANDLE_VALID(t.hooked_nocopy));
    g_construct = g_destruct = g_copy = 0;
    return t;
}

/* ---- resource.* --------------------------------------------------------------------------------
 */

TEST_F(resource, insert_get, ecs_fx) {
    const res_types t = register_res_types(f->types);

    /* Copy of `init`; ids are dense in first-insert order. */
    const RTime init = {1.5, 7u, 0u};
    const de_resource_id time = de_resource_insert(f->w, t.time, &init);
    ASSERT_TRUE(DE_HANDLE_VALID(time));
    EXPECT_EQ(time.id, 1u);
    RTime *tp = (RTime *)de_resource_get(f->w, time);
    ASSERT_TRUE(tp != NULL);
    EXPECT_TRUE(tp != &init);
    EXPECT_FLOAT_EQ(tp->elapsed, 1.5, 0.0);
    EXPECT_EQ(tp->frame, 7u);

    /* NULL `init`: zero for a POD type without a construct hook. */
    const de_resource_id aligned = de_resource_insert(f->w, t.aligned, NULL);
    ASSERT_TRUE(DE_HANDLE_VALID(aligned));
    EXPECT_EQ(aligned.id, 2u);
    RAligned *ap = (RAligned *)de_resource_get(f->w, aligned);
    ASSERT_TRUE(ap != NULL);
    EXPECT_EQ((uintptr_t)ap % 16u, 0u);
    EXPECT_FLOAT_EQ(ap->v.x, 0.0, 0.0);
    EXPECT_FLOAT_EQ(ap->v.w, 0.0, 0.0);

    /* NULL `init` with a construct hook. */
    const de_resource_id hooked = de_resource_insert(f->w, t.hooked, NULL);
    ASSERT_TRUE(DE_HANDLE_VALID(hooked));
    EXPECT_EQ(hooked.id, 3u);
    EXPECT_EQ(((RHooked *)de_resource_get(f->w, hooked))->value, 0xC0FFEEu);
    EXPECT_EQ(g_construct, 1);

    /* The values are writable and pointers stay valid as other resources come and go. */
    tp->frame = 99u;
    const de_resource_id counter = de_resource_insert(f->w, t.counter, NULL);
    ASSERT_TRUE(DE_HANDLE_VALID(counter));
    de_resource_remove(f->w, counter);
    EXPECT_TRUE(de_resource_get(f->w, time) == tp);
    EXPECT_EQ(((RTime *)de_resource_get(f->w, time))->frame, 99u);

    /* Refusals: no world, a type the registry does not know, an uncopyable `init`. */
    EXPECT_FALSE(DE_HANDLE_VALID(de_resource_insert(NULL, t.time, &init)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    const de_type_id bogus = {0x7FFFFFF0u};
    EXPECT_FALSE(DE_HANDLE_VALID(de_resource_insert(f->w, bogus, NULL)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    const RHooked h = {5u};
    EXPECT_FALSE(DE_HANDLE_VALID(de_resource_insert(f->w, t.hooked_nocopy, &h)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    const de_resource_id nocopy = de_resource_insert(f->w, t.hooked_nocopy, NULL);
    EXPECT_TRUE(DE_HANDLE_VALID(nocopy)); /* constructing needs no copy hook */
    EXPECT_FALSE(DE_HANDLE_VALID(de_resource_insert(f->w, t.hooked_nocopy, &h)));
    EXPECT_EQ(((RHooked *)de_resource_get(f->w, nocopy))->value, 0xC0FFEEu); /* unchanged */

    /* Bad ids. */
    EXPECT_TRUE(de_resource_get(f->w, (de_resource_id){0}) == NULL);
    EXPECT_TRUE(de_resource_get(f->w, (de_resource_id){1000u}) == NULL);
    EXPECT_TRUE(de_resource_get(NULL, time) == NULL);

    /* Out of memory: each allocation of a first insert fails in turn and leaves no trace. */
    for (i64 n = 0; n < 8; ++n) {
        ecs_fx_destroy_world(f);
        ecs_fx_create_world(f, NULL);
        de_tracking_allocator_fail_after(&f->track, n);
        const de_resource_id id = de_resource_insert(f->w, t.time, &init);
        de_tracking_allocator_fail_after(&f->track, -1);
        if (!DE_HANDLE_VALID(id)) {
            EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
            EXPECT_FALSE(DE_HANDLE_VALID(de_resource_find(f->w, t.time)));
            EXPECT_EQ(f->w->resources.count, 0u);
        } else {
            EXPECT_EQ(((RTime *)de_resource_get(f->w, id))->frame, 7u);
        }
        /* Inserting works again once memory is back. */
        const de_resource_id again = de_resource_insert(f->w, t.time, &init);
        EXPECT_EQ(again.id, 1u);
    }
}

TEST_F(resource, replace_destructs, ecs_fx) {
    const res_types t = register_res_types(f->types);

    const RHooked a = {1u};
    const de_resource_id id = de_resource_insert(f->w, t.hooked, &a);
    ASSERT_TRUE(DE_HANDLE_VALID(id));
    RHooked *p = (RHooked *)de_resource_get(f->w, id);
    ASSERT_TRUE(p != NULL);
    EXPECT_EQ(p->value, 1u);
    EXPECT_EQ(g_copy, 1);
    EXPECT_EQ(g_destruct, 0);

    /* Replace with a copy: the old value is destructed, the id and the block stay. */
    const RHooked b = {2u};
    const de_resource_id id2 = de_resource_insert(f->w, t.hooked, &b);
    EXPECT_EQ(id2.id, id.id);
    EXPECT_TRUE(de_resource_get(f->w, id) == p);
    EXPECT_EQ(p->value, 2u);
    EXPECT_EQ(g_destruct, 1);
    EXPECT_EQ(g_copy, 2);

    /* Replace with a constructed value. */
    const de_resource_id id3 = de_resource_insert(f->w, t.hooked, NULL);
    EXPECT_EQ(id3.id, id.id);
    EXPECT_EQ(p->value, 0xC0FFEEu);
    EXPECT_EQ(g_destruct, 2);
    EXPECT_EQ(g_construct, 1);

    /* Re-inserting the current value itself changes nothing. */
    const de_resource_id id4 = de_resource_insert(f->w, t.hooked, p);
    EXPECT_EQ(id4.id, id.id);
    EXPECT_EQ(p->value, 0xC0FFEEu);
    EXPECT_EQ(g_destruct, 2);
    EXPECT_EQ(g_copy, 2);

    /* POD replace. */
    const RCounter c1 = {10u}, c2 = {20u};
    const de_resource_id cid = de_resource_insert(f->w, t.counter, &c1);
    EXPECT_EQ(de_resource_insert(f->w, t.counter, &c2).id, cid.id);
    EXPECT_EQ(((RCounter *)de_resource_get(f->w, cid))->value, 20u);

    /* The world destructs what is left when it is destroyed. */
    ecs_fx_destroy_world(f);
    EXPECT_EQ(g_destruct, 3);
    EXPECT_EQ(g_construct, 1);
}

TEST_F(resource, find_by_type, ecs_fx) {
    const res_types t = register_res_types(f->types);

    EXPECT_FALSE(DE_HANDLE_VALID(de_resource_find(f->w, t.time)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_resource_find(NULL, t.time)));

    const de_resource_id counter = de_resource_insert(f->w, t.counter, NULL);
    const de_resource_id time = de_resource_insert(f->w, t.time, NULL);
    ASSERT_TRUE(DE_HANDLE_VALID(counter) && DE_HANDLE_VALID(time));
    EXPECT_NE(counter.id, time.id);
    EXPECT_EQ(de_resource_find(f->w, t.counter).id, counter.id);
    EXPECT_EQ(de_resource_find(f->w, t.time).id, time.id);
    EXPECT_FALSE(DE_HANDLE_VALID(de_resource_find(f->w, t.aligned)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_resource_find(f->w, (de_type_id){0})));
    EXPECT_FALSE(DE_HANDLE_VALID(de_resource_find(f->w, (de_type_id){0x7FFFFFF0u})));

    /* A component type can be a resource too: the two registries are separate. */
    const de_resource_id pos = de_resource_insert(f->w, f->t_pos, NULL);
    EXPECT_EQ(de_resource_find(f->w, f->t_pos).id, pos.id);
    EXPECT_EQ(pos.id, 3u);

    /* Removed: not found; inserted again: found under the same id. */
    de_resource_remove(f->w, time);
    EXPECT_FALSE(DE_HANDLE_VALID(de_resource_find(f->w, t.time)));
    EXPECT_EQ(de_resource_find(f->w, t.counter).id, counter.id);
    EXPECT_EQ(de_resource_insert(f->w, t.time, NULL).id, time.id);
    EXPECT_EQ(de_resource_find(f->w, t.time).id, time.id);

    /* Many types: each one found, ids in first-insert order. */
    static const de_field_info k_counter[] = {
        DE_FIELD(RCounter, value, DE_FK_U32, DE_FIELD_DEFAULT)};
    de_type_id types[40];
    de_resource_id ids[40];
    for (u32 i = 0; i < 40; ++i) {
        char name[32];
        snprintf(name, sizeof name, "RMany%u", i);
        de_type_desc d = DE_TYPE_DESC(RCounter, 1, k_counter);
        d.name = name; /* the registry deep-copies the description */
        types[i] = de_type_register(f->types, &d);
        ASSERT_TRUE(DE_HANDLE_VALID(types[i]));
        const RCounter v = {i};
        ids[i] = de_resource_insert(f->w, types[i], &v);
        EXPECT_EQ(ids[i].id, 4u + i);
    }
    for (u32 i = 0; i < 40; ++i) {
        EXPECT_EQ(de_resource_find(f->w, types[i]).id, ids[i].id);
        EXPECT_EQ(((const RCounter *)de_resource_get(f->w, ids[i]))->value, i);
    }
}

TEST_F(resource, remove, ecs_fx) {
    const res_types t = register_res_types(f->types);

    const RHooked a = {3u};
    const de_resource_id id = de_resource_insert(f->w, t.hooked, &a);
    ASSERT_TRUE(de_resource_get(f->w, id) != NULL);

    de_resource_remove(f->w, id);
    EXPECT_TRUE(de_resource_get(f->w, id) == NULL);
    EXPECT_EQ(g_destruct, 1);

    /* Removing again, or removing bad ids, does nothing. */
    de_resource_remove(f->w, id);
    de_resource_remove(f->w, (de_resource_id){0});
    de_resource_remove(f->w, (de_resource_id){77u});
    de_resource_remove(NULL, id);
    EXPECT_EQ(g_destruct, 1);

    /* Inserting again gives the same id and a new value. */
    const RHooked b = {4u};
    const de_resource_id again = de_resource_insert(f->w, t.hooked, &b);
    EXPECT_EQ(again.id, id.id);
    ASSERT_TRUE(de_resource_get(f->w, id) != NULL);
    EXPECT_EQ(((RHooked *)de_resource_get(f->w, id))->value, 4u);
    EXPECT_EQ(g_destruct, 1);

    /* The memory of a removed value is returned. */
    const de_tracking_stats before = de_tracking_allocator_stats(&f->track);
    de_resource_remove(f->w, again);
    const de_tracking_stats after = de_tracking_allocator_stats(&f->track);
    EXPECT_EQ(after.blocks_live + 1u, before.blocks_live);
    EXPECT_EQ(g_destruct, 2);
    /* Teardown checks that the world gives back everything else. */
}
