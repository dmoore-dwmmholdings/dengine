/*
 * DENG-271: streaming sources and world/bundle queries for the world partition
 * (src/world/streaming). See the "World streaming" section of de_asset.h.
 *
 * DENG-856: the bundle list is the merged list of the manifest and the mounted packs, so the
 * partition finds the cells and proxies of a PACKED game (the .dpak bundle tables) with the same
 * code as in SOURCE mode. The order follows de_asset_load_bundle: PACKED mode lists the packs
 * first (topmost first), SOURCE mode the manifest first; a name that both define is listed once,
 * where the source that serves it comes. The list is built on first use and dropped when the
 * manifest or the mount stack changes.
 */
#include "asset/asset_internal.h"
#include "asset/manifest_internal.h"
#include "asset/mount_internal.h"

#include <stdlib.h>
#include <string.h>

void de_asset_set_streaming_sources(de_asset_system *s, const de_streaming_source *src,
                                    u32 count) {
    if (s == NULL)
        return;
    if (src == NULL)
        count = 0;
    if (count > DE_ASSET_MAX_STREAMING_SOURCES)
        count = DE_ASSET_MAX_STREAMING_SOURCES;
    if (count != 0)
        memcpy(s->stream_sources, src, sizeof *src * count);
    s->stream_source_count = count;
}

u32 de_asset_get_streaming_sources(const de_asset_system *s, const de_streaming_source **out) {
    if (out != NULL)
        *out = s != NULL ? s->stream_sources : NULL;
    return s != NULL ? s->stream_source_count : 0u;
}

/* ---- Merged bundle names (DENG-856) -------------------------------------------------------- */

typedef struct bn_cand {
    const char *name;
    u32 rank; /* position in precedence order */
} bn_cand;

static int bn_cmp_name(const void *pa, const void *pb) {
    const bn_cand *a = (const bn_cand *)pa, *b = (const bn_cand *)pb;
    int c = strcmp(a->name, b->name);
    if (c != 0)
        return c;
    return a->rank < b->rank ? -1 : (a->rank > b->rank ? 1 : 0);
}

static int bn_cmp_rank(const void *pa, const void *pb) {
    const bn_cand *a = (const bn_cand *)pa, *b = (const bn_cand *)pb;
    return a->rank < b->rank ? -1 : (a->rank > b->rank ? 1 : 0);
}

void de__asset_bundle_names_invalidate(de_asset_system *s) {
    if (s == NULL)
        return;
    if (s->bundle_names != NULL)
        DE_FREE(s->alloc, s->bundle_names, sizeof *s->bundle_names * s->bundle_name_capacity);
    s->bundle_names = NULL;
    s->bundle_name_count = 0;
    s->bundle_name_capacity = 0;
    s->bundle_names_valid = false;
}

/* Builds the merged list. False on out of memory (the callers then fall back to the manifest
 * alone, the pre-DENG-856 behaviour, and try again next call). */
static bool bn_build(de_asset_system *s) {
    const de__manifest *m = s->manifest;
    u32 mc = m != NULL ? m->bundle_count : 0u;
    u32 pc = de__mount_bundle_names(s, NULL, 0);
    if (pc == 0) { /* manifest only: no copy of the strings needed beyond the pointers */
        if (mc != 0) {
            s->bundle_names = (const char **)DE_ALLOC(s->alloc, sizeof(const char *) * mc,
                                                      DE_ALIGNOF(const char *), DE_MEM_ASSET);
            if (s->bundle_names == NULL)
                return false;
            for (u32 i = 0; i < mc; ++i)
                s->bundle_names[i] = m->bundles[i].name;
        }
        s->bundle_name_count = s->bundle_name_capacity = mc;
        s->bundle_names_valid = true;
        return true;
    }
    u64 total = (u64)mc + pc;
    if (total > UINT32_MAX / sizeof(bn_cand))
        return false;
    const de_allocator *a = s->alloc;
    bn_cand *c = (bn_cand *)DE_ALLOC(a, sizeof *c * (size_t)total, DE_ALIGNOF(bn_cand),
                                     DE_MEM_ASSET);
    const char **pn = (const char **)DE_ALLOC(a, sizeof(const char *) * pc,
                                              DE_ALIGNOF(const char *), DE_MEM_ASSET);
    if (c == NULL || pn == NULL) {
        if (c != NULL)
            DE_FREE(a, c, sizeof *c * (size_t)total);
        if (pn != NULL)
            DE_FREE(a, pn, sizeof(const char *) * pc);
        return false;
    }
    (void)de__mount_bundle_names(s, pn, pc);
    bool packs_first = s->desc.mode == DE_ASSET_MODE_PACKED;
    u32 n = 0;
    for (u32 pass = 0; pass < 2; ++pass) {
        if ((pass == 0) == packs_first) {
            for (u32 i = 0; i < pc; ++i, ++n)
                c[n] = (bn_cand){pn[i], n};
        } else {
            for (u32 i = 0; i < mc; ++i, ++n)
                c[n] = (bn_cand){m->bundles[i].name, n};
        }
    }
    DE_FREE(a, pn, sizeof(const char *) * pc);
    /* Each name once, at its first (highest precedence) place. Sorting keeps the build
     * O(n log n): a 10 km world has thousands of cell and proxy bundles. */
    qsort(c, n, sizeof *c, bn_cmp_name);
    u32 u = 0;
    for (u32 i = 0; i < n; ++i)
        if (u == 0 || strcmp(c[u - 1].name, c[i].name) != 0)
            c[u++] = c[i];
    qsort(c, u, sizeof *c, bn_cmp_rank);
    const char **out = NULL;
    if (u != 0) {
        out = (const char **)DE_ALLOC(a, sizeof(const char *) * u, DE_ALIGNOF(const char *),
                                      DE_MEM_ASSET);
        if (out == NULL) {
            DE_FREE(a, c, sizeof *c * (size_t)total);
            return false;
        }
        for (u32 i = 0; i < u; ++i)
            out[i] = c[i].name;
    }
    DE_FREE(a, c, sizeof *c * (size_t)total);
    s->bundle_names = out;
    s->bundle_name_count = s->bundle_name_capacity = u;
    s->bundle_names_valid = true;
    return true;
}

/* The list is a cache of state the caller cannot see, so the const queries may build it. */
static bool bn_ensure(const de_asset_system *cs) {
    de_asset_system *s = (de_asset_system *)cs;
    return s->bundle_names_valid || bn_build(s);
}

u32 de_asset_bundle_count(const de_asset_system *s) {
    if (s == NULL)
        return 0u;
    if (bn_ensure(s))
        return s->bundle_name_count;
    return s->manifest != NULL ? s->manifest->bundle_count : 0u;
}

const char *de_asset_bundle_name(const de_asset_system *s, u32 index) {
    if (s == NULL)
        return NULL;
    if (bn_ensure(s))
        return index < s->bundle_name_count ? s->bundle_names[index] : NULL;
    if (s->manifest == NULL || index >= s->manifest->bundle_count)
        return NULL;
    return s->manifest->bundles[index].name;
}

/* ---- World info ------------------------------------------------------------------------------ */

/* True when a bundle of the list is a cell of world `name`: "world/<name>/cell_...". */
static bool packs_have_world(const de_asset_system *s, const char *name) {
    if (!de__mount_any(s))
        return false;
    size_t wl = strlen(name);
    u32 nb = de_asset_bundle_count(s);
    for (u32 i = 0; i < nb; ++i) {
        const char *bn = de_asset_bundle_name(s, i);
        if (bn != NULL && strncmp(bn, "world/", 6) == 0 && strncmp(bn + 6, name, wl) == 0 &&
            strncmp(bn + 6 + wl, "/cell_", 6) == 0)
            return true;
    }
    return false;
}

de_result de_asset_world_info_get(const de_asset_system *s, const char *name,
                                  de_asset_world_info *out) {
    if (s == NULL || name == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_asset_world_info_get: NULL argument");
    memset(out, 0, sizeof *out);
    const de__manifest_world *w = de__manifest_find_world(s->manifest, name);
    if (w == NULL) {
        /* DENG-856: a PACKED game without the world in a manifest. The pack has no [worlds]
         * table, so the settings are the manifest defaults. */
        size_t len = strlen(name);
        if (len == 0 || len >= DE__ASSET_PACK_WORLD_NAME_MAX || !packs_have_world(s, name))
            return DE_FAIL(DE_ERR_NOT_FOUND, "asset manifest: no world \"%s\"", name);
        de__manifest_world d;
        memset(&d, 0, sizeof d);
        de__manifest_world_defaults(&d);
        de_asset_system *ms = (de_asset_system *)s;
        memcpy(ms->pack_world_name, name, len + 1);
        out->name = ms->pack_world_name;
        out->cells_dir = d.cells_dir;
        out->cell_size = d.cell_size;
        out->load_radius = d.load_radius;
        out->unload_radius = d.unload_radius;
        out->max_loads_per_frame = d.max_loads_per_frame;
        out->scene_count = 0;
        out->proxy_enabled = d.proxy_enabled;
        out->proxy_triangle_ratio = d.proxy_triangle_ratio;
        out->proxy_fade_seconds = d.proxy_fade_seconds;
        return DE_OK;
    }
    out->name = w->name;
    out->cells_dir = w->cells_dir;
    out->cell_size = w->cell_size;
    out->load_radius = w->load_radius;
    out->unload_radius = w->unload_radius;
    out->max_loads_per_frame = w->max_loads_per_frame;
    out->scene_count = w->scene_count;
    out->proxy_enabled = w->proxy_enabled;
    out->proxy_triangle_ratio = w->proxy_triangle_ratio;
    out->proxy_fade_seconds = w->proxy_fade_seconds;
    return DE_OK;
}
