/* record_format.h: the .deir input recording format (v1) and the internals shared by record.c,
 * replay.c and pack.c (DENG-89). SPEC §15, §16.2, §19.4, §5.7.
 *
 * File layout. Every integer is little-endian; there is no padding.
 *
 *   offset  size  field
 *   0       4     magic "DEIR"
 *   4       2     version (DE__DEIR_VERSION)
 *   6       2     flags (0; anything else is refused)
 *   8       8     schema hash (de_input_schema_hash: map and action names and action types, in
 *                 declaration order)
 *   16      4     tick rate (fixed ticks per second)
 *   20      4     action count N (1 .. DE_ACTION_MAX_ACTIONS)
 *   24      8     start tick (the first recorded tick; 0 when there is none)
 *   32      4     tick count (records in all chunks)
 *   36      4     chunk count
 *   40      N     action types (de_action_type, one byte each)
 *   then    ...   chunks, each: u64 first tick, u32 record count (>= 1), u32 byte count, bytes
 *   end-8   8     XXH3-64 (seed 0) of every byte before it
 *
 * A chunk is self-contained: its first record has tick delta 0 (it is the chunk's first tick)
 * and is encoded against the idle state, so a reader can start at any chunk. Chunks follow each
 * other in tick order; the first tick of a chunk is after the last tick of the one before.
 *
 * A tick record, against the state of the record before it in the chunk:
 *   varint  tick delta (0 for the first record of a chunk, >= 1 after it)
 *   varint  changed count C (0 .. N)
 *   if C > 0:
 *     ceil(N / 8) bytes  changed-action bitmask, action i = bit (i % 8) of byte (i / 8); exactly C
 *                        bits set, none at or above N
 *     per set bit, in action order, the action's new quantized state:
 *       button:  u8 flags (bit 0 held, bit 1 triggered; other bits 0)
 *       1D axis: u8 flags, i16 x
 *       2D axis: u8 flags, i16 x, i16 y
 *     (axis values in [-32767, 32767]; -32768 is refused)
 * Varints are unsigned LEB128, at most 10 bytes. */
#ifndef DE_INPUT_RECORD_FORMAT_H
#define DE_INPUT_RECORD_FORMAT_H
#include "action_internal.h"

DE_EXTERN_C_BEGIN

#define DE__DEIR_VERSION 1u
/** Header bytes before the action type table. */
#define DE__DEIR_HEADER_FIXED 40u
/** Bytes of a chunk header (first tick, record count, byte count). */
#define DE__DEIR_CHUNK_HEADER 16u
/** Bytes of the trailing checksum. */
#define DE__DEIR_TRAILER 8u
/** Capacity of a recording chunk block (raised to the worst-case record size when that is
 * larger). */
#define DE__DEIR_CHUNK_BYTES 16384u
/** Longest varint (u64). */
#define DE__VARINT_MAX 10u

/** Quantized state flags. */
#define DE__Q_HELD      0x01u
#define DE__Q_TRIGGERED 0x02u

/** One action's quantized tick state: what is recorded, packed and checksummed. */
typedef struct de__qstate {
    u8 flags; /**< DE__Q_HELD, DE__Q_TRIGGERED */
    i16 x, y; /**< axis values, value * 32767 (0 for buttons; y 0 for 1D axes) */
} de__qstate;

/** One chunk of a recording: an allocated block while recording, or a view into the loaded
 * file. */
typedef struct de__rec_chunk {
    u8 *data;
    u32 bytes;      /**< used bytes */
    u32 capacity;   /**< allocated bytes; 0 for a view into the loaded file */
    u32 tick_count; /**< records */
    u64 first_tick, last_tick;
} de__rec_chunk;

typedef void (*de__tick_override_fn)(de_input *in, u64 tick, void *user);

struct de_input_recording {
    de_allocator alloc;
    u32 mem_tag;
    de_input *in;
    bool loaded; /**< made by de_input_replay_load (replays; cannot record) */

    u64 schema_hash;
    u32 tick_rate;
    u32 action_count;
    u8 types[DE_ACTION_MAX_ACTIONS];

    u32 tick_count;
    u64 first_tick, last_tick;

    /* chunks[0 .. chunk_count) hold records. While recording, the descriptors after them may
     * hold spare preallocated blocks (data != NULL, bytes 0). */
    de__rec_chunk *chunks;
    u32 chunk_count;
    u32 chunk_capacity;
    u32 chunk_bytes; /**< capacity of a new chunk block */
    u32 max_record;  /**< worst-case bytes of one tick record for this schema */

    /* Recording: the state of the last record of the current chunk. */
    de__qstate prev[DE_ACTION_MAX_ACTIONS];

    /* Replay. */
    u8 *file; /**< the loaded file (owned), freed with file_alloc_size */
    size_t file_alloc_size;
    de__qstate cur[DE_ACTION_MAX_ACTIONS]; /**< decoder state at cur_tick */
    bool cur_valid;
    u64 cur_tick;
    u32 cur_chunk;                            /**< chunk of the next record */
    u32 cur_offset;                           /**< byte offset of the next record in that chunk */
    u32 cur_in_chunk;                         /**< records of that chunk decoded so far */
    de__qstate staged[DE_ACTION_MAX_ACTIONS]; /**< what the override applies */
    bool staged_valid;
    bool staged_applied; /**< applied once: triggers are not repeated */
    bool active;         /**< the replay override is installed (or saved under another) */
    de__tick_override_fn prev_override;
    void *prev_user;
};

/* ---------- Little-endian and varints ---------- */

static inline void de__le16(u8 *p, u16 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}
static inline void de__le32(u8 *p, u32 v) {
    for (u32 i = 0; i < 4u; ++i)
        p[i] = (u8)(v >> (8u * i));
}
static inline void de__le64(u8 *p, u64 v) {
    for (u32 i = 0; i < 8u; ++i)
        p[i] = (u8)(v >> (8u * i));
}
static inline u16 de__rd16(const u8 *p) {
    return (u16)(p[0] | (p[1] << 8));
}
static inline u32 de__rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static inline u64 de__rd64(const u8 *p) {
    u64 v = 0;
    for (u32 i = 0; i < 8u; ++i)
        v |= (u64)p[i] << (8u * i);
    return v;
}

/** Writes `v` as a varint at `p` (room for DE__VARINT_MAX bytes) and returns the bytes. */
static inline u32 de__varint_put(u8 *p, u64 v) {
    u32 n = 0;
    while (v >= 0x80u) {
        p[n++] = (u8)(v | 0x80u);
        v >>= 7;
    }
    p[n++] = (u8)v;
    return n;
}

/** Reads a varint from p[*off .. end). False (nothing consumed) when it runs past `end` or is
 * longer than 64 bits. */
static inline bool de__varint_get(const u8 *p, u32 end, u32 *off, u64 *out) {
    u64 v = 0;
    u32 o = *off;
    for (u32 i = 0; i < DE__VARINT_MAX; ++i) {
        if (o >= end)
            return false;
        const u8 b = p[o++];
        if (i == DE__VARINT_MAX - 1u && b > 1u)
            return false; /* more than 64 bits */
        v |= (u64)(b & 0x7Fu) << (7u * i);
        if ((b & 0x80u) == 0) {
            *off = o;
            *out = v;
            return true;
        }
    }
    return false;
}

/* ---------- Quantization (pack.c) ---------- */

/** round_half_away(clamp(v, -1, 1) * 32767), exact: the f64 product of an f32 and 32767 is
 * exact, and the rounding is done on its integer and fraction parts. NaN gives 0. */
i16 de__quantize_axis(f32 v);
/** q / 32767.0f (one correctly rounded f32 division). */
f32 de__dequantize_axis(i16 q);
/** The quantized form of tick state `s` of `act` (see de_input.h, "Recording / replay"). */
de__qstate de__quantize_state(const de__action *act, const de__action_state *s);
/** The axis `held` bit of `act` for quantized values: magnitude >= press_threshold. */
bool de__quantized_axis_held(const de__action *act, i16 x, i16 y);
/** The tick state of a quantized state (its flags as they are; values q / 32767). */
void de__dequantize_state(const de__action *act, const de__qstate *q, de__action_state *out);
/** The quantize-only tick override (identifies quantization in de__actions.tick_override). */
void de__input_quantize_override(de_input *in, u64 tick, void *user);

/* ---------- Schema (record.c) ---------- */

/** The schema hash of the actions of `in` now, and their types (`types` may be NULL; room for
 * DE_ACTION_MAX_ACTIONS). `*count` (may be NULL) gets the action count. 0 without actions. */
u64 de__input_schema(const de_input *in, u8 *types, u32 *count);
/** Worst-case bytes of one tick record for `count` actions of `types`. */
u32 de__record_max_bytes(const u8 *types, u32 count);
/** Bytes of the file of `rec`, and the file written to `out` (that many bytes). */
u64 de__recording_file_size(const de_input_recording *rec);
void de__recording_write_file(const de_input_recording *rec, u8 *out);

/* ---------- Replay (replay.c) ---------- */

/** The replay tick override (identifies a replay in de__actions.tick_override; user = the
 * recording). */
void de__input_replay_override(de_input *in, u64 tick, void *user);
/** Ends the replay of `rec` if it still runs: removes the replay override and restores the
 * one it replaced (if the replay override is still installed). */
void de__input_replay_end(de_input_recording *rec);
/**
 * Loads a recording from `size` bytes at `data` (copied), named `source` in error messages, for
 * replay on `in` (de_input_replay_load without the file; the fuzz target and tests use it).
 * Installs the replay override like de_input_replay_load. Errors as de_input_replay_load.
 */
de_result de__input_replay_load_mem(de_input *in, const void *data, u64 size, const char *source,
                                    de_input_recording **out);

DE_EXTERN_C_END
#endif
