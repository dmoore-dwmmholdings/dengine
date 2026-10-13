/*
 * fuzz_replay_load: the .deir loader (de_input_replay_load's parser, DENG-89) on untrusted bytes.
 *
 * Each input runs twice against a fresh input state with a three-action schema (one action of
 * each type, so every record payload kind is decoded):
 *   1. as it is: header, checksum, schema and body checks;
 *   2. "fixed up": the magic, version, flags, schema hash, action count and type table are made
 *      valid and the trailing checksum is recomputed, so the fuzzer reaches the chunk and record
 *      decoder instead of stopping at the checksum.
 * A load that succeeds is replayed: every chunk's first tick, the ticks after it, a backward
 * seek and the end. Checked besides "no crash, no sanitizer report":
 *   - a failure is one of the documented results, sets the error context, returns no recording
 *     and installs no tick override;
 *   - a success saves back to the same bytes (de_input_recording_checksum equals the trailer).
 * A broken check aborts.
 */
#include "de_fuzz.h"

#include "input/record_format.h"

#include <dengine/de_input.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_ACTIONS 3u

static void check(int ok, const char *what) {
    if (ok)
        return;
    fprintf(stderr, "fuzz_replay_load: %s\n", what);
    abort();
}

static de_input *make_input(void) {
    de_input_desc d;
    memset(&d, 0, sizeof d);
    de_input *in = NULL;
    check(de_input_create(&d, &in) == DE_OK, "de_input_create");
    const de_action_map m = de_action_map_create(in, "fuzz");
    static const de_action_type types[FUZZ_ACTIONS] = {DE_ACTION_BUTTON, DE_ACTION_AXIS1D,
                                                       DE_ACTION_AXIS2D};
    static const char *const names[FUZZ_ACTIONS] = {"jump", "throttle", "move"};
    for (u32 i = 0; i < FUZZ_ACTIONS; ++i) {
        de_binding b;
        memset(&b, 0, sizeof b);
        b.composite_slot = DE_COMPOSITE_NONE;
        b.source = i == 0 ? DE_BIND_KEY : DE_BIND_PAD_AXIS;
        b.code =
            i == 0 ? (u32)DE_KEY_SPACE : (i == 1 ? (u32)DE_AXIS_LEFT_TRIGGER : DE_BIND_LEFT_STICK);
        de_action_desc ad;
        memset(&ad, 0, sizeof ad);
        ad.name = names[i];
        ad.type = types[i];
        ad.bindings = &b;
        ad.binding_count = 1;
        check(DE_HANDLE_VALID(de_action_add(in, m, &ad)), "de_action_add");
    }
    de_action_map_push(in, m);
    return in;
}

static void replay(de_input *in, de_input_recording *rec, const u8 *file, u64 size) {
    check(de_input_tick_quantization(in), "replay installed no override");
    check(de_input_recording_checksum(rec) == de__rd64(file + size - DE__DEIR_TRAILER),
          "save does not give back the loaded bytes");
    const u32 chunks = rec->chunk_count;
    for (u32 k = 0; k < chunks; ++k) {
        const u64 first = rec->chunks[k].first_tick;
        for (u64 t = first; t < first + 8u && t >= first; ++t) {
            if (!de_input_replay_tick(in, rec, t))
                return; /* ended */
            de__actions_fixed_tick(in, t);
            (void)de_input_tick_checksum(in);
        }
    }
    if (rec->tick_count != 0) {
        (void)de_input_replay_tick(in, rec, rec->first_tick); /* backward seek */
        de__actions_fixed_tick(in, 0);
        check(!de_input_replay_tick(in, rec, rec->last_tick + 1u) || rec->last_tick == ~(u64)0,
              "replay did not end after the last tick");
    }
}

static void load_once(const u8 *data, u64 size) {
    de_input *in = make_input();
    de_input_recording *rec = (de_input_recording *)(uintptr_t)1;
    const de_result r = de__input_replay_load_mem(in, data, size, "fuzz", &rec);
    if (r == DE_OK) {
        check(rec != NULL, "DE_OK without a recording");
        replay(in, rec, data, size);
        de_input_recording_destroy(rec);
        check(!de_input_tick_quantization(in), "destroy left the replay override");
    } else {
        check(rec == NULL, "a failure returned a recording");
        check(r == DE_ERR_PARSE || r == DE_ERR_VERSION_MISMATCH || r == DE_ERR_INVALID_ARGUMENT,
              "unexpected result");
        const de_error *e = de_error_last();
        check(e != NULL && e->result == r, "failure without error context");
        check(!de_input_tick_quantization(in), "a failed load installed an override");
        de_error_clear();
    }
    de_input_destroy(in);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > (1u << 20))
        return 0;
    load_once(data, size);

    /* Fixed up: valid header identity and checksum around the fuzzed counts and body. */
    const u64 head = DE__DEIR_HEADER_FIXED + FUZZ_ACTIONS;
    const u64 n = size < head + DE__DEIR_TRAILER ? head + DE__DEIR_TRAILER : (u64)size;
    u8 *buf = (u8 *)calloc(1, (size_t)n);
    check(buf != NULL, "calloc");
    if (size != 0)
        memcpy(buf, data, size);
    de_input *in = make_input();
    memcpy(buf, "DEIR", 4);
    de__le16(buf + 4, (u16)DE__DEIR_VERSION);
    de__le16(buf + 6, 0);
    de__le64(buf + 8, de_input_schema_hash(in));
    de__le32(buf + 20, FUZZ_ACTIONS);
    buf[DE__DEIR_HEADER_FIXED + 0] = (u8)DE_ACTION_BUTTON;
    buf[DE__DEIR_HEADER_FIXED + 1] = (u8)DE_ACTION_AXIS1D;
    buf[DE__DEIR_HEADER_FIXED + 2] = (u8)DE_ACTION_AXIS2D;
    de__le64(buf + n - DE__DEIR_TRAILER, de_hash64(buf, (size_t)(n - DE__DEIR_TRAILER), 0));
    de_input_destroy(in);
    load_once(buf, n);
    free(buf);
    return 0;
}
