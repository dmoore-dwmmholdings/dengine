/* record.c: input recording per fixed tick (DENG-89): the schema hash, de_input_record_begin /
 * _tick / _save, the .deir writer (format: record_format.h) and the recording accessors.
 *
 * A recording appends delta-encoded tick records into chunk blocks from its allocator. A tick
 * record goes into the current chunk when the chunk still has room for the worst-case record of
 * the schema, so it never has to grow; otherwise the next block is taken (preallocated by
 * de_input_record_desc.reserve_ticks, else allocated). Steady-state recording therefore
 * allocates nothing per tick.
 *
 * SPEC §5.7: part of the DETERMINISTIC module de_input. Actions are visited in declaration order;
 * the values come from the quantized tick view (pack.c). No float math here. */
#include "record_format.h"

#include <string.h>

/* ---------- Schema ---------- */

/* Domain tag of the schema hash chain; a format change that changes the schema meaning changes
 * it. */
static const char k_schema_tag[] = "DEIR-schema-1";

u64 de__input_schema(const de_input *in, u8 *types, u32 *count) {
    const struct de__actions *s = de__actions_state_const(in);
    const u32 n = s != NULL ? s->action_count : 0u;
    if (count != NULL)
        *count = n;
    if (n == 0)
        return 0;
    /* XXH3-64 chained through the seed: one step per action, in declaration order, over
     * (map name length, map name, action name length, action name, type). Names are at most
     * DE_ACTION_NAME_MAX bytes. */
    u64 h = de_hash64(k_schema_tag, sizeof k_schema_tag - 1u, 0);
    u8 buf[2u * (DE_ACTION_NAME_MAX + 1u) + 1u];
    for (u32 i = 0; i < n; ++i) {
        const de__action *act = &s->actions[i];
        const char *map_name = act->map < s->map_count ? s->maps[act->map].name : "";
        const size_t ml = strlen(map_name);
        const size_t al = strlen(act->name);
        u32 k = 0;
        buf[k++] = (u8)ml;
        memcpy(&buf[k], map_name, ml);
        k += (u32)ml;
        buf[k++] = (u8)al;
        memcpy(&buf[k], act->name, al);
        k += (u32)al;
        buf[k++] = (u8)act->type;
        h = de_hash64(buf, k, h);
        if (types != NULL)
            types[i] = (u8)act->type;
    }
    return h;
}

u64 de_input_schema_hash(const de_input *in) {
    return de__input_schema(in, NULL, NULL);
}

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

u32 de__record_max_bytes(const u8 *types, u32 count) {
    u32 n = DE__VARINT_MAX + 2u /* changed count <= 256: 2 bytes */ + (count + 7u) / 8u;
    for (u32 i = 0; i < count; ++i)
        n += payload_bytes(types[i]);
    return n;
}

/* ---------- Recording ---------- */

static void free_chunks(de_input_recording *rec) {
    if (rec->chunks == NULL)
        return;
    for (u32 i = 0; i < rec->chunk_capacity; ++i) {
        de__rec_chunk *c = &rec->chunks[i];
        if (c->data != NULL && c->capacity != 0)
            DE_FREE(&rec->alloc, c->data, c->capacity);
    }
    DE_FREE(&rec->alloc, rec->chunks, (size_t)rec->chunk_capacity * sizeof *rec->chunks);
    rec->chunks = NULL;
    rec->chunk_capacity = 0;
    rec->chunk_count = 0;
}

/** Grows the chunk descriptor array to at least `want` entries (new ones zeroed). */
static de_result grow_descriptors(de_input_recording *rec, u32 want) {
    if (want <= rec->chunk_capacity)
        return DE_OK;
    u32 cap = rec->chunk_capacity != 0 ? rec->chunk_capacity : 4u;
    while (cap < want)
        cap *= 2u;
    de__rec_chunk *c = (de__rec_chunk *)DE_REALLOC(
        &rec->alloc, rec->chunks, (size_t)rec->chunk_capacity * sizeof *rec->chunks,
        (size_t)cap * sizeof *rec->chunks, DE_ALIGNOF(de__rec_chunk), rec->mem_tag);
    if (c == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "input recording: %u chunk descriptors",
                       (unsigned)cap);
    memset(&c[rec->chunk_capacity], 0, (size_t)(cap - rec->chunk_capacity) * sizeof *c);
    rec->chunks = c;
    rec->chunk_capacity = cap;
    return DE_OK;
}

/** Makes sure descriptor `i` has a data block. */
static de_result ensure_block(de_input_recording *rec, u32 i) {
    de__rec_chunk *c = &rec->chunks[i];
    if (c->data != NULL)
        return DE_OK;
    c->data = (u8 *)DE_ALLOC(&rec->alloc, rec->chunk_bytes, 16, rec->mem_tag);
    if (c->data == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "input recording: chunk of %u bytes",
                       (unsigned)rec->chunk_bytes);
    c->capacity = rec->chunk_bytes;
    c->bytes = 0;
    c->tick_count = 0;
    return DE_OK;
}

de_result de_input_record_begin(de_input *in, const de_input_record_desc *desc,
                                de_input_recording **out) {
    if (out != NULL)
        *out = NULL;
    if (in == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_input_record_begin: in and out are required");
    de_input_record_desc d;
    memset(&d, 0, sizeof d);
    if (desc != NULL)
        d = *desc;

    u8 types[DE_ACTION_MAX_ACTIONS];
    u32 count = 0;
    const u64 schema = de__input_schema(in, types, &count);
    if (count == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_input_record_begin: the input has no actions");

    /* The simulation must read the quantized tick view while it is recorded: quantization is
     * turned on and stays on (during a replay of `in`, it is what comes back after it). */
    const de_result qr = de_input_set_tick_quantization(in, true);
    if (qr != DE_OK)
        return DE_FAIL_WRAP(qr, "de_input_record_begin: cannot quantize the tick view");

    const de_allocator *a = d.allocator != NULL ? d.allocator : &in->alloc;
    de_input_recording *rec =
        (de_input_recording *)DE_ALLOC(a, sizeof *rec, DE_ALIGNOF(de_input_recording), in->mem_tag);
    if (rec == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_input_record_begin: %u bytes",
                       (unsigned)sizeof *rec);
    memset(rec, 0, sizeof *rec);
    rec->alloc = *a;
    rec->mem_tag = in->mem_tag;
    rec->in = in;
    rec->schema_hash = schema;
    rec->tick_rate = d.tick_rate != 0 ? d.tick_rate : 60u;
    rec->action_count = count;
    memcpy(rec->types, types, count);
    rec->max_record = de__record_max_bytes(types, count);
    rec->chunk_bytes =
        rec->max_record > DE__DEIR_CHUNK_BYTES ? rec->max_record : DE__DEIR_CHUNK_BYTES;

    /* Preallocate: every block holds at least chunk_bytes / max_record worst-case records. */
    const u32 per_chunk = rec->chunk_bytes / rec->max_record;
    u32 blocks = d.reserve_ticks != 0 ? (d.reserve_ticks + per_chunk - 1u) / per_chunk : 1u;
    de_result r = grow_descriptors(rec, blocks + 1u);
    for (u32 i = 0; r == DE_OK && i < blocks; ++i)
        r = ensure_block(rec, i);
    if (r != DE_OK) {
        de_input_recording_destroy(rec);
        return DE_FAIL_WRAP(r, "de_input_record_begin: reserving %u ticks",
                            (unsigned)d.reserve_ticks);
    }
    *out = rec;
    return DE_OK;
}

de_result de_input_record_tick(de_input *in, de_input_recording *rec, u64 tick) {
    if (in == NULL || rec == NULL || rec->in != in || rec->loaded)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_input_record_tick: needs the input state and a recording made from it");
    const struct de__actions *s = de__actions_state_const(in);
    if (s == NULL || s->action_count != rec->action_count)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_input_record_tick: the action schema changed (%u actions, recorded %u)",
                       s != NULL ? (unsigned)s->action_count : 0u, (unsigned)rec->action_count);
    if (rec->tick_count != 0 && tick <= rec->last_tick)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_input_record_tick: tick %llu is not after the last recorded tick %llu",
                       (unsigned long long)tick, (unsigned long long)rec->last_tick);
    const u32 n = rec->action_count;
    for (u32 i = 0; i < n; ++i) {
        if ((u8)s->actions[i].type != rec->types[i])
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_input_record_tick: action '%s' changed its type",
                           s->actions[i].name);
    }

    /* The chunk: the current one if the worst-case record fits, else the next block. */
    de__rec_chunk *c = rec->chunk_count != 0 ? &rec->chunks[rec->chunk_count - 1u] : NULL;
    if (c == NULL || c->capacity - c->bytes < rec->max_record) {
        de_result r = grow_descriptors(rec, rec->chunk_count + 2u);
        if (r == DE_OK)
            r = ensure_block(rec, rec->chunk_count);
        if (r != DE_OK)
            return DE_FAIL_WRAP(r, "de_input_record_tick: tick %llu", (unsigned long long)tick);
        c = &rec->chunks[rec->chunk_count++];
        c->bytes = 0;
        c->tick_count = 0;
        c->first_tick = tick;
        c->last_tick = tick;
        memset(rec->prev, 0, sizeof rec->prev); /* chunks are self-contained */
    }

    de__qstate q[DE_ACTION_MAX_ACTIONS];
    u8 mask[DE_ACTION_MAX_ACTIONS / 8u];
    const u32 mask_bytes = (n + 7u) / 8u;
    memset(mask, 0, sizeof mask);
    u32 changed = 0;
    for (u32 i = 0; i < n; ++i) {
        q[i] = de__quantize_state(&s->actions[i], &s->actions[i].tick);
        if (q[i].flags != rec->prev[i].flags || q[i].x != rec->prev[i].x ||
            q[i].y != rec->prev[i].y) {
            mask[i >> 3] |= (u8)(1u << (i & 7u));
            changed++;
        }
    }

    u8 *p = c->data + c->bytes;
    u32 k = 0;
    k += de__varint_put(&p[k], c->tick_count == 0 ? 0u : tick - c->last_tick);
    k += de__varint_put(&p[k], changed);
    if (changed != 0) {
        memcpy(&p[k], mask, mask_bytes);
        k += mask_bytes;
        for (u32 i = 0; i < n; ++i) {
            if ((mask[i >> 3] & (1u << (i & 7u))) == 0)
                continue;
            p[k++] = q[i].flags;
            if (rec->types[i] != DE_ACTION_BUTTON) {
                de__le16(&p[k], (u16)q[i].x);
                k += 2u;
            }
            if (rec->types[i] == DE_ACTION_AXIS2D) {
                de__le16(&p[k], (u16)q[i].y);
                k += 2u;
            }
        }
    }
    c->bytes += k;
    c->tick_count++;
    c->last_tick = tick;
    memcpy(rec->prev, q, (size_t)n * sizeof q[0]);
    if (rec->tick_count == 0)
        rec->first_tick = tick;
    rec->last_tick = tick;
    rec->tick_count++;
    return DE_OK;
}

/* ---------- Writer ---------- */

u64 de__recording_file_size(const de_input_recording *rec) {
    u64 size = DE__DEIR_HEADER_FIXED + (u64)rec->action_count + DE__DEIR_TRAILER;
    for (u32 i = 0; i < rec->chunk_count; ++i)
        size += DE__DEIR_CHUNK_HEADER + (u64)rec->chunks[i].bytes;
    return size;
}

void de__recording_write_file(const de_input_recording *rec, u8 *out) {
    u8 *p = out;
    memcpy(p, "DEIR", 4);
    de__le16(p + 4, (u16)DE__DEIR_VERSION);
    de__le16(p + 6, 0);
    de__le64(p + 8, rec->schema_hash);
    de__le32(p + 16, rec->tick_rate);
    de__le32(p + 20, rec->action_count);
    de__le64(p + 24, rec->tick_count != 0 ? rec->first_tick : 0u);
    de__le32(p + 32, rec->tick_count);
    de__le32(p + 36, rec->chunk_count);
    memcpy(p + DE__DEIR_HEADER_FIXED, rec->types, rec->action_count);
    p += DE__DEIR_HEADER_FIXED + rec->action_count;
    for (u32 i = 0; i < rec->chunk_count; ++i) {
        const de__rec_chunk *c = &rec->chunks[i];
        de__le64(p, c->first_tick);
        de__le32(p + 8, c->tick_count);
        de__le32(p + 12, c->bytes);
        p += DE__DEIR_CHUNK_HEADER;
        if (c->bytes != 0)
            memcpy(p, c->data, c->bytes);
        p += c->bytes;
    }
    const u64 sum = de_hash64(out, (size_t)(p - out), 0);
    de__le64(p, sum);
}

/** The file bytes of `rec` in a block from its allocator. */
static u8 *file_bytes(const de_input_recording *rec, u64 *size) {
    *size = de__recording_file_size(rec);
    if (*size > (u64)(size_t)-1)
        return NULL;
    de_allocator a = rec->alloc;
    u8 *buf = (u8 *)DE_ALLOC(&a, (size_t)*size, 16, rec->mem_tag);
    if (buf != NULL)
        de__recording_write_file(rec, buf);
    return buf;
}

de_result de_input_record_save(const de_input_recording *rec, const char *vpath) {
    if (rec == NULL || vpath == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_input_record_save: rec and vpath are required");
    u64 size = 0;
    u8 *buf = file_bytes(rec, &size);
    if (buf == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_input_record_save: %s: %llu bytes", vpath,
                       (unsigned long long)size);
    const de_result r = de_file_write_all_atomic(vpath, buf, size);
    de_allocator a = rec->alloc;
    DE_FREE(&a, buf, (size_t)size);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "de_input_record_save: writing %s", vpath);
    return DE_OK;
}

u64 de_input_recording_checksum(const de_input_recording *rec) {
    if (rec == NULL)
        return 0;
    u64 size = 0;
    u8 *buf = file_bytes(rec, &size);
    if (buf == NULL)
        return 0;
    const u64 sum = de__rd64(buf + size - DE__DEIR_TRAILER);
    de_allocator a = rec->alloc;
    DE_FREE(&a, buf, (size_t)size);
    return sum;
}

/* ---------- Accessors and destroy ---------- */

u32 de_input_recording_tick_count(const de_input_recording *rec) {
    return rec != NULL ? rec->tick_count : 0u;
}

u64 de_input_recording_first_tick(const de_input_recording *rec) {
    return rec != NULL && rec->tick_count != 0 ? rec->first_tick : 0u;
}

u64 de_input_recording_last_tick(const de_input_recording *rec) {
    return rec != NULL && rec->tick_count != 0 ? rec->last_tick : 0u;
}

u32 de_input_recording_tick_rate(const de_input_recording *rec) {
    return rec != NULL ? rec->tick_rate : 0u;
}

u64 de_input_recording_schema(const de_input_recording *rec) {
    return rec != NULL ? rec->schema_hash : 0u;
}

void de_input_recording_destroy(de_input_recording *rec) {
    if (rec == NULL)
        return;
    if (rec->loaded)
        de__input_replay_end(rec);
    free_chunks(rec);
    if (rec->file != NULL)
        DE_FREE(&rec->alloc, rec->file, rec->file_alloc_size);
    de_allocator a = rec->alloc;
    DE_FREE(&a, rec, sizeof *rec);
}
