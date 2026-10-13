/* pack.c: quantization of the tick view, the quantize tick override, the per-tick input checksum,
 * netcode packing and the per-player queries (DENG-89).
 *
 * SPEC §5.7: this file is part of the DETERMINISTIC module de_input (-ffp-contract=off). It uses
 * no <math.h>, iterates actions in declaration order (fixed arrays), and does its float work as
 * single IEEE operations: one exact f64 multiply per quantized value, one correctly rounded f32
 * division per dequantized value, and f32 multiply/add/compare for the derived `held`. */
#include "record_format.h"

#include <dengine/de_math.h>

#include <string.h>

/* ---------- Quantization ---------- */

i16 de__quantize_axis(f32 v) {
    if (v != v) /* NaN */
        return 0;
    if (v > 1.0f)
        v = 1.0f;
    if (v < -1.0f)
        v = -1.0f;
    /* An f32 has a 24-bit significand and 32767 has 15 bits, so the f64 product is exact. The
     * integer part (truncation toward zero) and the fraction (t - q) are exact too, so the
     * rounding below is exact round-half-away-from-zero on every platform. */
    const f64 t = (f64)v * (f64)DE_INPUT_AXIS_QUANT;
    i32 q = (i32)t;
    const f64 frac = t - (f64)q;
    if (frac >= 0.5)
        q += 1;
    else if (frac <= -0.5)
        q -= 1;
    return (i16)q;
}

f32 de__dequantize_axis(i16 q) {
    return (f32)q / (f32)DE_INPUT_AXIS_QUANT;
}

bool de__quantized_axis_held(const de__action *act, i16 x, i16 y) {
    const f32 fx = de__dequantize_axis(x);
    const f32 t = act->press_threshold;
    if (act->type == DE_ACTION_AXIS1D)
        return (fx < 0.0f ? -fx : fx) >= t;
    const f32 fy = de__dequantize_axis(y);
    const f32 m2 = fx * fx + fy * fy;
    return m2 >= t * t;
}

de__qstate de__quantize_state(const de__action *act, const de__action_state *s) {
    de__qstate q;
    memset(&q, 0, sizeof q);
    switch (act->type) {
    case DE_ACTION_BUTTON:
        q.flags = (u8)((s->held ? DE__Q_HELD : 0u) | (s->triggered ? DE__Q_TRIGGERED : 0u));
        break;
    case DE_ACTION_AXIS1D:
        q.x = de__quantize_axis(s->value.x);
        q.flags = de__quantized_axis_held(act, q.x, 0) ? (u8)DE__Q_HELD : (u8)0u;
        break;
    case DE_ACTION_AXIS2D:
    default:
        q.x = de__quantize_axis(s->value.x);
        q.y = de__quantize_axis(s->value.y);
        q.flags = de__quantized_axis_held(act, q.x, q.y) ? (u8)DE__Q_HELD : (u8)0u;
        break;
    }
    return q;
}

void de__dequantize_state(const de__action *act, const de__qstate *q, de__action_state *out) {
    out->held = (q->flags & DE__Q_HELD) != 0;
    out->triggered = (q->flags & DE__Q_TRIGGERED) != 0;
    if (act->type == DE_ACTION_BUTTON) {
        out->value.x = out->held ? 1.0f : 0.0f;
        out->value.y = 0.0f;
    } else {
        out->value.x = de__dequantize_axis(q->x);
        out->value.y = act->type == DE_ACTION_AXIS2D ? de__dequantize_axis(q->y) : 0.0f;
    }
}

void de__input_quantize_override(de_input *in, u64 tick, void *user) {
    (void)tick;
    (void)user;
    struct de__actions *s = de__actions_state(in);
    if (s == NULL)
        return;
    for (u32 i = 0; i < s->action_count; ++i) {
        de__action *act = &s->actions[i];
        const de__qstate q = de__quantize_state(act, &act->tick);
        de__dequantize_state(act, &q, &act->tick);
    }
}

de_result de_input_set_tick_quantization(de_input *in, bool on) {
    if (in == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_input_set_tick_quantization: in is NULL");
    struct de__actions *s = on ? de__actions_get_or_create(in) : de__actions_state(in);
    if (s == NULL)
        return on ? DE_ERR_OUT_OF_MEMORY : DE_OK;
    const de__tick_override_fn want = on ? de__input_quantize_override : NULL;
    /* While a replay runs (it quantizes anyway), the setting is the override that comes back
     * when it ends. */
    de_input_recording *rec = s->tick_override == de__input_replay_override
                                  ? (de_input_recording *)s->tick_override_user
                                  : NULL;
    const de__tick_override_fn cur = rec != NULL ? rec->prev_override : s->tick_override;
    if (cur != NULL && cur != de__input_quantize_override) {
        /* A foreign override (a game's, netcode's): quantization cannot be added on top of it,
         * and it is not quantization, so turning quantization off leaves it alone. */
        if (!on)
            return DE_OK;
        return DE_FAIL(DE_ERR_BUSY,
                       "de_input_set_tick_quantization: another tick override is installed");
    }
    if (rec != NULL) {
        rec->prev_override = want;
        rec->prev_user = NULL;
    } else {
        de__action_tick_override(in, want, NULL);
    }
    return DE_OK;
}

bool de_input_tick_quantization(const de_input *in) {
    const struct de__actions *s = de__actions_state_const(in);
    return s != NULL && (s->tick_override == de__input_quantize_override ||
                         s->tick_override == de__input_replay_override);
}

u64 de_input_tick_checksum(const de_input *in) {
    const struct de__actions *s = de__actions_state_const(in);
    if (s == NULL || s->action_count == 0)
        return 0;
    /* 6 bytes per action: type, flags, x, y (LE). One hash over the whole buffer. */
    u8 buf[DE_ACTION_MAX_ACTIONS * 6u];
    u32 n = 0;
    for (u32 i = 0; i < s->action_count; ++i) {
        const de__action *act = &s->actions[i];
        const de__qstate q = de__quantize_state(act, &act->tick);
        buf[n++] = (u8)act->type;
        buf[n++] = q.flags;
        de__le16(&buf[n], (u16)q.x);
        de__le16(&buf[n + 2u], (u16)q.y);
        n += 4u;
    }
    return de_hash64(buf, n, 0);
}

/* ---------- Packing ---------- */

static u32 type_bits(de_action_type t) {
    switch (t) {
    case DE_ACTION_BUTTON:
        return 2u;
    case DE_ACTION_AXIS1D:
        return 16u;
    case DE_ACTION_AXIS2D:
    default:
        return 32u;
    }
}

/** Bits of map index `mi` (0 for a map without actions). */
static u32 map_bits(const struct de__actions *s, u32 mi) {
    u32 bits = 0;
    for (u32 i = s->maps[mi].first_action; i != DE__ACTION_NONE; i = s->actions[i].next_in_map)
        bits += type_bits(s->actions[i].type);
    return bits;
}

u32 de_input_packed_size(const de_input *in, de_action_map m) {
    const u32 mi = de__action_map_index(in, m);
    if (mi == DE__ACTION_NONE)
        return 0;
    return (map_bits(de__actions_state_const(in), mi) + 7u) / 8u;
}

typedef struct bit_writer {
    u8 *p;
    u32 bit;
} bit_writer;

static void put_bits(bit_writer *w, u32 v, u32 n) {
    for (u32 i = 0; i < n; ++i, ++w->bit) {
        if ((v >> i) & 1u)
            w->p[w->bit >> 3] |= (u8)(1u << (w->bit & 7u));
    }
}

typedef struct bit_reader {
    const u8 *p;
    u32 bit;
} bit_reader;

static u32 get_bits(bit_reader *r, u32 n) {
    u32 v = 0;
    for (u32 i = 0; i < n; ++i, ++r->bit)
        v |= (u32)((r->p[r->bit >> 3] >> (r->bit & 7u)) & 1u) << i;
    return v;
}

u32 de_input_pack_actions(const de_input *in, de_action_map m, void *dst, u32 max_bytes) {
    const u32 mi = de__action_map_index(in, m);
    if (mi == DE__ACTION_NONE || dst == NULL)
        return 0;
    const struct de__actions *s = de__actions_state_const(in);
    const u32 bytes = (map_bits(s, mi) + 7u) / 8u;
    if (bytes == 0 || bytes > max_bytes)
        return 0;
    bit_writer w = {(u8 *)dst, 0};
    memset(dst, 0, bytes);
    for (u32 i = s->maps[mi].first_action; i != DE__ACTION_NONE; i = s->actions[i].next_in_map) {
        const de__action *act = &s->actions[i];
        const de__qstate q = de__quantize_state(act, &act->tick);
        switch (act->type) {
        case DE_ACTION_BUTTON:
            put_bits(&w, q.flags & (DE__Q_HELD | DE__Q_TRIGGERED), 2u);
            break;
        case DE_ACTION_AXIS1D:
            put_bits(&w, (u16)q.x, 16u);
            break;
        case DE_ACTION_AXIS2D:
        default:
            put_bits(&w, (u16)q.x, 16u);
            put_bits(&w, (u16)q.y, 16u);
            break;
        }
    }
    return bytes;
}

bool de_input_unpack_actions(de_input *in, de_action_map m, u32 player, const void *src,
                             u32 bytes) {
    const u32 mi = de__action_map_index(in, m);
    if (mi == DE__ACTION_NONE || src == NULL || player >= DE_INPUT_MAX_PLAYERS)
        return false;
    struct de__actions *s = de__actions_state(in);
    const u32 bits = map_bits(s, mi);
    if (bits == 0 || bytes != (bits + 7u) / 8u)
        return false;
    const u8 *p = (const u8 *)src;
    if (bits % 8u != 0 && (p[bytes - 1u] >> (bits % 8u)) != 0)
        return false; /* padding bits set: not a stream of this map */
    /* Check every value before changing anything. */
    bit_reader r = {p, 0};
    for (u32 i = s->maps[mi].first_action; i != DE__ACTION_NONE; i = s->actions[i].next_in_map) {
        if (s->actions[i].type == DE_ACTION_BUTTON) {
            (void)get_bits(&r, 2u); /* every combination of held and triggered is valid */
            continue;
        }
        const u32 values = s->actions[i].type == DE_ACTION_AXIS2D ? 2u : 1u;
        for (u32 k = 0; k < values; ++k) {
            if (get_bits(&r, 16u) == 0x8000u)
                return false; /* -32768 is outside the quantized range */
        }
    }
    r.bit = 0;
    for (u32 i = s->maps[mi].first_action; i != DE__ACTION_NONE; i = s->actions[i].next_in_map) {
        de__action *act = &s->actions[i];
        de__qstate q;
        memset(&q, 0, sizeof q);
        if (act->type == DE_ACTION_BUTTON) {
            q.flags = (u8)get_bits(&r, 2u);
        } else {
            q.x = (i16)(u16)get_bits(&r, 16u);
            if (act->type == DE_ACTION_AXIS2D)
                q.y = (i16)(u16)get_bits(&r, 16u);
            q.flags = de__quantized_axis_held(act, q.x, q.y) ? (u8)DE__Q_HELD : (u8)0u;
        }
        de__action_state *dst = player == 0 ? &act->tick : &s->players[player - 1u][i];
        de__dequantize_state(act, &q, dst);
    }
    return true;
}

/* ---------- Per-player queries ---------- */

/** The slot state of player >= 1, or NULL. */
static const de__action_state *player_state(const de_input *in, de_action a, u32 player,
                                            de_action_type *type) {
    if (player == 0 || player >= DE_INPUT_MAX_PLAYERS)
        return NULL;
    const u32 i = de__action_index(in, a);
    if (i == DE__ACTION_NONE)
        return NULL;
    const struct de__actions *s = de__actions_state_const(in);
    if (type != NULL)
        *type = s->actions[i].type;
    return &s->players[player - 1u][i];
}

bool de_action_triggered_p(const de_input *in, de_action a, u32 player) {
    if (player == 0)
        return de_action_triggered(in, a);
    const de__action_state *st = player_state(in, a, player, NULL);
    return st != NULL && st->triggered;
}

bool de_action_held_p(const de_input *in, de_action a, u32 player) {
    if (player == 0)
        return de_action_held(in, a);
    const de__action_state *st = player_state(in, a, player, NULL);
    return st != NULL && st->held;
}

f32 de_action_value_p(const de_input *in, de_action a, u32 player) {
    if (player == 0)
        return de_action_value(in, a);
    de_action_type type = DE_ACTION_BUTTON;
    const de__action_state *st = player_state(in, a, player, &type);
    if (st == NULL)
        return 0.0f;
    if (type == DE_ACTION_AXIS2D)
        return de_dsqrt(st->value.x * st->value.x + st->value.y * st->value.y);
    return st->value.x;
}

de_vec2 de_action_value2_p(const de_input *in, de_action a, u32 player) {
    if (player == 0)
        return de_action_value2(in, a);
    const de__action_state *st = player_state(in, a, player, NULL);
    if (st == NULL) {
        const de_vec2 zero = {0.0f, 0.0f};
        return zero;
    }
    return st->value;
}
