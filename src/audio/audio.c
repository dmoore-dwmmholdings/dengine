/* audio.c: de_audio create / destroy, stats, clock and ECS attach (DENG-308; SPEC §14).
 *
 * Create allocates everything the audio system will use, so the audio thread never allocates:
 * the command queue and pending list (`command_capacity` each), the voice slots and their
 * generations, the active list, the source scratch buffer and the output ring
 * (`audio.buffer_blocks` blocks). Then it installs the real-time guard (Debug and Dev), starts the
 * mixer thread, and lets the device manager pick the backend, open the output and start the
 * control thread.
 */
#include "audio/audio_internal.h"

#include <dengine/de_debug.h>
#include <dengine/de_reflect.h>

#include <string.h>

static u32 next_pow2(u32 v) {
    u32 p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

static de_cvar buffer_blocks_cvar(void) {
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = "audio.buffer_blocks";
    d.help = "Mixer blocks buffered ahead of the audio device (latency vs. underrun safety); "
             "read when the audio system is created";
    d.type = DE_CVAR_INT;
    d.default_number = DE__AUDIO_DEFAULT_RING_BLOCKS;
    d.min = DE__AUDIO_MIN_RING_BLOCKS;
    d.max = DE__AUDIO_MAX_RING_BLOCKS;
    return de_cvar_register(&d);
}

/* Frees what create allocated; works on a partly built system. No thread runs any more. */
static void free_storage(de_audio *a) {
    de_allocator *al = &a->alloc;
    de__audio_ring_free(a);
    if (a->voices != NULL)
        DE_FREE(al, a->voices, sizeof(de__voice) * a->voice_count);
    if (a->voice_gen != NULL)
        DE_FREE(al, a->voice_gen, sizeof(de_atomic_u32) * a->voice_count);
    if (a->active != NULL)
        DE_FREE(al, a->active, sizeof(u32) * a->voice_count);
    if (a->pending != NULL)
        DE_FREE(al, a->pending, sizeof(de__audio_cmd) * a->command_capacity);
    if (a->src_buf != NULL)
        DE_FREE(al, a->src_buf,
                sizeof(f32) * (size_t)a->block_frames * DE__AUDIO_MAX_SOURCE_CHANNELS);
    de_mpmc_destroy(&a->cmd_queue);
    de_mpmc_destroy(&a->free_slots);
    de_mpmc_destroy(&a->retired);
}

static void close_src(de__audio_source *src) {
    if (src != NULL && src->vt != NULL && src->vt->close != NULL)
        src->vt->close(src);
}

/* Closes every source the system still owns. No audio, device or control thread runs. */
static void close_all_sources(de_audio *a) {
    de__audio_collect(a); /* ended voices */
    for (u32 i = 0; i < a->voice_count; ++i) {
        de__voice *v = &a->voices[i];
        if (v->state != DE__VOICE_FREE) {
            close_src(v->src);
            v->src = NULL;
            v->state = DE__VOICE_FREE;
        }
    }
    a->active_count = 0;
    de__audio_cmd_discard_all(a); /* queued play commands own their sources */
}

static de_result alloc_storage(de_audio *a) {
    de_allocator *al = &a->alloc;
    de_mpmc_desc qd;
    memset(&qd, 0, sizeof qd);
    qd.capacity = a->command_capacity;
    qd.elem_size = (u32)sizeof(de__audio_cmd);
    qd.tag = DE_MEM_AUDIO;
    qd.allocator = al;
    de_result r = de_mpmc_init(&a->cmd_queue, &qd);
    if (r != DE_OK)
        return r;
    qd.capacity = next_pow2(a->voice_count);
    qd.elem_size = (u32)sizeof(u32);
    if ((r = de_mpmc_init(&a->free_slots, &qd)) != DE_OK)
        return r;
    if ((r = de_mpmc_init(&a->retired, &qd)) != DE_OK)
        return r;

    a->voices = (de__voice *)DE_ALLOC(al, sizeof(de__voice) * a->voice_count, DE_ALIGNOF(de__voice),
                                      DE_MEM_AUDIO);
    a->voice_gen = (de_atomic_u32 *)DE_ALLOC(al, sizeof(de_atomic_u32) * a->voice_count,
                                             DE_ALIGNOF(de_atomic_u32), DE_MEM_AUDIO);
    a->active = (u32 *)DE_ALLOC(al, sizeof(u32) * a->voice_count, DE_ALIGNOF(u32), DE_MEM_AUDIO);
    a->pending = (de__audio_cmd *)DE_ALLOC(al, sizeof(de__audio_cmd) * a->command_capacity,
                                           DE_ALIGNOF(de__audio_cmd), DE_MEM_AUDIO);
    a->src_buf =
        (f32 *)DE_ALLOC(al, sizeof(f32) * (size_t)a->block_frames * DE__AUDIO_MAX_SOURCE_CHANNELS,
                        64, DE_MEM_AUDIO);
    if (a->voices != NULL) /* zeroed first: a failed create walks the voices to close sources */
        memset(a->voices, 0, sizeof(de__voice) * a->voice_count);
    if (a->voices == NULL || a->voice_gen == NULL || a->active == NULL || a->pending == NULL ||
        a->src_buf == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                       "audio: cannot allocate the voice and command storage");
    for (u32 i = 0; i < a->voice_count; ++i) {
        de_atomic_init_u32(&a->voice_gen[i], 1);
        bool ok = de_mpmc_try_push(&a->free_slots, &i);
        DE_ASSERT(ok);
        (void)ok;
    }
    return DE_OK;
}

de_result de__audio_create_ex(const de_audio_desc *desc, const de__audio_create_opts *opts,
                              de_audio **out) {
    if (out == NULL || desc == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_audio_create: desc and out are required");
    *out = NULL;
    const u32 rate = desc->sample_rate ? desc->sample_rate : DE__AUDIO_DEFAULT_RATE;
    const u32 block = desc->block_frames ? desc->block_frames : DE__AUDIO_DEFAULT_BLOCK;
    const u32 voices = desc->max_voices ? desc->max_voices : DE__AUDIO_DEFAULT_VOICES;
    const u32 commands =
        desc->command_capacity ? desc->command_capacity : DE__AUDIO_DEFAULT_COMMANDS;
    if (rate < 8000 || rate > 192000)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_audio_create: sample_rate %u not in 8000..192000", rate);
    if (block < 16 || block > 4096)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_audio_create: block_frames %u not in 16..4096",
                       block);
    if (voices > DE__VOICE_MAX_SLOTS)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_audio_create: max_voices %u above %u", voices,
                       DE__VOICE_MAX_SLOTS);
    if (commands > DE__AUDIO_MAX_COMMANDS)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_audio_create: command_capacity %u above %u",
                       commands, DE__AUDIO_MAX_COMMANDS);
    if ((u32)desc->backend > (u32)DE_AUDIO_NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_audio_create: unknown backend %d",
                       (int)desc->backend);
    const bool manual = opts != NULL && opts->null_manual;
    if (manual && desc->backend != DE_AUDIO_NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "audio: manual mode needs DE_AUDIO_NULL");

    i64 blocks = de_cvar_int(buffer_blocks_cvar());
    if (blocks < (i64)DE__AUDIO_MIN_RING_BLOCKS)
        blocks = DE__AUDIO_DEFAULT_RING_BLOCKS; /* not registered (Shipping, error): default */
    if (blocks > (i64)DE__AUDIO_MAX_RING_BLOCKS)
        blocks = DE__AUDIO_MAX_RING_BLOCKS;

    const de_allocator *al = desc->allocator != NULL ? desc->allocator : de_heap_allocator();
    de_audio *a = DE_NEW(al, de_audio, DE_MEM_AUDIO);
    if (a == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_audio_create: out of memory");
    memset(a, 0, sizeof *a);
    a->alloc = *al;
    a->assets = desc->assets;
    a->log = de_log_register_category("audio", DE_LOG_INFO);
    a->sample_rate = rate;
    a->block_frames = block;
    a->channels = DE__AUDIO_CHANNELS;
    a->voice_count = voices;
    a->command_capacity = next_pow2(commands);
    a->manual = manual;
    de_atomic_init_u32(&a->mixer_waiting, 0);
    de_atomic_init_u32(&a->mixer_quit, 0);
    de_atomic_init_u32(&a->test_stall, 0);
    de_atomic_init_u64(&a->frames_mixed, 0);
    de_atomic_init_u64(&a->play_pos, 0);
    de_atomic_init_u32(&a->underruns, 0);
    de_atomic_init_u32(&a->cmds_dropped, 0);
    de_atomic_init_u32(&a->cmds_late, 0);
    de_atomic_init_u32(&a->voices_playing, 0);
    de_atomic_init_u32(&a->dsp_load_bits, 0);

    bool sem_ok = false, guard = false, device = false;
    de_result r = alloc_storage(a);
    if (r == DE_OK)
        r = de__audio_ring_init(a, (u32)blocks);
    if (r == DE_OK) {
        r = de_sem_init(&a->mixer_wake, 0);
        sem_ok = r == DE_OK;
    }
    if (r == DE_OK) {
        de__rt_guard_install();
        guard = true;
        r = de__audio_device_init(a, desc->backend);
        device = r == DE_OK;
    }
    if (r == DE_OK && !manual)
        r = de__audio_mixer_start(a);
    if (r == DE_OK)
        r = de__audio_device_start(a); /* the mixer is filling the ring already */
    if (r != DE_OK) {
        de__audio_mixer_stop(a);
        if (device)
            de__audio_device_shutdown(a);
        if (guard)
            de__rt_guard_uninstall();
        if (sem_ok)
            de_sem_destroy(&a->mixer_wake);
        if (a->voices != NULL)
            close_all_sources(a);
        free_storage(a);
        DE_FREE(al, a, sizeof *a);
        return r;
    }
    *out = a;
    return DE_OK;
}

de_result de_audio_create(const de_audio_desc *desc, de_audio **out) {
    return de__audio_create_ex(desc, NULL, out);
}

void de_audio_destroy(de_audio *a) {
    if (a == NULL)
        return;
    /* The mixer first (it wakes the control thread), then the control thread and the stream. */
    de__audio_mixer_stop(a);
    de__audio_device_shutdown(a);
    close_all_sources(a);
    de__rt_guard_uninstall();
    de_sem_destroy(&a->mixer_wake);
    free_storage(a);
    de_allocator al = a->alloc;
    DE_FREE(&al, a, sizeof *a);
}

u64 de_audio_clock(const de_audio *a) {
    return a != NULL ? de_atomic_load_u64(&a->play_pos, DE_ATOMIC_RELAXED) : 0;
}

de_audio_stats de_audio_get_stats(const de_audio *a) {
    de_audio_stats s;
    memset(&s, 0, sizeof s);
    if (a == NULL)
        return s;
    s.voices_real = de_atomic_load_u32(&a->voices_playing, DE_ATOMIC_RELAXED);
    s.voices_virtual = 0; /* virtualization: S26 */
    u32 bits = de_atomic_load_u32(&a->dsp_load_bits, DE_ATOMIC_RELAXED);
    memcpy(&s.dsp_load, &bits, sizeof bits);
    s.underruns = de_atomic_load_u32(&a->underruns, DE_ATOMIC_RELAXED);
    s.commands_dropped = de_atomic_load_u32(&a->cmds_dropped, DE_ATOMIC_RELAXED);
    return s;
}

de_result de_audio_attach(de_audio *a, de_world *w) {
    if (a == NULL || w == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_audio_attach: a and w are required");
    de_type_registry *types = de_world_types(w);
    if (types == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_audio_attach: the world has no type registry");
    /* A pointer has no field kind, so the type has no fields: it is never serialized or
     * snapshotted, only looked up. Registering it again (another attach) keeps its id. */
    de_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = "de_audio_resource";
    d.size = (u32)sizeof(de_audio_resource);
    d.align = (u32)DE_ALIGNOF(de_audio_resource);
    d.version = 1;
    d.is_pod = true;
    de_type_id t = de_type_register(types, &d);
    if (!DE_HANDLE_VALID(t))
        return DE_FAIL_WRAP(de_error_last() != NULL ? de_error_last()->result : DE_ERR_INTERNAL,
                            "de_audio_attach: cannot register de_audio_resource");
    de_audio_resource res;
    res.audio = a;
    de_resource_id id = de_resource_insert(w, t, &res);
    if (!DE_HANDLE_VALID(id))
        return DE_FAIL_WRAP(de_error_last() != NULL ? de_error_last()->result : DE_ERR_INTERNAL,
                            "de_audio_attach: cannot insert the resource");
    return DE_OK;
}
