#include "render/probes/probes_internal.h"
#include "render/formats/de_cooked_le.h"
#include <dengine/de_math.h>
#include <string.h>
static u64 bytes_hash(const void *bytes, u64 size) {
    const u8 *b = (const u8 *)bytes;
    u64 h = 14695981039346656037ull;
    for (u64 i = 0; i < size; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}
static void put64(u8 *p, u64 n) {
    de_cooked_put_u32(p, (u32)n);
    de_cooked_put_u32(p + 4, (u32)(n >> 32));
}
static u64 get64(const u8 *p) {
    return de_cooked_get_u32(p) | ((u64)de_cooked_get_u32(p + 4) << 32);
}
de_result de__probe_envelope_write(void *dst, u64 size, const char magic[4], u32 tag, u64 settings,
                                   const void *payload, u64 bytes) {
    if (!dst || !magic || !payload || size < DE__PROBE_BLOB_HEADER ||
        size - DE__PROBE_BLOB_HEADER != bytes)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "probe envelope: size");
    u8 *b = dst;
    memset(b, 0, DE__PROBE_BLOB_HEADER);
    memcpy(b, magic, 4);
    de_cooked_put_u32(b + 4, 1);
    de_cooked_put_u32(b + 8, DE__PROBE_BLOB_HEADER);
    de_cooked_put_u32(b + 12, tag);
    put64(b + 16, settings);
    put64(b + 24, bytes);
    put64(b + 32, bytes_hash(payload, bytes));
    memmove(b + DE__PROBE_BLOB_HEADER, payload, (size_t)bytes);
    return DE_OK;
}
de_result de__probe_envelope_read(const void *src, u64 size, const char magic[4], u64 settings,
                                  u32 *tag, const void **payload, u64 *bytes) {
    if (payload)
        *payload = NULL;
    if (!src || !magic || !tag || !payload || !bytes || size < DE__PROBE_BLOB_HEADER)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "probe envelope: truncated");
    const u8 *b = src;
    if (memcmp(b, magic, 4) || de_cooked_get_u32(b + 4) != 1 ||
        de_cooked_get_u32(b + 8) != DE__PROBE_BLOB_HEADER || get64(b + 16) != settings ||
        get64(b + 40) || get64(b + 24) != size - DE__PROBE_BLOB_HEADER ||
        bytes_hash(b + DE__PROBE_BLOB_HEADER, get64(b + 24)) != get64(b + 32))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "probe envelope: corrupt or stale");
    *tag = de_cooked_get_u32(b + 12);
    *bytes = get64(b + 24);
    *payload = b + DE__PROBE_BLOB_HEADER;
    return DE_OK;
}
de_result de__probe_blob_write(void *dst, u64 size, u64 settings, u32 resolution,
                               const void *payload) {
    if (resolution != 64 && resolution != 128 && resolution != 256)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "probe blob: resolution");
    return de__probe_envelope_write(dst, size, "DEPR", resolution, settings, payload,
                                    de__probe_payload_size(resolution));
}
de_result de__probe_blob_read(const void *src, u64 size, u64 settings, u32 *resolution,
                              const void **payload) {
    u64 bytes = 0;
    u32 tag = 0;
    if (!resolution || !payload)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "probe blob: arguments");
    DE_TRY(de__probe_envelope_read(src, size, "DEPR", settings, &tag, payload, &bytes));
    if ((tag != 64 && tag != 128 && tag != 256) || bytes != de__probe_payload_size(tag)) {
        *payload = NULL;
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "probe blob: layout");
    }
    *resolution = tag;
    return DE_OK;
}
u64 de__probe_settings_hash(const de__probe_snapshot *p) {
    /* Fieldwise encoding avoids compiler padding and runtime asset handles. */
    u8 b[100] = {0};
    u32 off = 0;
    const f64 position[3] = {p->position.x, p->position.y, p->position.z};
    for (u32 i = 0; i < 3; ++i) {
        u64 bits;
        memcpy(&bits, &position[i], 8);
        put64(b + off, bits);
        off += 8;
    }
    const f32 v[] = {p->rotation.x,     p->rotation.y,           p->rotation.z,
                     p->rotation.w,     p->probe.extent.x,       p->probe.extent.y,
                     p->probe.extent.z, p->probe.blend_distance, p->probe.intensity};
    for (u32 i = 0; i < sizeof v / sizeof v[0]; ++i) {
        u32 bits;
        memcpy(&bits, &v[i], 4);
        de_cooked_put_u32(b + off, bits);
        off += 4;
    }
    de_cooked_put_u32(b + off, p->probe.resolution ? p->probe.resolution : 128);
    off += 4;
    de_cooked_put_u32(b + off, (u32)p->probe.shape);
    off += 4;
    de_cooked_put_u32(b + off, p->probe.box_projection ? 1u : 0u);
    off += 4;
    return bytes_hash(b, off);
}
