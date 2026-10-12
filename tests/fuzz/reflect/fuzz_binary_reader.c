/*
 * fuzz_binary_reader: the binary reader of the reflection serializer (DENG-296) on untrusted
 * bytes (DENG-297; SPEC §19.1: cooked scenes, prefabs and save games).
 *
 * The input is either
 *   - a stream of binary objects as written ("DEBO" first; e.g. the golden file and the unit
 *     test outputs in the corpus), self-describing or compact: read as it is; or
 *   - anything else: byte 0 picks a type, the schema mode and the schema hash, byte 1 the
 *     writer's required_flags, and the rest becomes the schema block and payload behind a valid
 *     header that this target builds. That reaches the payload decoder and the schema parser
 *     without the fuzzer having to find a 64-bit schema hash first.
 *
 * Each input is read as every test type of the "current" and the "next" registry
 * (fuzz_reflect.h): up to FZ_MAX_OBJECTS objects in a row, through de_deserialize_binary with
 * stats, through de_deserialize_object(DE_SER_BINARY), with and without a de_ser_context, with
 * and without a required_flags filter, the fast path on or off, and a reader that returns uneven
 * chunks. Every object that loads must re-encode stably as binary and as text.
 */
#include "fuzz_reflect.h"

#define FZ_MAX_OBJECTS 4u

static const u32 k_req[] = {0, DE_FIELD_SERIALIZE, DE_FIELD_REPLICATE, DE_FIELD_SNAPSHOT};

static void put_u16(u8 *p, u16 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}
static void put_u32(u8 *p, u32 v) {
    for (u32 i = 0; i < 4; ++i)
        p[i] = (u8)(v >> (8 * i));
}
static void put_u64(u8 *p, u64 v) {
    for (u32 i = 0; i < 8; ++i)
        p[i] = (u8)(v >> (8 * i));
}

/* Builds header + rest for a non-"DEBO" input (see the file comment). Returns a malloc'd buffer
 * the caller frees, or NULL when the input is too short. */
static u8 *synthesize(const fz_world *wd, const u8 *data, size_t size, size_t *out_size) {
    if (size < 2)
        return NULL;
    u8 sel = data[0];
    u32 v = (sel >> 6) & 1u;
    const de_type_registry *r = wd->reg[v];
    de_type_id id = wd->types[v][(sel & 0x7u) % FZ_TYPE_COUNT];
    const de_type_desc *d = de_type_get(r, id);
    bool self = (sel & 0x08u) != 0;
    bool other_hash = (sel & 0x10u) != 0;
    bool newer = (sel & 0xA0u) == 0xA0u;
    const u8 *rest = data + 2;
    size_t rest_size = size - 2;

    u32 payload = (u32)rest_size;
    if (self) {
        /* The schema block (u32 size + bytes) comes first; the payload is what follows it. */
        u32 block = 0;
        if (rest_size >= 4)
            block =
                (u32)rest[0] | ((u32)rest[1] << 8) | ((u32)rest[2] << 16) | ((u32)rest[3] << 24);
        u64 used = 4u + (u64)block;
        payload = used >= rest_size ? 0u : (u32)(rest_size - used);
    }
    u8 *buf = (u8 *)malloc(32u + rest_size);
    fz_check(buf != NULL, "out of memory");
    memcpy(buf, "DEBO", 4);
    put_u16(buf + 4, 1);
    put_u16(buf + 6, (u16)((self ? 1u : 0u) | ((u32)data[1] << 8)));
    put_u64(buf + 8, de_hash64_str(d->name));
    put_u32(buf + 16, d->version + (newer ? 1u : 0u));
    put_u64(buf + 20, other_hash ? 0x0123456789abcdefull : de_type_schema_hash(r, id));
    put_u32(buf + 28, payload);
    if (rest_size > 0)
        memcpy(buf + 32, rest, rest_size);
    *out_size = 32u + rest_size;
    return buf;
}

static void read_stream(const fz_world *wd, u32 v, u32 t, const u8 *data, size_t size,
                        u32 variant) {
    const de_type_registry *r = wd->reg[v];
    de_type_id id = wd->types[v][t];
    de_ser_options o = {DE_SER_BINARY, k_req[variant % 4u], (variant & 1u) ? &g_fz_ctx : NULL};
    de_ser_binary_stats stats;
    memset(&stats, 0, sizeof stats);
    de_ser_binary_options bo = {DE_SER_BINARY_SCHEMA_DEFAULT, &stats, (variant & 2u) != 0};
    bool object_api = variant % 5u == 3u;

    fz_reader fr;
    de_reader rd = fz_reader_make(&fr, data, size, variant % 3u == 0 ? 0u : 1u + variant % 11u);
    for (u32 n = 0; n < FZ_MAX_OBJECTS && fr.pos < fr.size; ++n) {
        _Alignas(16) u8 obj[FZ_OBJ_MAX];
        fz_check(de_type_construct(r, id, obj) == DE_OK, "construct");
        size_t before = fr.pos;
        de_result res = object_api ? de_deserialize_object(r, id, obj, &o, &rd)
                                   : de_deserialize_binary(r, id, obj, &o, &bo, &rd);
        fz_check(fr.pos >= before && fr.pos <= fr.size, "the reader position moved backwards");
        if (res == DE_OK) {
            fz_binary_stable(r, id, obj, &o, DE_SER_BINARY_SELF_DESCRIBING);
            fz_binary_stable(r, id, obj, &o, DE_SER_BINARY_COMPACT);
            fz_text_stable(r, id, obj, &o);
        }
        de_error_clear();
        /* After DE_OK and DE_ERR_VERSION_MISMATCH the stream stands after the object (header
         * documentation); after other errors the position is unspecified: stop. */
        if (res != DE_OK && res != DE_ERR_VERSION_MISMATCH)
            break;
    }
    fz_check(stats.objects_read <= FZ_MAX_OBJECTS, "stats count more objects than were read");
    fz_check(stats.fast_path + stats.field_wise + stats.schema_matched == stats.objects_read,
             "every object read is counted on exactly one path");
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const fz_world *wd = fz_world_get();
    const u8 *in = data;
    size_t in_size = size;
    u8 *owned = NULL;
    if (size < 4 || memcmp(data, "DEBO", 4) != 0) {
        owned = synthesize(wd, data, size, &in_size);
        if (owned == NULL)
            return 0;
        in = owned;
    }
    for (u32 v = 0; v < 2; ++v)
        for (u32 t = 0; t < FZ_TYPE_COUNT; ++t)
            read_stream(wd, v, t, in, in_size, (u32)(size + v * 5u + t));
    free(owned);
    return 0;
}
