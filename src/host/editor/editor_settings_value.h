#ifndef DE_EDITOR_SETTINGS_VALUE_H
#define DE_EDITOR_SETTINGS_VALUE_H
#include "asset/manifest_internal.h"
#include <de_editor_protocol.h>
/* Bounded TOML-compatible MessagePack conversion into caller-owned arena; no borrowed values. */
de_result de__ed_settings_values(const de_ep_map_raw *input, de_ep_arena *arena,
                                 de__manifest_kv **settings);
#endif
