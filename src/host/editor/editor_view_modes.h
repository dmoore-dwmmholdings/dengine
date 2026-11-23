/* Editor-facing names for implemented per-view renderer diagnostics (DENG-743).
 * Pure mapping: discovery and every view request use the same exact string spans. */
#ifndef DE_HOST_EDITOR_VIEW_MODES_H
#define DE_HOST_EDITOR_VIEW_MODES_H

#include <dengine/de_render.h>

#include <stddef.h>
#include <string.h>

/* Order follows the existing public enum, including DENG-697's appended geometry modes.
 * Scene meshletIds matches the established toolbar ID; asset previews use their own meshlets
 * option. A new renderer enum must get an explicit service name before this table can drift. */
static const char *const de__ed_view_render_names[] = {
    "lit",      "unlit",         "baseColor", "normals",   "roughness",  "metallic", "ao",
    "emissive", "motionVectors", "depth",     "wireframe", "meshletIds", "uv0",      "uv1",
};
_Static_assert(sizeof de__ed_view_render_names / sizeof de__ed_view_render_names[0] ==
                   DE_VIEW_RENDER_MODE_COUNT,
               "editor render-mode names must cover the implemented renderer enum");

static inline bool de__ed_view_render_mode_parse(const char *name, size_t len,
                                                 de_view_render_mode *out) {
    if (out == NULL)
        return false;
    if (len == 0) {
        *out = DE_VIEW_RENDER_LIT;
        return true;
    }
    if (name == NULL)
        return false;
    for (u32 i = 0; i < DE_VIEW_RENDER_MODE_COUNT; ++i) {
        const char *candidate = de__ed_view_render_names[i];
        if (strlen(candidate) == len && memcmp(name, candidate, len) == 0) {
            *out = (de_view_render_mode)i;
            return true;
        }
    }
    return false;
}

/* Game views share the runtime output: only Scene views may select diagnostics.
 * Validate before assigning so a rejected override never replaces the caller's mode. */
static inline bool de__ed_view_render_mode_request(const char *name, size_t len, bool game,
                                                   de_view_render_mode *out) {
    de_view_render_mode mode;
    if (out == NULL || !de__ed_view_render_mode_parse(name, len, &mode) ||
        (game && mode != DE_VIEW_RENDER_LIT))
        return false;
    *out = mode;
    return true;
}

#endif /* DE_HOST_EDITOR_VIEW_MODES_H */
