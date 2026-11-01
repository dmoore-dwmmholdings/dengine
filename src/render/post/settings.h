#ifndef DE_RENDER_POST_SETTINGS_H
#define DE_RENDER_POST_SETTINGS_H

#include <dengine/de_render.h>

/** Pure normalization shared by setters and immutable per-view snapshots. */
de_post_settings de__post_settings_resolve(const de_post_settings *settings);
de_hdr_settings de__hdr_settings_resolve(const de_hdr_settings *settings);

#endif
