/* glyphs.c: prompt glyph lookup per device family (DENG-88, SPEC §15, SDL3 path).
 *
 * action -> binding -> glyph ID. The family comes from the last used device
 * (de__glyphs_frame_update, once per frame); the binding is the first one of the family's device
 * class, read through the iteration API of action_internal.h; the ID is a static string of
 * glyph_table.inc. Nothing allocates, and the lookup only reads the input state, so it is safe
 * wherever the const queries are. */
#include "action_internal.h"
#include "input_internal.h"

#include <string.h>

#include "glyph_table.inc"

static const char *const k_family_names[DE_GLYPH_FAMILY_COUNT_] = {
    [DE_GLYPH_FAMILY_KBM] = "kbm",         [DE_GLYPH_FAMILY_XBOX] = "xbox",
    [DE_GLYPH_FAMILY_PS] = "ps",           [DE_GLYPH_FAMILY_PS5] = "ps5",
    [DE_GLYPH_FAMILY_SWITCH] = "switch",   [DE_GLYPH_FAMILY_DECK] = "deck",
    [DE_GLYPH_FAMILY_GENERIC] = "generic",
};

const char *de_glyph_family_name(de_glyph_family f) {
    return (u32)f < (u32)DE_GLYPH_FAMILY_COUNT_ ? k_family_names[f] : NULL;
}

/* ---------- Family ---------- */

static de_glyph_family family_of_pad(const de__input_pad *p) {
    switch (p->info.type) {
    case DE_PAD_TYPE_XBOX:
        return DE_GLYPH_FAMILY_XBOX;
    case DE_PAD_TYPE_PS4:
        return DE_GLYPH_FAMILY_PS;
    case DE_PAD_TYPE_PS5:
        return DE_GLYPH_FAMILY_PS5;
    case DE_PAD_TYPE_SWITCH:
        return DE_GLYPH_FAMILY_SWITCH;
    case DE_PAD_TYPE_STEAM:
        return DE_GLYPH_FAMILY_DECK;
    default:
        return DE_GLYPH_FAMILY_GENERIC;
    }
}

/* The first connected pad (connection order), or NULL. */
static const de__input_pad *first_pad(const de_input *in) {
    return in->pad_count > 0 ? &in->pads[in->pad_order[0]] : NULL;
}

static de_glyph_family family_from_state(const de_input *in) {
    if (in->last_device == DE_INPUT_DEVICE_KEYBOARD_MOUSE)
        return DE_GLYPH_FAMILY_KBM;
    if (in->last_device == DE_INPUT_DEVICE_GAMEPAD) {
        const de__input_pad *p = de__input_pad_get_const(in, in->last_pad);
        if (p != NULL)
            return family_of_pad(p);
    }
    /* Nothing used yet, or the last used pad is gone: a connected pad is the likely device (a
     * Steam Deck before the first input), else keyboard and mouse. */
    const de__input_pad *p = first_pad(in);
    return p != NULL ? family_of_pad(p) : DE_GLYPH_FAMILY_KBM;
}

void de__glyphs_frame_update(de_input *in) {
    const de_glyph_family f = family_from_state(in);
    in->glyph_family_changed = f != in->glyph_family;
    in->glyph_family = f;
}

de_glyph_family de_input_glyph_family(const de_input *in) {
    return in != NULL ? in->glyph_family : DE_GLYPH_FAMILY_KBM;
}

bool de_input_glyph_family_changed(const de_input *in) {
    return in != NULL && in->glyph_family_changed;
}

/* ---------- Bindings to IDs ---------- */

static bool is_kbm_source(de_binding_source s) {
    return s == DE_BIND_KEY || s == DE_BIND_MOUSE_BUTTON || s == DE_BIND_MOUSE_DELTA ||
           s == DE_BIND_MOUSE_WHEEL;
}

static bool in_composite(const de__action *act, const de_binding *b) {
    /* Button actions ignore composite_slot (a zeroed binding has slot 0). */
    return act->type != DE_ACTION_BUTTON && b->composite_slot >= 0;
}

/* The direction a 1D axis feeds into a composite slot: a slot reads max(value, 0) after the
 * modifiers, so it is the axis' positive side, flipped by invert and by a negative scale. */
static bool feeds_positive(const de_binding *b) {
    return b->invert == (b->scale < 0.0f);
}

/* The ID of one binding for pad family `f` (a pad family; KBM bindings ignore it). */
static const char *binding_id(const de__action *act, const de_binding *b, de_glyph_family f) {
    const u32 c = b->code;
    switch (b->source) {
    case DE_BIND_KEY: {
        const char *id = c < (u32)DE_KEY_COUNT_ ? k_glyph_key[c] : NULL;
        return id != NULL ? id : GLYPH_ID_KEY_UNKNOWN;
    }
    case DE_BIND_MOUSE_BUTTON:
        return c < (u32)DE_MOUSE_BUTTON_COUNT_ ? k_glyph_mouse_button[c] : NULL;
    case DE_BIND_MOUSE_DELTA:
        return c <= DE_BIND_AXIS_XY ? k_glyph_mouse_move[c] : NULL;
    case DE_BIND_MOUSE_WHEEL:
        if (c == DE_BIND_AXIS_Y && in_composite(act, b))
            return k_glyph_wheel_dir[feeds_positive(b) ? 0 : 1];
        return c <= DE_BIND_AXIS_XY ? k_glyph_mouse_wheel[c] : NULL;
    case DE_BIND_PAD_BUTTON:
        return c < (u32)DE_PAD_BUTTON_COUNT_ ? k_glyph_pad_button[f][c] : NULL;
    case DE_BIND_PAD_AXIS:
        if (c <= DE_AXIS_RIGHT_Y && in_composite(act, b)) {
            const u32 stick = c >= DE_AXIS_RIGHT_X ? 1u : 0u;
            const bool y = c == DE_AXIS_LEFT_Y || c == DE_AXIS_RIGHT_Y;
            const bool pos = feeds_positive(b);
            /* Directions: 0 up, 1 down, 2 left, 3 right. */
            const u32 dir = y ? (pos ? 0u : 1u) : (pos ? 3u : 2u);
            return k_glyph_stick_dir[f][stick][dir];
        }
        return c < GLYPH_PAD_AXIS_CODES ? k_glyph_pad_axis[f][c] : NULL;
    case DE_BIND_GYRO:
        return c <= DE_BIND_AXIS_XY ? k_glyph_gyro[c] : NULL;
    default:
        return NULL;
    }
}

/* True when a composite binding of class `kbm` in slot `slot` has source `src` and code `code`. */
static bool slot_has(const de_input *in, de_action a, u32 count, bool kbm, i8 slot,
                     de_binding_source src, u32 code) {
    for (u32 i = 0; i < count; ++i) {
        de_binding b;
        if (de__action_get_binding(in, a, i, &b) && is_kbm_source(b.source) == kbm &&
            b.composite_slot == slot && b.source == src && b.code == code)
            return true;
    }
    return false;
}

static bool slots_are(const de_input *in, de_action a, u32 count, bool kbm, de_binding_source src,
                      u32 up, u32 down, u32 left, u32 right) {
    return slot_has(in, a, count, kbm, DE_COMPOSITE_UP, src, up) &&
           slot_has(in, a, count, kbm, DE_COMPOSITE_DOWN, src, down) &&
           slot_has(in, a, count, kbm, DE_COMPOSITE_LEFT, src, left) &&
           slot_has(in, a, count, kbm, DE_COMPOSITE_RIGHT, src, right);
}

/* The ID of a whole 2D composite of class `kbm`, or NULL when its slots form no known set. */
static const char *composite_id(const de_input *in, de_action a, u32 count, bool kbm,
                                de_glyph_family f) {
    if (kbm) {
        if (slots_are(in, a, count, true, DE_BIND_KEY, DE_KEY_W, DE_KEY_S, DE_KEY_A, DE_KEY_D))
            return GLYPH_ID_WASD;
        if (slots_are(in, a, count, true, DE_BIND_KEY, DE_KEY_UP, DE_KEY_DOWN, DE_KEY_LEFT,
                      DE_KEY_RIGHT))
            return GLYPH_ID_ARROWS;
        return NULL;
    }
    if (slots_are(in, a, count, false, DE_BIND_PAD_BUTTON, DE_PAD_DPAD_UP, DE_PAD_DPAD_DOWN,
                  DE_PAD_DPAD_LEFT, DE_PAD_DPAD_RIGHT))
        return k_glyph_dpad[f];
    return NULL;
}

/* The ID of the first binding of class `kbm` (pad IDs in family `pad_family`), or NULL. */
static const char *class_id(const de_input *in, de_action a, const de__action *act, bool kbm,
                            de_glyph_family pad_family) {
    const u32 count = de__action_binding_count(in, a);
    for (u32 i = 0; i < count; ++i) {
        de_binding b;
        if (!de__action_get_binding(in, a, i, &b) || is_kbm_source(b.source) != kbm)
            continue;
        if (act->type == DE_ACTION_AXIS2D && in_composite(act, &b)) {
            const char *whole = composite_id(in, a, count, kbm, pad_family);
            if (whole != NULL)
                return whole;
        }
        const char *id = binding_id(act, &b, pad_family);
        if (id != NULL)
            return id;
    }
    return NULL;
}

const char *de_action_glyph_id_family(const de_input *in, de_action a, de_glyph_family f) {
    if (in == NULL)
        return DE_GLYPH_ID_UNBOUND;
    if ((u32)f >= (u32)DE_GLYPH_FAMILY_COUNT_)
        f = DE_GLYPH_FAMILY_GENERIC;
    if (in->glyph_provider != NULL) {
        const char *id = in->glyph_provider(in, a, f, in->glyph_provider_user);
        if (id != NULL)
            return id;
    }
    const de__action *act = de__action_get_const(in, a);
    if (act == NULL)
        return DE_GLYPH_ID_UNBOUND;

    const bool kbm = f == DE_GLYPH_FAMILY_KBM;
    /* For the fallback from KBM to pad bindings: the family of the first connected pad. */
    const de__input_pad *p = first_pad(in);
    const de_glyph_family pad_family =
        kbm ? (p != NULL ? family_of_pad(p) : DE_GLYPH_FAMILY_GENERIC) : f;

    const char *id = class_id(in, a, act, kbm, pad_family);
    if (id == NULL)
        id = class_id(in, a, act, !kbm, pad_family);
    return id != NULL ? id : DE_GLYPH_ID_UNBOUND;
}

const char *de_action_glyph_id(const de_input *in, de_action a) {
    return de_action_glyph_id_family(in, a, de_input_glyph_family(in));
}

/* ---------- Provider, key labels ---------- */

void de_input_set_glyph_provider(de_input *in, de_glyph_provider_fn fn, void *user) {
    if (in == NULL)
        return;
    in->glyph_provider = fn;
    in->glyph_provider_user = fn != NULL ? user : NULL;
}

void de_input_set_key_label_fn(de_input *in, de_key_label_fn fn, void *user) {
    if (in == NULL)
        return;
    in->key_label_fn = fn;
    in->key_label_user = fn != NULL ? user : NULL;
}

const char *de_input_key_label(const de_input *in, de_key k) {
    if (in != NULL && in->key_label_fn != NULL) {
        const char *label = in->key_label_fn(k, in->key_label_user);
        if (label != NULL)
            return label;
    }
    return de_key_name(k);
}

#define GLYPH_KEY_PREFIX     "kbm/key_"
#define GLYPH_KEY_PREFIX_LEN (sizeof GLYPH_KEY_PREFIX - 1u)

de_key de_glyph_id_key(const char *id) {
    if (id == NULL || strncmp(id, GLYPH_KEY_PREFIX, GLYPH_KEY_PREFIX_LEN) != 0)
        return DE_KEY_UNKNOWN;
    /* de_key_from_name ignores ASCII case; glyph IDs are lower case, so check the exact ID. */
    const de_key k = de_key_from_name(id + GLYPH_KEY_PREFIX_LEN);
    return k != DE_KEY_UNKNOWN && k_glyph_key[k] != NULL && strcmp(k_glyph_key[k], id) == 0
               ? k
               : DE_KEY_UNKNOWN;
}

/* ---------- Table listing (tests) ---------- */

typedef struct id_list {
    const char **out;
    u32 max, count;
} id_list;

static void list_add(id_list *l, const char *id) {
    if (id == NULL)
        return;
    if (l->out != NULL && l->count < l->max)
        l->out[l->count] = id;
    l->count++;
}

static void list_table(id_list *l, const char *const *t, u32 n) {
    for (u32 i = 0; i < n; ++i)
        list_add(l, t[i]);
}

u32 de__glyph_table_ids(const char **out, u32 max) {
    id_list l = {out, max, 0};
    for (u32 f = 0; f < (u32)DE_GLYPH_FAMILY_COUNT_; ++f) {
        list_table(&l, k_glyph_pad_button[f], DE_PAD_BUTTON_COUNT_);
        list_table(&l, k_glyph_pad_axis[f], GLYPH_PAD_AXIS_CODES);
        list_table(&l, k_glyph_stick_dir[f][0], 4);
        list_table(&l, k_glyph_stick_dir[f][1], 4);
        list_add(&l, k_glyph_dpad[f]);
    }
    list_table(&l, k_glyph_gyro, DE_BIND_AXIS_XY + 1u);
    list_table(&l, k_glyph_mouse_button, DE_MOUSE_BUTTON_COUNT_);
    list_table(&l, k_glyph_mouse_move, DE_BIND_AXIS_XY + 1u);
    list_table(&l, k_glyph_mouse_wheel, DE_BIND_AXIS_XY + 1u);
    list_table(&l, k_glyph_wheel_dir, 2);
    list_add(&l, GLYPH_ID_WASD);
    list_add(&l, GLYPH_ID_ARROWS);
    list_add(&l, GLYPH_ID_KEY_UNKNOWN);
    list_table(&l, k_glyph_key, DE_KEY_COUNT_);
    list_add(&l, DE_GLYPH_ID_UNBOUND);
    return l.count;
}
