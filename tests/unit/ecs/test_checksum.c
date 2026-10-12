/* test_checksum.c: de_world_checksum (DENG-352, SPEC §5.7 rule 7, §19.4).
 *
 * The fixtures use fixed values only (integers and exact float products, no <math.h>), so the
 * golden values are the same on every platform. */
#include "ecs_test_util.h"

#include <stdio.h>

/* ---- Types ----------------------------------------------------------------------------------- */

/* Padding everywhere: after `a`, after `flag`, after `s`, inside the nested structs and at the
 * end. */
typedef struct TInner {
    u8 a;
    u32 b;
} TInner;
typedef struct TPadded {
    u8 flag;
    f64 d;
    u16 s;
    TInner inner;
    char name[12];
    TInner arr[2];
    f32 tail;
} TPadded;

/* A data component with flags other than DE_COMP_SNAPSHOT. */
typedef struct TNote {
    u32 value;
} TNote;

static de_type_id register_padded(de_type_registry *r) {
    de_type_id t = de_type_find(r, "TPadded");
    if (DE_HANDLE_VALID(t))
        return t;
    static const de_field_info k_inner[] = {
        DE_FIELD(TInner, a, DE_FK_U8, DE_FIELD_DEFAULT),
        DE_FIELD(TInner, b, DE_FK_U32, DE_FIELD_DEFAULT),
    };
    static const de_field_info k_padded[] = {
        DE_FIELD(TPadded, flag, DE_FK_U8, DE_FIELD_DEFAULT),
        DE_FIELD(TPadded, d, DE_FK_F64, DE_FIELD_DEFAULT),
        DE_FIELD(TPadded, s, DE_FK_U16, DE_FIELD_DEFAULT),
        DE_FIELD_EX(TPadded, inner, DE_FK_STRUCT, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TInner", 0}),
        DE_FIELD(TPadded, name, DE_FK_STRING, DE_FIELD_DEFAULT),
        DE_FIELD_EX(TPadded, arr, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TInner", 2}),
        DE_FIELD(TPadded, tail, DE_FK_F32, DE_FIELD_DEFAULT),
    };
    ASSERT_TRUE(DE_HANDLE_VALID(de_type_register(r, &DE_TYPE_DESC(TInner, 1, k_inner))));
    t = de_type_register(r, &DE_TYPE_DESC(TPadded, 1, k_padded));
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    return t;
}

static de_type_id register_note_type(de_type_registry *r) {
    de_type_id t = de_type_find(r, "TNote");
    if (DE_HANDLE_VALID(t))
        return t;
    static const de_field_info k_fields[] = {DE_FIELD(TNote, value, DE_FK_U32, DE_FIELD_DEFAULT)};
    t = de_type_register(r, &DE_TYPE_DESC(TNote, 1, k_fields));
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    return t;
}

/* A second world with the fixture's five components, registered in the same order. */
static de_world *make_world(ecs_fx *f, u32 chunk_bytes) {
    de_world_desc d;
    memset(&d, 0, sizeof d);
    d.types = f->types;
    d.chunk_bytes = chunk_bytes;
    de_world *w = NULL;
    ASSERT_EQ(de_world_create(&d, &w), DE_OK);
    ASSERT_EQ(de_component_register(w, f->t_pos, DE_COMP_SNAPSHOT).id, 1u);
    ASSERT_EQ(de_component_register(w, f->t_vel, DE_COMP_SNAPSHOT).id, 2u);
    ASSERT_EQ(de_component_register(w, f->t_health, DE_COMP_SNAPSHOT).id, 3u);
    ASSERT_EQ(de_component_register(w, f->t_enemy, DE_COMP_TAG).id, 4u);
    ASSERT_EQ(de_component_register(w, f->t_rot, DE_COMP_SNAPSHOT).id, 5u);
    return w;
}

/* The fixed component values of entity number i. */
static void write_values(de_world *w, de_entity e, u32 i) {
    const de_component pos = {1}, vel = {2}, health = {3}, rot = {5};
    TPos *p = (TPos *)de_entity_get(w, e, pos);
    if (p != NULL) {
        p->v.x = (f32)i * 0.5f;
        p->v.y = -(f32)i;
        p->v.z = (f32)(i % 17u) * 0.25f;
    }
    TVel *v = (TVel *)de_entity_get(w, e, vel);
    if (v != NULL) {
        v->v.x = 1.0f;
        v->v.y = (f32)i * 0.125f;
        v->v.z = 0.0f;
    }
    THealth *h = (THealth *)de_entity_get(w, e, health);
    if (h != NULL) {
        h->current = (f32)(i % 100u);
        h->max = 100.0f;
    }
    TRot *r = (TRot *)de_entity_get(w, e, rot);
    if (r != NULL) {
        r->q.x = 0.0f;
        r->q.y = (f32)(i % 3u) * 0.5f;
        r->q.z = 0.0f;
        r->q.w = 1.0f;
    }
}

/* Component set of entity number i (ids, ascending): 0 {pos, vel}, 1 {pos}, 2 {pos, vel,
 * health}, 3 {pos, rot}, 4 {pos, enemy}. */
static u32 set_of(u32 i, u32 *ids) {
    switch (i % 5u) {
    case 0:
        ids[0] = 1;
        ids[1] = 2;
        return 2;
    case 1:
        ids[0] = 1;
        return 1;
    case 2:
        ids[0] = 1;
        ids[1] = 2;
        ids[2] = 3;
        return 3;
    case 3:
        ids[0] = 1;
        ids[1] = 5;
        return 2;
    default:
        ids[0] = 1;
        ids[1] = 4;
        return 2;
    }
}

static const u32 k_masks[] = {DE_COMP_SNAPSHOT, 0xFFFFFFFFu, 0u, DE_COMP_TAG};

/* ---- Tests ----------------------------------------------------------------------------------- */

TEST_F(checksum, layout_independent, ecs_fx) {
    enum { N = 300 };
    static de_entity a_ids[N], b_ids[N];

    /* World A: spawn_with, in entity order, default chunks. */
    for (u32 i = 0; i < N; ++i) {
        u32 ids[4];
        const u32 n = set_of(i, ids);
        de_component comps[4];
        for (u32 k = 0; k < n; ++k)
            comps[k].id = ids[k];
        ASSERT_EQ(de_entity_spawn_with(f->w, comps, n, &a_ids[i]), DE_OK);
        write_values(f->w, a_ids[i], i);
    }

    /* World B: 1 KiB chunks, archetypes created up front in another order (plus unused ones),
     * plain spawns, then components added in reverse entity order and reverse component order
     * (through intermediate archetypes), so rows sit in other archetypes, chunks and rows. */
    de_world *b = make_world(f, 1024);
    static const u32 k_sets[][3] = {{1, 4, 0}, {1, 5, 0}, {2, 3, 0},
                                    {1, 2, 3}, {1, 0, 0}, {1, 2, 0}};
    static const u32 k_set_n[] = {2, 2, 2, 3, 1, 2};
    for (u32 k = 0; k < 6; ++k) {
        ecs_archetype *ign = NULL;
        ASSERT_EQ(ecs_archetype_get_or_create(b, k_sets[k], k_set_n[k], &ign), DE_OK);
    }
    for (u32 i = 0; i < N; ++i)
        b_ids[i] = de_entity_spawn(b);
    for (u32 j = N; j-- > 0;) {
        u32 ids[4];
        const u32 n = set_of(j, ids);
        for (u32 k = n; k-- > 0;)
            ASSERT_TRUE(de_entity_add(b, b_ids[j], (de_component){ids[k]}) != NULL);
        write_values(b, b_ids[j], j);
    }
    /* The same entities despawned, in another order (the free lists differ; the checksum does
     * not cover them). */
    for (u32 i = 0; i < N; i += 7u)
        de_entity_despawn(f->w, a_ids[i]);
    for (u32 i = (N - 1u) / 7u * 7u;; i -= 7u) {
        de_entity_despawn(b, b_ids[i]);
        if (i == 0)
            break;
    }

    /* The layouts really differ. */
    ASSERT_EQ(a_ids[1].bits, b_ids[1].bits);
    EXPECT_NE(ecs_entity_record(f->w, a_ids[1])->archetype,
              ecs_entity_record(b, b_ids[1])->archetype);
    EXPECT_NE(ecs_entity_record(f->w, a_ids[2])->row, ecs_entity_record(b, b_ids[2])->row);

    for (u32 m = 0; m < sizeof k_masks / sizeof k_masks[0]; ++m)
        EXPECT_EQ(de_world_checksum(f->w, k_masks[m]), de_world_checksum(b, k_masks[m]));

    /* Any difference shows: one more component, one other value, one other entity. */
    const u64 ck = de_world_checksum(f->w, DE_COMP_SNAPSHOT);
    ASSERT_TRUE(de_entity_add(b, b_ids[1], f->health) != NULL);
    EXPECT_NE(de_world_checksum(b, DE_COMP_SNAPSHOT), ck);
    de_entity_remove(b, b_ids[1], f->health);
    EXPECT_EQ(de_world_checksum(b, DE_COMP_SNAPSHOT), ck);
    de_entity_despawn(b, b_ids[2]);
    EXPECT_NE(de_world_checksum(b, DE_COMP_SNAPSHOT), ck);
    EXPECT_NE(de_world_checksum(b, 0), de_world_checksum(f->w, 0));
    de_world_destroy(b);
}

TEST_F(checksum, ulp_sensitive, ecs_fx) {
    de_entity e[10];
    for (u32 i = 0; i < 10; ++i) {
        u32 ids[4];
        const u32 n = set_of(i, ids);
        de_component comps[4];
        for (u32 k = 0; k < n; ++k)
            comps[k].id = ids[k];
        ASSERT_EQ(de_entity_spawn_with(f->w, comps, n, &e[i]), DE_OK);
        write_values(f->w, e[i], i);
    }
    const u64 ck = de_world_checksum(f->w, DE_COMP_SNAPSHOT);

    /* One ulp up and down in a position, a velocity and a quaternion. */
    f32 *fields[3] = {&((TPos *)de_entity_get(f->w, e[7], f->pos))->v.x,
                      &((TVel *)de_entity_get(f->w, e[5], f->vel))->v.y,
                      &((TRot *)de_entity_get(f->w, e[3], f->rot))->q.w};
    for (u32 k = 0; k < 3; ++k) {
        u32 bits;
        memcpy(&bits, fields[k], sizeof bits);
        const u32 orig = bits;
        for (int d = -1; d <= 1; d += 2) {
            bits = orig + (u32)d;
            memcpy(fields[k], &bits, sizeof bits);
            EXPECT_NE(de_world_checksum(f->w, DE_COMP_SNAPSHOT), ck);
        }
        memcpy(fields[k], &orig, sizeof orig);
        EXPECT_EQ(de_world_checksum(f->w, DE_COMP_SNAPSHOT), ck);
    }

    /* +0.0 and -0.0 compare equal but are different bit patterns. */
    TVel *v = (TVel *)de_entity_get(f->w, e[0], f->vel);
    ASSERT_TRUE(v->v.z == 0.0f);
    v->v.z = -0.0f;
    EXPECT_NE(de_world_checksum(f->w, DE_COMP_SNAPSHOT), ck);
    v->v.z = 0.0f;
    EXPECT_EQ(de_world_checksum(f->w, DE_COMP_SNAPSHOT), ck);
}

/* Fills `p` with `garbage` (every padding byte), then writes the same field values. */
static void fill_padded(TPadded *p, u8 garbage, u32 seed) {
    memset(p, garbage, sizeof *p);
    p->flag = (u8)(seed & 1u);
    p->d = (f64)seed * 0.5;
    p->s = (u16)(seed * 3u);
    p->inner.a = (u8)seed;
    p->inner.b = seed * 1000u;
    memset(p->name, garbage, sizeof p->name);
    p->name[0] = 'n';
    p->name[1] = (char)('a' + (char)(seed % 26u));
    p->name[2] = '\0'; /* the bytes after the terminator stay garbage */
    p->arr[0].a = 1;
    p->arr[0].b = seed;
    p->arr[1].a = 2;
    p->arr[1].b = seed + 1u;
    p->tail = (f32)seed * 0.25f;
}

TEST_F(checksum, padding_ignored, ecs_fx) {
    ASSERT_GT(sizeof(TPadded), (size_t)(1 + 8 + 2 + 8 + 12 + 16 + 4)); /* fields: 51 bytes */
    const de_type_id tp = register_padded(f->types);
    const de_component padded = de_component_register(f->w, tp, DE_COMP_SNAPSHOT);
    ASSERT_TRUE(DE_HANDLE_VALID(padded));
    de_world *b = make_world(f, 0);
    ASSERT_EQ(de_component_register(b, tp, DE_COMP_SNAPSHOT).id, padded.id);

    de_entity ea[20], eb[20];
    for (u32 i = 0; i < 20; ++i) {
        const de_component c[2] = {f->pos, padded};
        ASSERT_EQ(de_entity_spawn_with(f->w, c, 2, &ea[i]), DE_OK);
        ASSERT_EQ(de_entity_spawn_with(b, c, 2, &eb[i]), DE_OK);
        write_values(f->w, ea[i], i);
        write_values(b, eb[i], i);
        fill_padded((TPadded *)de_entity_get(f->w, ea[i], padded), 0x00, i);
        fill_padded((TPadded *)de_entity_get(b, eb[i], padded), (u8)(0xA5u + i), i);
    }
    ASSERT_TRUE(memcmp(de_entity_get(f->w, ea[3], padded), de_entity_get(b, eb[3], padded),
                       sizeof(TPadded)) != 0);
    const u64 ck = de_world_checksum(f->w, DE_COMP_SNAPSHOT);
    EXPECT_EQ(de_world_checksum(b, DE_COMP_SNAPSHOT), ck);

    /* Scribbling over padding again changes nothing; a field byte does. */
    TPadded *p = (TPadded *)de_entity_get(b, eb[4], padded);
    u8 *raw = (u8 *)p;
    raw[1] ^= 0xFFu;                            /* after flag */
    raw[offsetof(TPadded, inner) + 1] ^= 0xFFu; /* after inner.a */
    raw[offsetof(TPadded, name) + 7] ^= 0xFFu;  /* after the terminator */
    raw[sizeof(TPadded) - 1] ^= 0xFFu;          /* tail padding */
    EXPECT_EQ(de_world_checksum(b, DE_COMP_SNAPSHOT), ck);
    p->inner.b += 1u;
    EXPECT_NE(de_world_checksum(b, DE_COMP_SNAPSHOT), ck);
    p->inner.b -= 1u;
    p->arr[1].a = 9;
    EXPECT_NE(de_world_checksum(b, DE_COMP_SNAPSHOT), ck);
    p->arr[1].a = 2;
    p->name[2] = 'x'; /* the string gets longer */
    EXPECT_NE(de_world_checksum(b, DE_COMP_SNAPSHOT), ck);
    p->name[2] = '\0';
    EXPECT_EQ(de_world_checksum(b, DE_COMP_SNAPSHOT), ck);
    de_world_destroy(b);
}

TEST_F(checksum, mask_filters, ecs_fx) {
    const de_component note =
        de_component_register(f->w, register_note_type(f->types), DE_COMP_REPLICATE);
    ASSERT_TRUE(DE_HANDLE_VALID(note));
    de_entity e[8];
    for (u32 i = 0; i < 8; ++i) {
        const de_component c[2] = {f->pos, note};
        ASSERT_EQ(de_entity_spawn_with(f->w, c, 2, &e[i]), DE_OK);
        write_values(f->w, e[i], i);
        ((TNote *)de_entity_get(f->w, e[i], note))->value = i;
    }
    const u64 snap0 = de_world_checksum(f->w, DE_COMP_SNAPSHOT);
    const u64 rep0 = de_world_checksum(f->w, DE_COMP_REPLICATE);
    const u64 both0 = de_world_checksum(f->w, DE_COMP_SNAPSHOT | DE_COMP_REPLICATE);
    const u64 tag0 = de_world_checksum(f->w, DE_COMP_TAG);
    const u64 ent0 = de_world_checksum(f->w, 0);
    EXPECT_NE(snap0, rep0);
    EXPECT_NE(snap0, both0);
    EXPECT_NE(snap0, ent0);

    /* A replicated-only change. */
    ((TNote *)de_entity_get(f->w, e[2], note))->value = 99u;
    EXPECT_EQ(de_world_checksum(f->w, DE_COMP_SNAPSHOT), snap0);
    EXPECT_NE(de_world_checksum(f->w, DE_COMP_REPLICATE), rep0);
    EXPECT_NE(de_world_checksum(f->w, DE_COMP_SNAPSHOT | DE_COMP_REPLICATE), both0);
    EXPECT_EQ(de_world_checksum(f->w, 0), ent0);
    ((TNote *)de_entity_get(f->w, e[2], note))->value = 2u;

    /* A snapshot-only change. */
    ((TPos *)de_entity_get(f->w, e[6], f->pos))->v.y = 123.0f;
    EXPECT_NE(de_world_checksum(f->w, DE_COMP_SNAPSHOT), snap0);
    EXPECT_EQ(de_world_checksum(f->w, DE_COMP_REPLICATE), rep0);
    EXPECT_EQ(de_world_checksum(f->w, 0), ent0);
    write_values(f->w, e[6], 6);

    /* A tag (selected by DE_COMP_TAG, by id only). */
    ASSERT_TRUE(de_entity_add(f->w, e[1], f->enemy) != NULL);
    EXPECT_NE(de_world_checksum(f->w, DE_COMP_TAG), tag0);
    EXPECT_EQ(de_world_checksum(f->w, DE_COMP_SNAPSHOT), snap0);
    EXPECT_EQ(de_world_checksum(f->w, 0), ent0);
    de_entity_remove(f->w, e[1], f->enemy);
    EXPECT_EQ(de_world_checksum(f->w, DE_COMP_TAG), tag0);

    /* Mask 0 covers the entity set only. */
    const de_entity extra = de_entity_spawn(f->w);
    EXPECT_NE(de_world_checksum(f->w, 0), ent0);
    de_entity_despawn(f->w, extra); /* the alive set is the old one again */
    EXPECT_EQ(de_world_checksum(f->w, 0), ent0);
    EXPECT_EQ(de_world_checksum(f->w, DE_COMP_SNAPSHOT), snap0);
    EXPECT_EQ(de_world_checksum(NULL, DE_COMP_SNAPSHOT), 0u);
}

TEST_F(checksum, uncached_same_value, ecs_fx) {
    /* The first checksum of a world builds the field-span cache; when that allocation fails it
     * walks the fields per row and must give the same value. */
    const de_type_id tp = register_padded(f->types);
    const de_component padded = de_component_register(f->w, tp, DE_COMP_SNAPSHOT);
    de_entity e[30];
    for (u32 i = 0; i < 30; ++i) {
        const de_component c[3] = {f->pos, f->rot, padded};
        ASSERT_EQ(de_entity_spawn_with(f->w, c, 3, &e[i]), DE_OK);
        write_values(f->w, e[i], i);
        fill_padded((TPadded *)de_entity_get(f->w, e[i], padded), (u8)i, i);
    }
    ASSERT_TRUE(f->w->ck == NULL);
    de_tracking_allocator_fail_after(&f->track, 0);
    const u64 uncached = de_world_checksum(f->w, 0xFFFFFFFFu);
    de_tracking_allocator_fail_after(&f->track, -1);
    ASSERT_TRUE(f->w->ck == NULL);
    const u64 cached = de_world_checksum(f->w, 0xFFFFFFFFu);
    ASSERT_TRUE(f->w->ck != NULL);
    EXPECT_EQ(uncached, cached);

    /* The cache allocates once; later checksums allocate nothing. */
    const de_tracking_stats s0 = de_tracking_allocator_stats(&f->track);
    for (u32 k = 0; k < 10; ++k)
        EXPECT_EQ(de_world_checksum(f->w, 0xFFFFFFFFu), cached);
    const de_tracking_stats s1 = de_tracking_allocator_stats(&f->track);
    EXPECT_EQ(s1.alloc_count, s0.alloc_count);
    EXPECT_EQ(s1.realloc_count, s0.realloc_count);
}

/* The golden scenario: a cross-platform contract (SPEC §19.4). Integer-derived values only. */
TEST_F(checksum, golden, ecs_fx) {
    enum { N = 64 };
    de_entity e[N];
    for (u32 i = 0; i < N; ++i) {
        u32 ids[4];
        const u32 n = set_of(i, ids);
        de_component comps[4];
        for (u32 k = 0; k < n; ++k)
            comps[k].id = ids[k];
        ASSERT_EQ(de_entity_spawn_with(f->w, comps, n, &e[i]), DE_OK);
        write_values(f->w, e[i], i);
    }
    for (u32 i = 0; i < N; i += 9u)
        de_entity_despawn(f->w, e[i]);
    for (u32 i = 0; i < 4; ++i) {
        const de_entity x = de_entity_spawn(f->w);
        ASSERT_TRUE(de_entity_add(f->w, x, f->health) != NULL);
        write_values(f->w, x, 1000u + i);
    }
    const u64 snap = de_world_checksum(f->w, DE_COMP_SNAPSHOT);
    const u64 all = de_world_checksum(f->w, 0xFFFFFFFFu);
    const u64 ent = de_world_checksum(f->w, 0);
    printf("checksum.golden: snapshot 0x%016llX, all 0x%016llX, entities 0x%016llX\n",
           (unsigned long long)snap, (unsigned long long)all, (unsigned long long)ent);
    EXPECT_EQ(snap, 0x2690ED42827FD378ull);
    EXPECT_EQ(all, 0x6DFF17D34C11988Bull);
    EXPECT_EQ(ent, 0xE6758481290A1873ull);
}
