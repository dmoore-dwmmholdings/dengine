#ifndef DE_EDITOR_SETTINGS_IMAGE_H
#define DE_EDITOR_SETTINGS_IMAGE_H
#include <dengine/de_base.h>
/* Mip0/layer0 pixels from actual cooked DTEX bytes, nearest sampled to 1..256 dimensions.
 * Colour textures are presented in sRGB; data channels stay raw, HDR is clamped to [0,1]. */
de_result de__ed_settings_pixels(const void *data, u64 bytes, u32 width, u32 height, u8 *rgba,
                                 u32 *source_width, u32 *source_height, const char **format);
#endif
