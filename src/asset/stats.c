/*
 * stats.c: de_asset_get_stats (SPEC §11.8; DENG-144): resident bytes per category and assets
 * (budget.c accounting), loads in flight (the load state's per-slot records) and the I/O rate
 * (sampled by de_asset_system_update).
 */

#include "asset/asset_internal.h"
#include "asset/budget_internal.h"
#include "asset/load_internal.h"
#include "asset/slots_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

#include "platform/de_atomic.h"

#include <string.h>

/* Slots submitted to the backend and not finished: the per-slot load records in flight. */
static u32 stats_loads_in_flight(const de_asset_system *s) {
    const de__load_state *L = s->load;
    if (L == NULL)
        return 0;
    u32 n = 0;
    for (u32 p = 0; p < DE__SLOT_MAX_PAGES; ++p) {
        const de__load_slot *page = (const de__load_slot *)de_atomic_load_ptr(
            (de_atomic_ptr *)&L->slot_pages[p], DE_ATOMIC_ACQUIRE);
        if (page == NULL)
            continue;
        for (u32 i = 0; i < DE__SLOT_PAGE_SLOTS; ++i)
            n += page[i].gen != 0 && page[i].in_flight ? 1u : 0u;
    }
    return n;
}

de_asset_stats de_asset_get_stats(const de_asset_system *s) {
    de_asset_stats st;
    memset(&st, 0, sizeof st);
    if (s == NULL)
        return st;
    const de__budget *b = s->budget;
    if (b != NULL) {
        st.bytes_resident[DE_ASSET_BYTES_GPU_TEXTURE] = b->bytes[DE_ASSET_CAT_GPU_TEXTURE];
        st.bytes_resident[DE_ASSET_BYTES_GPU_MESH] = b->bytes[DE_ASSET_CAT_GPU_MESH];
        st.bytes_resident[DE_ASSET_BYTES_AUDIO] = b->bytes[DE_ASSET_CAT_AUDIO];
        st.bytes_resident[DE_ASSET_BYTES_CPU] = b->bytes[DE_ASSET_CAT_CPU];
        st.assets_resident = b->assets;
        st.io_bytes_per_sec = b->io_rate;
    }
    st.loads_in_flight = stats_loads_in_flight(s);
    st.uploads_in_flight = 0; /* S11: the GPU upload stage */
    return st;
}
