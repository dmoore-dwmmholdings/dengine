/* test_event.c: double-buffered typed events and readers (DENG-348). */
#include "ecs_test_util.h"

/* ---- Event types ------------------------------------------------------------------------------
 */

typedef struct EVal {
    u32 seq;
    u32 tag;
} EVal;

/* Not POD: copy and destruct hooks count their calls. */
typedef struct EHooked {
    u32 value;
} EHooked;

static int g_copy;
static int g_destruct;

static void ehooked_copy(void *dst, const void *src) {
    ((EHooked *)dst)->value = ((const EHooked *)src)->value;
    ++g_copy;
}
static void ehooked_destruct(void *obj) {
    ((EHooked *)obj)->value = 0xDEADu;
    ++g_destruct;
}

typedef struct ev_types {
    de_type_id val, hooked, no_destruct;
} ev_types;

static ev_types register_ev_types(de_type_registry *r) {
    static const de_field_info k_val[] = {
        DE_FIELD(EVal, seq, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(EVal, tag, DE_FK_U32, DE_FIELD_DEFAULT),
    };
    static const de_field_info k_hooked[] = {DE_FIELD(EHooked, value, DE_FK_U32, DE_FIELD_DEFAULT)};
    ev_types t;
    t.val = de_type_register(r, &DE_TYPE_DESC(EVal, 1, k_val));
    de_type_desc d = DE_TYPE_DESC(EHooked, 1, k_hooked);
    d.is_pod = false;
    d.hooks.copy = ehooked_copy;
    d.hooks.destruct = ehooked_destruct;
    t.hooked = de_type_register(r, &d);
    d.name = "EHookedNoDestruct";
    d.hooks.destruct = NULL;
    t.no_destruct = de_type_register(r, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(t.val) && DE_HANDLE_VALID(t.hooked) &&
                DE_HANDLE_VALID(t.no_destruct));
    g_copy = g_destruct = 0;
    return t;
}

static void send_val(de_world *w, de_event_type t, u32 seq, u32 tag) {
    const EVal v = {seq, tag};
    de_event_send(w, t, &v);
}

/* Reads every event `r` has left; returns how many, and checks they are `first`, first + 1, ... */
static u32 drain_expect(de_world *w, de_event_reader *r, u32 first) {
    u32 n = 0;
    const EVal *e;
    while ((e = (const EVal *)de_event_next(w, r)) != NULL) {
        EXPECT_EQ(e->seq, first + n);
        ++n;
    }
    return n;
}

/* An allocator that always moves a block on realloc and fills freed memory with 0xA5, so a read
 * through a pointer into a block that moved sees garbage (without needing ASan). */
static void *poison_alloc(void *user, size_t size, size_t align, u32 tag) {
    return DE_ALLOC((const de_allocator *)user, size, align, tag);
}
static void poison_free(void *user, void *ptr, size_t size) {
    if (ptr == NULL)
        return;
    memset(ptr, 0xA5, size);
    DE_FREE((const de_allocator *)user, ptr, size);
}
static void *poison_realloc(void *user, void *ptr, size_t old_size, size_t new_size, size_t align,
                            u32 tag) {
    void *n = DE_ALLOC((const de_allocator *)user, new_size, align, tag);
    if (n == NULL)
        return NULL;
    if (ptr != NULL) {
        memcpy(n, ptr, old_size < new_size ? old_size : new_size);
        poison_free(user, ptr, old_size);
    }
    return n;
}

/* ---- event.* ----------------------------------------------------------------------------------
 */

TEST_F(event, register_and_send, ecs_fx) {
    const ev_types t = register_ev_types(f->types);

    const de_event_type ev = de_event_register(f->w, t.val);
    ASSERT_TRUE(DE_HANDLE_VALID(ev));
    EXPECT_EQ(ev.id, 1u);
    EXPECT_EQ(de_event_register(f->w, t.val).id, ev.id); /* same type: same id */
    const de_event_type hooked = de_event_register(f->w, t.hooked);
    EXPECT_EQ(hooked.id, 2u);

    /* Refusals. */
    EXPECT_FALSE(DE_HANDLE_VALID(de_event_register(NULL, t.val)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_FALSE(DE_HANDLE_VALID(de_event_register(f->w, (de_type_id){0x7FFFFFF0u})));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_FALSE(DE_HANDLE_VALID(de_event_register(f->w, t.no_destruct)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    de_event_send(f->w, (de_event_type){0}, &(EVal){1u, 0u});
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    de_event_send(f->w, (de_event_type){99u}, &(EVal){1u, 0u});
    de_event_send(f->w, ev, NULL);
    de_event_send(NULL, ev, &(EVal){1u, 0u});

    /* A reader of a bad type reads nothing. */
    de_event_reader bad = de_event_reader_create(f->w, (de_event_type){99u});
    EXPECT_FALSE(DE_HANDLE_VALID(bad.type));
    EXPECT_TRUE(de_event_next(f->w, &bad) == NULL);
    EXPECT_TRUE(de_event_next(f->w, NULL) == NULL);
    EXPECT_EQ(de_event_reader_missed(NULL), 0u);

    /* Nothing was sent by the refused calls. */
    de_event_reader r = de_event_reader_create(f->w, ev);
    EXPECT_TRUE(de_event_next(f->w, &r) == NULL);

    /* Copy hooks run on send; destruct hooks when the event is dropped. */
    const EHooked h = {7u};
    de_event_send(f->w, hooked, &h);
    de_event_send(f->w, hooked, &h);
    EXPECT_EQ(g_copy, 2);
    de_event_reader hr = de_event_reader_create(f->w, hooked);
    const EHooked *got = (const EHooked *)de_event_next(f->w, &hr);
    ASSERT_TRUE(got != NULL);
    EXPECT_EQ(got->value, 7u);
    de_world_update_events(f->w);
    EXPECT_EQ(g_destruct, 0);
    de_world_update_events(f->w);
    EXPECT_EQ(g_destruct, 2);
    de_event_send(f->w, hooked, &h); /* alive at destroy: the world destructs it */

    /* A payload that points into the buffer itself survives the buffer growing: re-send every
     * event read, so event k is a copy of event k - 4 and the buffer grows under the payload. A
     * world on the poisoning allocator makes a stale payload pointer visible. */
    {
        const de_allocator poison = {poison_alloc, poison_realloc, poison_free, &f->alloc};
        de_world_desc pd;
        memset(&pd, 0, sizeof pd);
        pd.allocator = &poison;
        pd.types = f->types;
        de_world *pw = NULL;
        ASSERT_EQ(de_world_create(&pd, &pw), DE_OK);
        const de_event_type pev = de_event_register(pw, t.val);
        ASSERT_TRUE(DE_HANDLE_VALID(pev));
        for (u32 i = 0; i < 4; ++i)
            send_val(pw, pev, i, 0u);
        de_event_reader rr = de_event_reader_create(pw, pev);
        for (u32 i = 0; i < 64; ++i) {
            const EVal *e = (const EVal *)de_event_next(pw, &rr);
            ASSERT_TRUE(e != NULL);
            de_event_send(pw, pev, e);
        }
        de_event_reader check = de_event_reader_create(pw, pev);
        for (u32 k = 0; k < 68; ++k) {
            const EVal *e = (const EVal *)de_event_next(pw, &check);
            ASSERT_TRUE(e != NULL);
            EXPECT_EQ(e->seq, k % 4u);
            EXPECT_EQ(e->tag, 0u);
        }
        EXPECT_TRUE(de_event_next(pw, &check) == NULL);
        de_world_destroy(pw);
    }
    for (u32 i = 0; i < 68; ++i)
        send_val(f->w, ev, i, 0u); /* warms the buffer for the out-of-memory part */

    /* Out of memory while sending: the event is dropped and the error set; the queue stays
     * usable. Sends that fit the existing capacity still work. */
    de_world_update_events(f->w);
    de_world_update_events(f->w);
    de_event_reader om = de_event_reader_create(f->w, ev);
    de_tracking_allocator_fail_after(&f->track, 0);
    u32 ok = 0;
    for (;;) {
        de_error_clear();
        send_val(f->w, ev, 1000u + ok, 0u);
        if (de_error_last() != NULL)
            break;
        ++ok;
        ASSERT_LT(ok, 100000u);
    }
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    de_tracking_allocator_fail_after(&f->track, -1);
    EXPECT_GT(ok, 0u); /* the buffer kept its capacity from the 68 events above */
    EXPECT_EQ(drain_expect(f->w, &om, 1000u), ok);
    send_val(f->w, ev, 1000u + ok, 0u);
    EXPECT_EQ(drain_expect(f->w, &om, 1000u + ok), 1u);
}

TEST_F(event, two_frame_lifetime, ecs_fx) {
    const ev_types t = register_ev_types(f->types);
    const de_event_type ev = de_event_register(f->w, t.val);
    ASSERT_TRUE(DE_HANDLE_VALID(ev));

    /* Frame N: three events. */
    de_event_reader same_frame = de_event_reader_create(f->w, ev);
    de_event_reader next_frame = de_event_reader_create(f->w, ev);
    de_event_reader two_frames = de_event_reader_create(f->w, ev);
    for (u32 i = 0; i < 3; ++i)
        send_val(f->w, ev, i, 0u);
    EXPECT_EQ(drain_expect(f->w, &same_frame, 0u), 3u);
    /* A reader created in frame N after the sends sees them too. */
    de_event_reader created_late = de_event_reader_create(f->w, ev);
    EXPECT_EQ(drain_expect(f->w, &created_late, 0u), 3u);

    /* Frame N + 1: the events are still readable. */
    de_world_update_events(f->w);
    EXPECT_EQ(drain_expect(f->w, &next_frame, 0u), 3u);
    EXPECT_EQ(de_event_reader_missed(&next_frame), 0u);
    de_event_reader created_n1 = de_event_reader_create(f->w, ev);
    EXPECT_EQ(drain_expect(f->w, &created_n1, 0u), 3u);
    send_val(f->w, ev, 3u, 1u); /* sent in N + 1 */

    /* Frame N + 2: the frame-N events are gone; the N + 1 event is still there. */
    de_world_update_events(f->w);
    de_event_reader created_n2 = de_event_reader_create(f->w, ev);
    const EVal *e = (const EVal *)de_event_next(f->w, &created_n2);
    ASSERT_TRUE(e != NULL);
    EXPECT_EQ(e->seq, 3u);
    EXPECT_EQ(e->tag, 1u);
    EXPECT_TRUE(de_event_next(f->w, &created_n2) == NULL);
    /* A reader from frame N that never read: the three are dropped, then it reads event 3. */
    e = (const EVal *)de_event_next(f->w, &two_frames);
    ASSERT_TRUE(e != NULL);
    EXPECT_EQ(e->seq, 3u);
    EXPECT_EQ(de_event_reader_missed(&two_frames), 3u);
    /* Readers that kept up see only the new event, and miss nothing. */
    EXPECT_EQ(drain_expect(f->w, &same_frame, 3u), 1u);
    EXPECT_EQ(de_event_reader_missed(&same_frame), 0u);

    /* Frame N + 3: everything is gone. */
    de_world_update_events(f->w);
    de_event_reader created_n3 = de_event_reader_create(f->w, ev);
    EXPECT_TRUE(de_event_next(f->w, &created_n3) == NULL);
    EXPECT_TRUE(de_event_next(f->w, &next_frame) == NULL);
    EXPECT_EQ(de_event_reader_missed(&next_frame), 1u);
}

TEST_F(event, multiple_readers, ecs_fx) {
    const ev_types t = register_ev_types(f->types);
    const de_event_type ev = de_event_register(f->w, t.val);
    ASSERT_TRUE(DE_HANDLE_VALID(ev));

    de_event_reader a = de_event_reader_create(f->w, ev);
    de_event_reader b = de_event_reader_create(f->w, ev);
    u32 seq = 0, next_a = 0, next_b = 0;
    /* 20 frames. Each frame sends a varying number of events in several bursts; reader a polls
     * after every burst, reader b once per frame (and only every other frame), so b reads events
     * from two frames at once. Each must see each event exactly once, in send order. */
    for (u32 frame = 0; frame < 20; ++frame) {
        for (u32 burst = 0; burst < 3; ++burst) {
            const u32 n = (frame * 7u + burst * 3u) % 11u;
            for (u32 i = 0; i < n; ++i)
                send_val(f->w, ev, seq++, frame);
            next_a += drain_expect(f->w, &a, next_a);
        }
        if (frame % 2u == 1u)
            next_b += drain_expect(f->w, &b, next_b);
        de_world_update_events(f->w);
    }
    next_b += drain_expect(f->w, &b, next_b);
    EXPECT_EQ(next_a, seq);
    EXPECT_EQ(next_b, seq);
    EXPECT_EQ(de_event_reader_missed(&a), 0u);
    EXPECT_EQ(de_event_reader_missed(&b), 0u);

    /* A copy of a reader is an independent reader (plain data). */
    send_val(f->w, ev, seq++, 0u);
    de_event_reader c = a;
    EXPECT_EQ(drain_expect(f->w, &a, next_a), 1u);
    EXPECT_EQ(drain_expect(f->w, &c, next_a), 1u);

    /* Two event types do not share sequence numbers or buffers. */
    static const de_field_info k_val[] = {DE_FIELD(EVal, seq, DE_FK_U32, DE_FIELD_DEFAULT),
                                          DE_FIELD(EVal, tag, DE_FK_U32, DE_FIELD_DEFAULT)};
    de_type_desc d = DE_TYPE_DESC(EVal, 1, k_val);
    d.name = "EVal2";
    const de_type_id t2 = de_type_register(f->types, &d);
    const de_event_type ev2 = de_event_register(f->w, t2);
    ASSERT_TRUE(DE_HANDLE_VALID(ev2));
    de_event_reader r2 = de_event_reader_create(f->w, ev2);
    send_val(f->w, ev2, 0u, 2u);
    send_val(f->w, ev, seq++, 0u);
    send_val(f->w, ev2, 1u, 2u);
    EXPECT_EQ(drain_expect(f->w, &r2, 0u), 2u);
    EXPECT_EQ(r2.cursor, 2u);
}

TEST_F(event, late_reader, ecs_fx) {
    const ev_types t = register_ev_types(f->types);
    const de_event_type ev = de_event_register(f->w, t.val);
    ASSERT_TRUE(DE_HANDLE_VALID(ev));

    /* No events yet: an early reader sees later events. */
    de_event_reader early = de_event_reader_create(f->w, ev);
    EXPECT_TRUE(de_event_next(f->w, &early) == NULL);

    /* Frame 0: events 0..4. Frame 1: events 5..9. */
    for (u32 i = 0; i < 5; ++i)
        send_val(f->w, ev, i, 0u);
    de_world_update_events(f->w);
    for (u32 i = 5; i < 10; ++i)
        send_val(f->w, ev, i, 1u);

    /* Created in frame 1: starts at event 0, the oldest still alive. */
    de_event_reader r1 = de_event_reader_create(f->w, ev);
    EXPECT_EQ(r1.cursor, 0u);
    EXPECT_EQ(drain_expect(f->w, &r1, 0u), 10u);
    EXPECT_EQ(de_event_reader_missed(&r1), 0u);
    EXPECT_EQ(drain_expect(f->w, &early, 0u), 10u);

    /* Frame 2: events 10..14. Created now: starts at event 5 (frame 0's are gone), misses none. */
    de_world_update_events(f->w);
    for (u32 i = 10; i < 15; ++i)
        send_val(f->w, ev, i, 2u);
    de_event_reader r2 = de_event_reader_create(f->w, ev);
    EXPECT_EQ(r2.cursor, 5u);
    EXPECT_EQ(drain_expect(f->w, &r2, 5u), 10u);
    EXPECT_EQ(de_event_reader_missed(&r2), 0u);

    /* Frame 3, no sends in frame 2's successor yet: starts at event 10. */
    de_world_update_events(f->w);
    de_event_reader r3 = de_event_reader_create(f->w, ev);
    EXPECT_EQ(r3.cursor, 10u);
    EXPECT_EQ(drain_expect(f->w, &r3, 10u), 5u);

    /* Frames 4 and 5 without events: a reader created then reads the next event sent. */
    de_world_update_events(f->w);
    de_world_update_events(f->w);
    de_event_reader r5 = de_event_reader_create(f->w, ev);
    EXPECT_EQ(r5.cursor, 15u);
    EXPECT_TRUE(de_event_next(f->w, &r5) == NULL);
    send_val(f->w, ev, 15u, 5u);
    EXPECT_EQ(drain_expect(f->w, &r5, 15u), 1u);
}

TEST_F(event, missed_count, ecs_fx) {
    const ev_types t = register_ev_types(f->types);
    const de_event_type ev = de_event_register(f->w, t.val);
    ASSERT_TRUE(DE_HANDLE_VALID(ev));

    de_event_reader r = de_event_reader_create(f->w, ev);
    u32 seq = 0;
    /* Frame 0: read everything. */
    for (u32 i = 0; i < 4; ++i)
        send_val(f->w, ev, seq++, 0u);
    EXPECT_EQ(drain_expect(f->w, &r, 0u), 4u);

    /* Frames 1, 2, 3: four events each; the reader is not polled. */
    for (u32 frame = 1; frame <= 3; ++frame) {
        de_world_update_events(f->w);
        for (u32 i = 0; i < 4; ++i)
            send_val(f->w, ev, seq++, frame);
    }
    /* Frame 1's events (4..7) are gone; frames 2 and 3 (8..15) are alive. */
    EXPECT_EQ(de_event_reader_missed(&r), 0u); /* known only when the reader is polled */
    EXPECT_EQ(drain_expect(f->w, &r, 8u), 8u);
    EXPECT_EQ(de_event_reader_missed(&r), 4u);
    EXPECT_GT(de_event_reader_missed(&r), 0u);

    /* It continues normally afterwards; the count is cumulative. */
    for (u32 frame = 4; frame < 10; ++frame) {
        de_world_update_events(f->w);
        for (u32 i = 0; i < 4; ++i)
            send_val(f->w, ev, seq++, frame);
        EXPECT_EQ(drain_expect(f->w, &r, seq - 4u), 4u);
    }
    EXPECT_EQ(de_event_reader_missed(&r), 4u);

    /* Not polled for 3 frames with nothing sent after: everything dropped, NULL, counted. */
    for (u32 i = 0; i < 3; ++i)
        de_world_update_events(f->w);
    send_val(f->w, ev, seq++, 99u);
    de_world_update_events(f->w);
    de_world_update_events(f->w);
    EXPECT_TRUE(de_event_next(f->w, &r) == NULL);
    EXPECT_EQ(de_event_reader_missed(&r), 5u);
    send_val(f->w, ev, seq++, 100u);
    EXPECT_EQ(drain_expect(f->w, &r, seq - 1u), 1u);

    /* The count saturates instead of wrapping. */
    de_event_reader old = r;
    old.cursor = 0;
    old.missed = 0xFFFFFFF0u;
    (void)de_event_next(f->w, &old);
    EXPECT_EQ(de_event_reader_missed(&old), 0xFFFFFFFFu);
}

TEST_F(event, no_alloc_steady_state, ecs_fx) {
    const ev_types t = register_ev_types(f->types);
    const de_event_type ev = de_event_register(f->w, t.val);
    ASSERT_TRUE(DE_HANDLE_VALID(ev));

    enum { EVENTS_PER_FRAME = 10000, FRAMES = 100 };
    de_event_reader r = de_event_reader_create(f->w, ev);
    u32 seq = 0;
    u64 read_total = 0;
    de_tracking_stats warm;
    memset(&warm, 0, sizeof warm);
    for (u32 frame = 0; frame < FRAMES; ++frame) {
        de_world_update_events(f->w);
        if (frame == 2)
            warm = de_tracking_allocator_stats(&f->track);
        for (u32 i = 0; i < EVENTS_PER_FRAME; ++i)
            send_val(f->w, ev, seq++, frame);
        const u32 n = drain_expect(f->w, &r, seq - EVENTS_PER_FRAME);
        EXPECT_EQ(n, (u32)EVENTS_PER_FRAME);
        read_total += n;
    }
    const de_tracking_stats end = de_tracking_allocator_stats(&f->track);
    EXPECT_EQ(end.alloc_count, warm.alloc_count);
    EXPECT_EQ(end.realloc_count, warm.realloc_count);
    EXPECT_EQ(end.free_count, warm.free_count);
    EXPECT_EQ(read_total, (u64)EVENTS_PER_FRAME * FRAMES);
    EXPECT_EQ(de_event_reader_missed(&r), 0u);
}
