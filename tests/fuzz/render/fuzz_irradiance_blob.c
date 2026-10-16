#include "de_fuzz.h"
#include "render/probes/irradiance_internal.h"
#include "render/formats/de_cooked_le.h"
#include <stdlib.h>
#include <string.h>
static void exercise(const u8 *data, size_t size) {
    const void *payload = NULL;
    de__gi_layout l;
    u64 hash = size >= 24 ? de_cooked_get_u64(data + 16) : 0;
    if (de__gi_blob_read(data, (u64)size, hash, &l, &payload) != DE_OK)
        return;
    if (payload != data + DE__GI_BLOB_HEADER || l.payload_bytes + DE__GI_BLOB_HEADER != size ||
        !l.probes || l.probes > DE__GI_PROBES)
        abort();
    u8 *out = malloc(size);
    if (!out)
        abort();
    if (de__gi_blob_write(out, (u64)size, hash, &l, payload) != DE_OK || memcmp(out, data, size))
        abort();
    free(out);
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    exercise(data, size);
    // Repair only the checksum of mutated candidates so hostile metadata/texels reach semantic
    // validation, rather than every mutation stopping at the envelope's integrity check.
    if (size >= DE__GI_BLOB_HEADER && size <= 8u * 1024 * 1024) {
        u8 *candidate = malloc(size);
        if (!candidate)
            return 0;
        memcpy(candidate, data, size);
        u64 hash = 14695981039346656037ull;
        for (size_t i = 48; i < size; ++i) {
            hash ^= candidate[i];
            hash *= 1099511628211ull;
        }
        de_cooked_put_u64(candidate + 32, hash);
        exercise(candidate, size);
        free(candidate);
    }
    return 0;
}
