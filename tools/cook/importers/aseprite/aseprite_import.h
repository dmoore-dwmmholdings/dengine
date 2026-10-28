#ifndef DE_ASEPRITE_IMPORT_H
#define DE_ASEPRITE_IMPORT_H
#include "atlas_pack.h"
#include <dengine/de_asset.h>

/* Emits the existing DSCP v2 geometry extension for one packed frame set. On failure
 * outputs are empty; the caller frees a successful blob with the explicit allocator. */
de_result de_aseprite_clip_encode(const de_aseprite_doc *doc, const de_aseprite_atlas *atlas,
                                  u32 set, de_asset_id texture, f32 pixels_per_meter,
                                  const char *pivot_slice, const de_allocator *allocator,
                                  void **data, u64 *bytes, const char **why);
de_result de_aseprite_import(de_asset_import_ctx *ctx);
de_asset_type_desc de_aseprite_import_type_desc(void);
#endif
