/* replay.c: loading .deir recordings and replaying them per fixed tick (DENG-89). Format:
 * record_format.h.
 *
 * The loader checks the whole file before anything is installed: the header, the checksum, the
 * schema hash and action types against the input state, and every chunk and record (bounds,
 * counts, tick order, value ranges). A file that passes cannot make de_input_replay_tick read
 * out of bounds. The loaded file stays in memory; chunks are views into it, and replay decodes
 * one record at a time.
 *
 * Replay is the tick override de__input_replay_override: after each tick latch it writes the
 * staged recorded state (selected by de_input_replay_tick) into the tick view of every action,
 * so live device input never reaches the tick view. A trigger is applied in one tick only.
 *
 * SPEC §5.7: part of the DETERMINISTIC module de_input. No float math besides the dequantize
 * division (pack.c). */
#include "record_format.h"

#include <stdint.h>
#include <string.h>

static u32 payload_bytes(u8 type) {
    switch (type) {
    case DE_ACTION_BUTTON:
        return 1u;
    case DE_ACTION_AXIS1D:
        return 3u;
    default:
        return 5u;
    }
}

static u32 popcount8(u8 b) {
    u32 n = 0;
    for (; b != 0; b = (u8)(b & (b - 1u)))
        ++n;
    return n;
}

/**
 * Decodes one tick record from d[*off .. end) into `st` (the n actions' states, updated in
 * place) and `*delta`. False when the record is malformed; `st` may then be partly updated.
 */
static bool decode_record(const u8 *d, u32 end, u32 *off, u32 n, const u8 *types, de__qstate *st,
                          u64 *delta) {
    u32 o = *off;
    u64 dl = 0, count = 0;
    if (!de__varint_get(d, end, &o, &dl) || !de__varint_get(d, end, &o, &count) || count > n)
        return false;
    if (count != 0) {
        const u32 mask_bytes = (n + 7u) / 8u;
        if (end - o < mask_bytes)
            return false;
        const u8 *mask = d + o;
        o += mask_bytes;
        u32 bits = 0;
        for (u32 j = 0; j < mask_bytes; ++j)
            bits += popcount8(mask[j]);
        if ((n % 8u) != 0 && (mask[mask_bytes - 1u] >> (n % 8u)) != 0)
            return false; /* a bit at or above n */
        if (bits != count)
            return false;
        for (u32 i = 0; i < n; ++i) {
            if ((mask[i >> 3] & (1u << (i & 7u))) == 0)
                continue;
            const u32 need = payload_bytes(types[i]);
            if (end - o < need)
                return false;
            de__qstate q;
            memset(&q, 0, sizeof q);
            q.flags = d[o];
            if ((q.flags & ~(DE__Q_HELD | DE__Q_TRIGGERED)) != 0)
                return false;
            if (types[i] != DE_ACTION_BUTTON) {
                q.x = (i16)de__rd16(d + o + 1u);
                if (q.x == INT16_MIN)
                    return false;
            }
            if (types[i] == DE_ACTION_AXIS2D) {
                q.y = (i16)de__rd16(d + o + 3u);
                if (q.y == INT16_MIN)
                    return false;
            }
            st[i] = q;
            o += need;
        }
    }
    *off = o;
    *delta = dl;
    return true;
}

/* ---------- Override ---------- */

void de__input_replay_override(de_input *in, u64 tick, void *user) {
    (void)tick;
    de_input_recording *rec = (de_input_recording *)user;
    struct de__actions *s = de__actions_state(in);
    if (s == NULL || rec == NULL)
        return;
    de__qstate idle;
    memset(&idle, 0, sizeof idle);
    for (u32 i = 0; i < s->action_count; ++i) {
        de__qstate q = (rec->staged_valid && i < rec->action_count) ? rec->staged[i] : idle;
        if (rec->staged_applied)
            q.flags = (u8)(q.flags & ~DE__Q_TRIGGERED); /* a trigger shows in one tick */
        de__dequantize_state(&s->actions[i], &q, &s->actions[i].tick);
    }
    rec->staged_applied = true;
}

void de__input_replay_end(de_input_recording *rec) {
    if (rec == NULL || !rec->active)
        return;
    rec->active = false;
    struct de__actions *s = de__actions_state(rec->in);
    if (s != NULL && s->tick_override == de__input_replay_override &&
        s->tick_override_user == (void *)rec)
        de__action_tick_override(rec->in, rec->prev_override, rec->prev_user);
}

/* ---------- Loading ---------- */

/** Frees `file` and returns the error set by `r`. */
static de_result fail_free(const de_allocator *a, u8 *file, size_t alloc_size, de_result r) {
    if (file != NULL)
        DE_FREE(a, file, alloc_size);
    return r;
}

/**
 * Checks `size` bytes at `file` and makes the recording. Takes ownership of `file` (an
 * `alloc_size` block from `a`): it is freed on failure and owned by the recording on success.
 */
static de_result replay_open(de_input *in, u8 *file, u64 size, size_t alloc_size,
                             const de_allocator *a, const char *source, de_input_recording **out) {
    const struct de__actions *s = de__actions_state_const(in);
    if (s != NULL && s->tick_override == de__input_replay_override)
        return fail_free(
            a, file, alloc_size,
            DE_FAIL(DE_ERR_BUSY, "%s: a replay of this input state is running", source));
    if (size < DE__DEIR_HEADER_FIXED + DE__DEIR_TRAILER)
        return fail_free(a, file, alloc_size,
                         DE_FAIL(DE_ERR_PARSE, "%s: truncated .deir file (%llu bytes)", source,
                                 (unsigned long long)size));
    if (memcmp(file, "DEIR", 4) != 0)
        return fail_free(a, file, alloc_size,
                         DE_FAIL(DE_ERR_PARSE, "%s: not a .deir file (bad magic)", source));
    const u32 version = de__rd16(file + 4);
    if (version != DE__DEIR_VERSION)
        return fail_free(a, file, alloc_size,
                         DE_FAIL(DE_ERR_VERSION_MISMATCH, "%s: .deir version %u (expected %u)",
                                 source, (unsigned)version, (unsigned)DE__DEIR_VERSION));
    if (de__rd16(file + 6) != 0)
        return fail_free(a, file, alloc_size,
                         DE_FAIL(DE_ERR_PARSE, "%s: unknown .deir flags 0x%04x", source,
                                 (unsigned)de__rd16(file + 6)));
    const u32 n = de__rd32(file + 20);
    if (n == 0 || n > DE_ACTION_MAX_ACTIONS)
        return fail_free(a, file, alloc_size,
                         DE_FAIL(DE_ERR_PARSE, "%s: bad action count %u", source, (unsigned)n));
    if (size < (u64)DE__DEIR_HEADER_FIXED + n + DE__DEIR_TRAILER || size > 0xFFFFFFFFu)
        return fail_free(a, file, alloc_size,
                         DE_FAIL(DE_ERR_PARSE, "%s: truncated .deir file (%llu bytes)", source,
                                 (unsigned long long)size));
    const u64 stored_sum = de__rd64(file + size - DE__DEIR_TRAILER);
    const u64 sum = de_hash64(file, (size_t)(size - DE__DEIR_TRAILER), 0);
    if (stored_sum != sum)
        return fail_free(a, file, alloc_size,
                         DE_FAIL(DE_ERR_PARSE,
                                 "%s: checksum mismatch (file %016llx, data %016llx): truncated or "
                                 "corrupt",
                                 source, (unsigned long long)stored_sum, (unsigned long long)sum));
    const u8 *types = file + DE__DEIR_HEADER_FIXED;
    for (u32 i = 0; i < n; ++i) {
        if (types[i] > (u8)DE_ACTION_AXIS2D)
            return fail_free(a, file, alloc_size,
                             DE_FAIL(DE_ERR_PARSE, "%s: action %u has the unknown type %u", source,
                                     (unsigned)i, (unsigned)types[i]));
    }

    /* The schema must be the input state's. */
    u8 live_types[DE_ACTION_MAX_ACTIONS];
    u32 live_n = 0;
    const u64 live = de__input_schema(in, live_types, &live_n);
    const u64 schema = de__rd64(file + 8);
    if (schema != live || live_n != n)
        return fail_free(a, file, alloc_size,
                         DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                                 "%s: action-map schema mismatch: recording %016llx (%u actions), "
                                 "input %016llx (%u actions)",
                                 source, (unsigned long long)schema, (unsigned)n,
                                 (unsigned long long)live, (unsigned)live_n));
    if (memcmp(types, live_types, n) != 0)
        return fail_free(a, file, alloc_size,
                         DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                                 "%s: action-map schema mismatch: the action types differ",
                                 source));
    const u32 tick_rate = de__rd32(file + 16);
    const u64 start_tick = de__rd64(file + 24);
    const u32 tick_count = de__rd32(file + 32);
    const u32 chunk_count = de__rd32(file + 36);
    const u32 body = DE__DEIR_HEADER_FIXED + n;
    const u32 body_end = (u32)(size - DE__DEIR_TRAILER);
    if (tick_rate == 0 || chunk_count > (body_end - body) / DE__DEIR_CHUNK_HEADER)
        return fail_free(a, file, alloc_size,
                         DE_FAIL(DE_ERR_PARSE, "%s: bad header (tick rate %u, %u chunks)", source,
                                 (unsigned)tick_rate, (unsigned)chunk_count));

    de_input_recording *rec =
        (de_input_recording *)DE_ALLOC(a, sizeof *rec, DE_ALIGNOF(de_input_recording), in->mem_tag);
    if (rec == NULL)
        return fail_free(
            a, file, alloc_size,
            DE_FAIL(DE_ERR_OUT_OF_MEMORY, "%s: %u bytes", source, (unsigned)sizeof *rec));
    memset(rec, 0, sizeof *rec);
    rec->alloc = *a;
    rec->mem_tag = in->mem_tag;
    rec->in = in;
    rec->loaded = true;
    rec->file = file;
    rec->file_alloc_size = alloc_size;
    rec->schema_hash = schema;
    rec->tick_rate = tick_rate;
    rec->action_count = n;
    memcpy(rec->types, types, n);
    rec->max_record = de__record_max_bytes(types, n);
    if (chunk_count != 0) {
        rec->chunks = (de__rec_chunk *)DE_ALLOC(a, (size_t)chunk_count * sizeof *rec->chunks,
                                                DE_ALIGNOF(de__rec_chunk), in->mem_tag);
        if (rec->chunks == NULL) {
            de_input_recording_destroy(rec);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "%s: %u chunks", source, (unsigned)chunk_count);
        }
        memset(rec->chunks, 0, (size_t)chunk_count * sizeof *rec->chunks);
        rec->chunk_capacity = chunk_count;
    }

    /* Every chunk and record. rec->cur is the scratch decoder state. */
    const char *why = NULL;
    u32 off = body;
    u64 total = 0;
    for (u32 k = 0; k < chunk_count && why == NULL; ++k) {
        if (body_end - off < DE__DEIR_CHUNK_HEADER) {
            why = "chunk header past the end";
            break;
        }
        const u64 first = de__rd64(file + off);
        const u32 count = de__rd32(file + off + 8);
        const u32 bytes = de__rd32(file + off + 12);
        off += DE__DEIR_CHUNK_HEADER;
        if (count == 0 || bytes > body_end - off) {
            why = "bad chunk size";
            break;
        }
        if (k != 0 && first <= rec->chunks[k - 1u].last_tick) {
            why = "chunks out of tick order";
            break;
        }
        memset(rec->cur, 0, sizeof rec->cur);
        u32 o = 0;
        u64 t = first;
        for (u32 r = 0; r < count; ++r) {
            u64 delta = 0;
            if (!decode_record(file + off, bytes, &o, n, types, rec->cur, &delta)) {
                why = "malformed tick record";
                break;
            }
            if ((r == 0) != (delta == 0) || t + delta < t) {
                why = "bad tick delta";
                break;
            }
            t += delta;
        }
        if (why == NULL && o != bytes)
            why = "chunk size does not match its records";
        if (why != NULL)
            break;
        de__rec_chunk *c = &rec->chunks[k];
        c->data = file + off;
        c->bytes = bytes;
        c->tick_count = count;
        c->first_tick = first;
        c->last_tick = t;
        total += count;
        off += bytes;
        rec->chunk_count = k + 1u;
    }
    if (why == NULL && off != body_end)
        why = "bytes after the last chunk";
    if (why == NULL && total != tick_count)
        why = "the tick count does not match the chunks";
    if (why == NULL && start_tick != (chunk_count != 0 ? rec->chunks[0].first_tick : 0u))
        why = "the start tick does not match the first chunk";
    if (why != NULL) {
        de_input_recording_destroy(rec);
        return DE_FAIL(DE_ERR_PARSE, "%s: corrupt .deir file: %s", source, why);
    }
    rec->tick_count = tick_count;
    rec->first_tick = chunk_count != 0 ? rec->chunks[0].first_tick : 0u;
    rec->last_tick = chunk_count != 0 ? rec->chunks[chunk_count - 1u].last_tick : 0u;
    memset(rec->cur, 0, sizeof rec->cur);

    /* Install the replay override; the one it replaces comes back at the end. */
    struct de__actions *ws = de__actions_state(in);
    rec->prev_override = ws->tick_override;
    rec->prev_user = ws->tick_override_user;
    de__action_tick_override(in, de__input_replay_override, rec);
    rec->active = true;
    *out = rec;
    return DE_OK;
}

de_result de_input_replay_load(de_input *in, const char *vpath, de_input_recording **out) {
    if (out != NULL)
        *out = NULL;
    if (in == NULL || vpath == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_input_replay_load: in, vpath and out are required");
    void *data = NULL;
    u64 size = 0;
    const de_result r = de_file_read_all(vpath, &in->alloc, &data, &size);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "de_input_replay_load: reading %s", vpath);
    return replay_open(in, (u8 *)data, size, (size_t)size + 1u, &in->alloc, vpath, out);
}

de_result de__input_replay_load_mem(de_input *in, const void *data, u64 size, const char *source,
                                    de_input_recording **out) {
    if (out != NULL)
        *out = NULL;
    if (in == NULL || (data == NULL && size != 0) || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "replay load: in, data and out are required");
    if (size > 0xFFFFFFFFu)
        return DE_FAIL(DE_ERR_PARSE, "%s: too large for a .deir file", source);
    const size_t alloc_size = (size_t)size + 1u;
    u8 *copy = (u8 *)DE_ALLOC(&in->alloc, alloc_size, 16, in->mem_tag);
    if (copy == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "%s: %llu bytes", source, (unsigned long long)size);
    if (size != 0)
        memcpy(copy, data, (size_t)size);
    return replay_open(in, copy, size, alloc_size, &in->alloc, source != NULL ? source : "<memory>",
                       out);
}

/* ---------- Replay ---------- */

/** Positions the decoder at the start of chunk `k` (idle state, nothing decoded yet). */
static void seek_chunk(de_input_recording *rec, u32 k) {
    rec->cur_chunk = k;
    rec->cur_offset = 0;
    rec->cur_in_chunk = 0;
    memset(rec->cur, 0, sizeof rec->cur);
    rec->cur_valid = false;
}

/** The tick of the next record, or false at the end of the recording. */
static bool next_tick(const de_input_recording *rec, u64 *t) {
    const de__rec_chunk *c = &rec->chunks[rec->cur_chunk];
    if (rec->cur_in_chunk < c->tick_count) {
        if (rec->cur_in_chunk == 0) {
            *t = c->first_tick;
            return true;
        }
        u32 o = rec->cur_offset;
        u64 delta = 0;
        if (!de__varint_get(c->data, c->bytes, &o, &delta))
            return false; /* cannot happen: the loader checked every record */
        *t = rec->cur_tick + delta;
        return true;
    }
    if (rec->cur_chunk + 1u < rec->chunk_count) {
        *t = rec->chunks[rec->cur_chunk + 1u].first_tick;
        return true;
    }
    return false;
}

/** Decodes records while the next one is at or before `tick`. */
static void advance(de_input_recording *rec, u64 tick) {
    u64 t = 0;
    while (next_tick(rec, &t) && t <= tick) {
        if (rec->cur_in_chunk == rec->chunks[rec->cur_chunk].tick_count)
            seek_chunk(rec, rec->cur_chunk + 1u);
        const de__rec_chunk *c = &rec->chunks[rec->cur_chunk];
        u64 delta = 0;
        if (!decode_record(c->data, c->bytes, &rec->cur_offset, rec->action_count, rec->types,
                           rec->cur, &delta))
            return; /* cannot happen: the loader checked every record */
        rec->cur_tick = t;
        rec->cur_in_chunk++;
        rec->cur_valid = true;
    }
}

bool de_input_replay_tick(de_input *in, de_input_recording *rec, u64 tick) {
    if (in == NULL || rec == NULL || !rec->loaded || rec->in != in || !rec->active)
        return false;
    if (rec->tick_count == 0 || tick > rec->last_tick) {
        de__input_replay_end(rec);
        return false;
    }
    const u32 n = rec->action_count;
    if (tick < rec->first_tick) {
        memset(rec->staged, 0, sizeof rec->staged);
    } else {
        if (!rec->cur_valid || tick < rec->cur_tick) {
            /* Seek: the last chunk that starts at or before `tick` (binary search). */
            u32 lo = 0, hi = rec->chunk_count;
            while (hi - lo > 1u) {
                const u32 mid = lo + (hi - lo) / 2u;
                if (rec->chunks[mid].first_tick <= tick)
                    lo = mid;
                else
                    hi = mid;
            }
            seek_chunk(rec, lo);
        }
        advance(rec, tick);
        memcpy(rec->staged, rec->cur, (size_t)n * sizeof rec->cur[0]);
        if (rec->cur_tick != tick) {
            /* A tick the recording skipped: the state holds, its triggers do not repeat. */
            for (u32 i = 0; i < n; ++i)
                rec->staged[i].flags = (u8)(rec->staged[i].flags & ~DE__Q_TRIGGERED);
        }
    }
    rec->staged_valid = true;
    rec->staged_applied = false;
    return true;
}
