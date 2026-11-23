/*
 * DENG-271: streaming sources and world/bundle queries for the world partition
 * (src/world/streaming). See the "World streaming" section of de_asset.h.
 */
#include "asset/asset_internal.h"
#include "asset/manifest_internal.h"

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

de_result de_asset_world_info_get(const de_asset_system *s, const char *name,
                                  de_asset_world_info *out) {
    if (s == NULL || name == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_asset_world_info_get: NULL argument");
    memset(out, 0, sizeof *out);
    const de__manifest_world *w = de__manifest_find_world(s->manifest, name);
    if (w == NULL)
        return DE_FAIL(DE_ERR_NOT_FOUND, "asset manifest: no world \"%s\"", name);
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

u32 de_asset_bundle_count(const de_asset_system *s) {
    return s != NULL && s->manifest != NULL ? s->manifest->bundle_count : 0u;
}

const char *de_asset_bundle_name(const de_asset_system *s, u32 index) {
    if (s == NULL || s->manifest == NULL || index >= s->manifest->bundle_count)
        return NULL;
    return s->manifest->bundles[index].name;
}
