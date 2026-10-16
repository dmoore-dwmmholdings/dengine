#include "de_fuzz.h"
#include "render/probes/probes_internal.h"
#include "render/formats/de_cooked_le.h"
#include <stdlib.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const void *payload = NULL;
    u32 resolution = 0;
    u64 settings = size >= 24 ? de_cooked_get_u64(data + 16) : 0;
    if (de__probe_blob_read(data, (u64)size, settings, &resolution, &payload) == DE_OK) {
        if (payload != data + DE__PROBE_BLOB_HEADER ||
            de__probe_payload_size(resolution) + DE__PROBE_BLOB_HEADER != size)
            abort();
        const volatile u8 *b = (const volatile u8 *)payload;
        (void)b[de__probe_payload_size(resolution) - 1];
    }
    return 0;
}
